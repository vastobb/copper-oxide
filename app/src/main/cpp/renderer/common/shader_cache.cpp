#include "shader_cache.h"

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "CopperOxide-Cache"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// WHY every constant of this file is a compile time constant in an anonymous
// namespace: the key stream and the file layout are a wire format. Silently
// changing one of them would let a stale artifact be served under a key that
// claims to describe it, so both are versioned and both live here where they
// are impossible to override from a header.

// Both the entry files and the in-memory keys are little endian, as is every
// Android device this ships on. Refuse to build elsewhere rather than write a
// file that a different build cannot read back.
static_assert(std::endian::native == std::endian::little,
              "the shader cache file format is little endian");

// ---- key -------------------------------------------------------------------

// "COK1" as an integer. It seeds the key stream so that a digest is only ever
// produced by buildKey() itself, never by something that happens to hash the
// same bytes.
constexpr uint32_t k_key_magic = 0x434F4B31u;
// Bump when a field is appended to the stream in build_key(). Old and new
// digests then cannot collide while meaning different things.
constexpr uint32_t k_key_version = 1u;

constexpr uint64_t k_fnv_offset_basis = 0xcbf29ce484222325ull;
constexpr uint64_t k_fnv_prime = 0x100000001b3ull;

constexpr size_t k_key_hex_length = 16u;

void put_u32_le(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void put_u64_le(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFull);
    }
}

uint32_t get_u32_le(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t get_u64_le(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

/// FNV-1a, 64 bit. Chosen because it is a fixed, published algorithm with no
/// library or platform dependency: the same source must produce the same key
/// on every device and in every process, forever, because the key is a
/// filename. std::hash cannot be used for this (it is implementation defined,
/// and libstdc++ and libc++ do not agree), and neither can a hash of a
/// std::string's internals. It is not a security hash and is only ever used for
/// keys and for catching torn files.
class Fnv1a64 {
public:
    void feed(const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            state_ ^= static_cast<uint64_t>(bytes[i]);
            state_ *= k_fnv_prime;
        }
    }

    /// Every field of the key stream is length prefixed. That is what makes the
    /// stream self delimiting: a later field can be appended without moving a
    /// byte of an earlier one (so keys stay stable across versions), and a
    /// define can never be forged by sliding bytes out of the source, because
    /// "FOO" + "1" and "FO" + "O1" produce different record streams.
    void feed_field(const void* data, size_t size) {
        uint8_t length[8];
        put_u64_le(length, static_cast<uint64_t>(size));
        feed(length, sizeof(length));
        if (size != 0) {
            feed(data, size);
        }
    }

    void feed_u8(uint8_t value) { feed_field(&value, sizeof(value)); }

    void feed_u32(uint32_t value) {
        uint8_t bytes[4];
        put_u32_le(bytes, value);
        feed_field(bytes, sizeof(bytes));
    }

    uint64_t value() const { return state_; }

private:
    uint64_t state_ = k_fnv_offset_basis;
};

std::string to_hex16(uint64_t value) {
    static const char k_digits[] = "0123456789abcdef";
    std::string out(k_key_hex_length, '0');
    for (size_t i = 0; i < k_key_hex_length; ++i) {
        const size_t shift = (k_key_hex_length - 1 - i) * 4;
        out[i] = k_digits[(value >> shift) & 0xFull];
    }
    return out;
}

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/// True when the 16 characters at `offset` are lowercase hex.
///
/// This is the security boundary of the whole class: keys reach the filesystem
/// as path components, and 16 lowercase hex digits cannot contain a separator,
/// a dot or anything else. A key that fails this test can never be turned into
/// a path, which is why lookup() and store() check it before touching the disk
/// instead of sanitising it.
bool is_key_hex(const std::string& text, size_t offset) {
    if (offset + k_key_hex_length > text.size()) {
        return false;
    }
    for (size_t i = 0; i < k_key_hex_length; ++i) {
        if (!is_hex_digit(text[offset + i])) {
            return false;
        }
    }
    return true;
}

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::string trim_ascii(const std::string& text) {
    size_t first = 0;
    size_t last = text.size();
    while (first < last && is_ascii_space(text[first])) {
        ++first;
    }
    while (last > first && is_ascii_space(text[last - 1])) {
        --last;
    }
    return text.substr(first, last - first);
}

/// UNSIGNED bytewise order, spelled out rather than left to std::string's
/// operator< so that the sort cannot become locale or char signedness
/// dependent: the key has to be identical on every device.
bool bytewise_less(const std::string& a, const std::string& b) {
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        const unsigned char left = static_cast<unsigned char>(a[i]);
        const unsigned char right = static_cast<unsigned char>(b[i]);
        if (left != right) {
            return left < right;
        }
    }
    return a.size() < b.size();
}

std::vector<std::string> normalise_defines(const std::vector<std::string>& defines) {
    std::vector<std::string> normalised;
    normalised.reserve(defines.size());
    for (const std::string& define : defines) {
        std::string trimmed = trim_ascii(define);
        if (!trimmed.empty()) {
            normalised.push_back(std::move(trimmed));
        }
    }
    std::sort(normalised.begin(), normalised.end(), bytewise_less);
    normalised.erase(std::unique(normalised.begin(), normalised.end()), normalised.end());
    return normalised;
}

std::string build_key(ShaderStage stage,
                      const std::string& source,
                      const std::vector<std::string>& defines,
                      const std::string& target_api,
                      uint32_t toolchain_version) {
    // WHY normalising happens here and nowhere else: this function is the
    // cache's entire notion of identity, so two call sites that pass the same
    // defines in a different order, with a stray tab, must not compile twice.
    const std::vector<std::string> normalised = normalise_defines(defines);

    Fnv1a64 hash;
    hash.feed_u32(k_key_magic);
    hash.feed_u32(k_key_version);
    hash.feed_u8(static_cast<uint8_t>(stage));
    // Length prefixed, so trailing whitespace or an embedded NUL in the source
    // is part of the key rather than invisible.
    hash.feed_field(source.data(), source.size());
    for (const std::string& define : normalised) {
        hash.feed_field(define.data(), define.size());
    }
    hash.feed_field(target_api.data(), target_api.size());
    // The last field on purpose: anything that changes the compiler's output
    // belongs here, and putting it last means an older build that lacks it can
    // never accidentally produce the same digest as a newer one.
    hash.feed_u32(toolchain_version);
    return to_hex16(hash.value());
}

// ---- SPIR-V ---------------------------------------------------------------

constexpr uint32_t k_spirv_magic = 0x07230203u;

/// Cheap structural check, deliberately NOT validation. It answers "is this a
/// plausible SPIR-V byte stream", not "is this a well formed module": full
/// validation is SPIRV-Tools' job and belongs where the words are handed to
/// the driver. What this does buy is that neither store() nor a disk hit can
/// hand back GLSL text, a truncated module or an empty vector.
bool is_structurally_valid(const std::vector<uint32_t>& spirv) {
    return !spirv.empty() && spirv.front() == k_spirv_magic;
}

// ---- on-disk entry ---------------------------------------------------------

// "COSH" as an integer: tells a cache entry apart from anything else that
// happens to share the directory.
constexpr uint32_t k_disk_magic = 0x434F5348u;
constexpr uint32_t k_disk_version = 1u;

// Fixed header, little endian:
//   0  u32  magic            k_disk_magic ("COSH")
//   4  u32  format_version   k_disk_version, bumped on any layout change
//   8  u32  header_size      44 today; lets a future version grow the header
//                            without moving the payload and without the reader
//                            having to know the new fields
//  12  u32  stage            ShaderStage, informational: the key already
//                            encodes the stage, this is there so a mismatch or
//                            a truncated file is detectable
//  16  16B  key              the 16 hex digits, so a renamed or copied file is
//                            rejected instead of served
//  32  u32  word_count       SPIR-V words in the payload
//  36  u64  payload_checksum FNV-1a 64 over the payload bytes
//  44  ...  payload          word_count * 4 bytes
constexpr size_t k_disk_header_size = 44u;
constexpr size_t k_key_offset = 16u;
constexpr size_t k_word_count_offset = 32u;
constexpr size_t k_checksum_offset = 36u;
constexpr size_t k_stage_offset = 12u;

constexpr uint32_t k_max_shader_stage = 13u;  // ShaderStage::Callable

// A cached module is a shader, not an asset bundle. Anything larger is either
// corrupt or not ours, and is refused before it is read into memory.
constexpr uint64_t k_max_entry_bytes = 64ull * 1024ull * 1024ull;

// Naming convention. Entries are "<16 hex>.spv"; temporaries are
// ".<16 hex>.<pid>.<serial>.tmp" so that the directory scan can never mistake a
// half written file for an entry, and so the leading dot keeps them out of a
// casual listing.
constexpr char k_entry_suffix[] = ".spv";
constexpr size_t k_entry_suffix_length = sizeof(k_entry_suffix) - 1u;
constexpr char k_temp_suffix[] = ".tmp";
constexpr size_t k_temp_suffix_length = sizeof(k_temp_suffix) - 1u;
// One 32 bit word count field, so one module cannot address more than this.
constexpr uint64_t k_max_word_count = 0xFFFFFFFFull;

std::string join_path(const std::string& directory, const std::string& leaf) {
    if (directory.empty()) {
        return leaf;
    }
    if (directory.back() == '/') {
        return directory + leaf;
    }
    return directory + "/" + leaf;
}

std::string entry_path(const std::string& directory, const std::string& key) {
    return join_path(directory, key + k_entry_suffix);
}

/// Unique per store() attempt. Two threads can store the same key at once (the
/// file I/O deliberately runs outside the lock), so a fixed "<key>.tmp" would
/// let two writers interleave and publish a spliced file. The pid is in the
/// name so that only this process's leftovers can be reaped later.
std::string temp_file_name(const std::string& key, const std::string& pid, uint32_t serial) {
    return std::string(1, '.') + key + "." + pid + "." + std::to_string(serial) + k_temp_suffix;
}

uint32_t next_temp_serial() {
    static std::atomic<uint32_t> serial{0};
    return serial.fetch_add(1, std::memory_order_relaxed);
}

/// True for "<16 hex>.spv" and nothing else. Trimming and clear() go through
/// this, which is what guarantees they cannot delete a file the app put there.
bool is_entry_name(const std::string& name) {
    if (name.size() != k_key_hex_length + k_entry_suffix_length || !is_key_hex(name, 0)) {
        return false;
    }
    return name.compare(k_key_hex_length, k_entry_suffix_length, k_entry_suffix) == 0;
}

/// True for a temporary this process left behind after a store() that never
/// reached its rename(). Reaping only our own pid is the conservative choice:
/// another live process may be halfway through writing its own temporary for
/// the same key, and deleting that would turn its rename() into an error.
///
/// The accepted shape is "." + 16 hex + "." + pid + "." + >=1 digit + ".tmp".
bool is_our_temp_name(const std::string& name, const std::string& pid) {
    const size_t shortest = 1u /*'.'*/ + k_key_hex_length + 1u /*'.'*/ + pid.size() +
                            1u /*'.'*/ + 1u /*one serial digit*/ + k_temp_suffix_length;
    if (name.size() < shortest) {
        return false;
    }
    if (name[0] != '.' || !is_key_hex(name, 1) || name[1u + k_key_hex_length] != '.') {
        return false;
    }
    size_t index = 2u + k_key_hex_length;
    if (name.compare(index, pid.size(), pid) != 0 || name[index + pid.size()] != '.') {
        return false;
    }
    index += pid.size() + 1u;
    size_t digits = 0;
    while (index + digits < name.size() && name[index + digits] >= '0' &&
           name[index + digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    index += digits;
    return name.size() - index == k_temp_suffix_length &&
           name.compare(index, k_temp_suffix_length, k_temp_suffix) == 0;
}

struct DiskEntryInfo {
    std::string name;
    std::string path;
    uint64_t bytes = 0;
    std::timespec modified{};
};

/// Lists the cache's own entries. Returns false when the directory cannot be
/// opened at all, which is the only case where a caller should say so rather
/// than report an empty directory.
bool scan_disk(const std::string& directory, std::vector<DiskEntryInfo>* out) {
    DIR* handle = ::opendir(directory.c_str());
    if (handle == nullptr) {
        return false;
    }
    while (const dirent* item = ::readdir(handle)) {
        const std::string name(item->d_name);
        if (!is_entry_name(name)) {
            continue;
        }
        const std::string path = join_path(directory, name);
        struct stat info{};
        // WHY lstat and not stat: a symlink dropped into the cache directory
        // must not be able to pull a file from outside it into the byte count
        // or into the set of paths clear() unlinks.
        if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        DiskEntryInfo entry;
        entry.name = name;
        entry.path = path;
        entry.bytes = static_cast<uint64_t>(info.st_size);
        entry.modified = info.st_mtim;
        out->push_back(std::move(entry));
    }
    ::closedir(handle);
    return true;
}

/// Deletes the oldest entries until the directory fits in `max_bytes`; returns
/// how many files went. Only files matching the naming convention are counted
/// or removed. Called with no lock held: it does syscalls.
size_t trim_disk(const std::string& directory, uint64_t max_bytes) {
    std::vector<DiskEntryInfo> entries;
    if (!scan_disk(directory, &entries)) {
        return 0;
    }
    uint64_t total = 0;
    for (const DiskEntryInfo& entry : entries) {
        total += entry.bytes;
    }
    if (total <= max_bytes) {
        return 0;
    }
    // Oldest first. The name breaks ties so that two files stamped in the same
    // clock tick go in the same order on every run.
    std::sort(entries.begin(), entries.end(),
              [](const DiskEntryInfo& a, const DiskEntryInfo& b) {
                  if (a.modified.tv_sec != b.modified.tv_sec) {
                      return a.modified.tv_sec < b.modified.tv_sec;
                  }
                  if (a.modified.tv_nsec != b.modified.tv_nsec) {
                      return a.modified.tv_nsec < b.modified.tv_nsec;
                  }
                  return a.name < b.name;
              });

    size_t removed = 0;
    for (const DiskEntryInfo& entry : entries) {
        if (total <= max_bytes) {
            break;
        }
        if (::unlink(entry.path.c_str()) != 0) {
            LOGW("trim: unlink(%s) failed: %s", entry.name.c_str(), std::strerror(errno));
            continue;
        }
        total -= entry.bytes;
        ++removed;
    }
    return removed;
}

/// mkdir -p. A caller normally passes "<filesDir>/shadercache" on a device
/// where "<filesDir>" does not exist yet, and a single mkdir() would then fail
/// for a reason that has nothing to do with the cache.
bool make_directories(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string built;
    size_t index = 0;
    if (path[0] == '/') {
        built = "/";
        index = 1;
    }
    while (index <= path.size()) {
        const size_t slash = path.find('/', index);
        const size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > index) {
            built.append(path, index, end - index);
            if (::mkdir(built.c_str(), 0770) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        index = slash + 1;
    }
    // A pre-existing regular file is not a usable directory either, and mkdir
    // reports EEXIST for it, so the only reliable check is to stat the result.
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool write_all(int fd, const void* data, size_t size) {
    // read() and write() may move fewer bytes than asked and may be interrupted
    // at any point, so both loops are mandatory rather than defensive.
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t moved = 0;
    while (moved < size) {
        const ssize_t written = ::write(fd, bytes + moved, size - moved);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        moved += static_cast<size_t>(written);
    }
    return true;
}

enum class ReadResult {
    Ok,
    Eof,     // the file ended early: it is shorter than its own header claims
    Failed,  // permissions or I/O: nothing is known about the bytes
};

ReadResult read_all(int fd, void* data, size_t size) {
    uint8_t* bytes = static_cast<uint8_t*>(data);
    size_t moved = 0;
    while (moved < size) {
        const ssize_t got = ::read(fd, bytes + moved, size - moved);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReadResult::Failed;
        }
        if (got == 0) {
            return ReadResult::Eof;
        }
        moved += static_cast<size_t>(got);
    }
    return ReadResult::Ok;
}

enum class EntryStatus {
    Ok,
    NotFound,    // nothing there: an ordinary miss, not an error
    Corrupt,     // the bytes cannot be trusted: reject and delete the file
    Unreadable,  // permissions or I/O: reject but keep the bytes
};

EntryStatus read_entry(const std::string& directory,
                       const std::string& key,
                       std::vector<uint32_t>* spirv,
                       std::string* reason) {
    const std::string path = entry_path(directory, key);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return EntryStatus::NotFound;
        }
        *reason = std::string("open: ") + std::strerror(errno);
        return EntryStatus::Unreadable;
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        *reason = std::string("fstat: ") + std::strerror(errno);
        ::close(fd);
        return EntryStatus::Unreadable;
    }
    if (!S_ISREG(info.st_mode)) {
        *reason = "not a regular file";
        ::close(fd);
        return EntryStatus::Unreadable;
    }

    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size < k_disk_header_size) {
        *reason = "shorter than a header";
        ::close(fd);
        return EntryStatus::Corrupt;
    }
    if (size > k_max_entry_bytes) {
        *reason = "implausible size " + std::to_string(size);
        ::close(fd);
        return EntryStatus::Corrupt;
    }

    // Read the whole entry, then close, then parse: one exit path for the
    // descriptor and no chance of forgetting to close it on a reject.
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const ReadResult read = read_all(fd, bytes.data(), bytes.size());
    if (read != ReadResult::Ok) {
        // WHY a short read is corrupt and not unreadable: the file announced
        // this size through its own stat() and then delivered less, so its
        // bytes are not trustworthy, and keeping a truncated module around
        // only means paying this cost again on every load.
        *reason = (read == ReadResult::Eof) ? "file shrank while being read"
                                            : std::string("read: ") + std::strerror(errno);
        ::close(fd);
        return read == ReadResult::Eof ? EntryStatus::Corrupt : EntryStatus::Unreadable;
    }
    ::close(fd);

    if (get_u32_le(bytes.data()) != k_disk_magic) {
        *reason = "magic mismatch";
        return EntryStatus::Corrupt;
    }
    const uint32_t version = get_u32_le(bytes.data() + 4);
    if (version != k_disk_version) {
        // Written by a build whose layout this one does not understand. Its
        // bytes are not ours to interpret, and guessing would be how a future
        // format gets silently misread as today's.
        *reason = "unknown format version " + std::to_string(version);
        return EntryStatus::Corrupt;
    }
    // header_size may be larger than this reader knows; the fields needed to
    // reach the payload are all inside the first k_disk_header_size bytes, so
    // the payload offset is read from the file instead of assumed.
    const uint32_t header_size = get_u32_le(bytes.data() + 8);
    if (header_size < k_disk_header_size || header_size > size) {
        *reason = "bad header size " + std::to_string(header_size);
        return EntryStatus::Corrupt;
    }
    const uint32_t stage = get_u32_le(bytes.data() + k_stage_offset);
    if (stage > k_max_shader_stage) {
        *reason = "stage " + std::to_string(stage) + " is not a ShaderStage";
        return EntryStatus::Corrupt;
    }
    if (std::memcmp(bytes.data() + k_key_offset, key.data(), k_key_hex_length) != 0) {
        // Someone renamed or copied a file over this key. Serving it would
        // return the wrong shader under a key that denies it.
        *reason = "key mismatch";
        return EntryStatus::Corrupt;
    }

    const uint32_t word_count = get_u32_le(bytes.data() + k_word_count_offset);
    if (word_count == 0) {
        *reason = "zero words";
        return EntryStatus::Corrupt;
    }
    // 64 bit arithmetic: word_count * 4 must not wrap.
    const uint64_t payload_bytes = static_cast<uint64_t>(word_count) * 4ull;
    if (size != static_cast<uint64_t>(header_size) + payload_bytes) {
        *reason = "file size " + std::to_string(size) + " disagrees with " +
                  std::to_string(word_count) + " words";
        return EntryStatus::Corrupt;
    }

    const uint8_t* payload = bytes.data() + header_size;
    Fnv1a64 hash;
    hash.feed(payload, static_cast<size_t>(payload_bytes));
    if (hash.value() != get_u64_le(bytes.data() + k_checksum_offset)) {
        // The header survived but the payload did not: exactly what a torn
        // write or a truncated file looks like.
        *reason = "checksum mismatch";
        return EntryStatus::Corrupt;
    }

    spirv->resize(word_count);
    std::memcpy(spirv->data(), payload, static_cast<size_t>(payload_bytes));
    if (!is_structurally_valid(*spirv)) {
        *reason = "payload is not SPIR-V";
        return EntryStatus::Corrupt;
    }
    return EntryStatus::Ok;
}

/// Writes one entry atomically: temporary file in the same directory, fsync,
/// then rename() over the final name. rename() is atomic on POSIX, so the entry
/// either does not exist or is complete; a crash mid-write can only leave a
/// temporary file behind, which is not an entry and is reaped on the next
/// open(). `reason` is filled for the log, never for control flow.
bool write_entry(const std::string& directory,
                 const std::string& key,
                 ShaderStage stage,
                 const std::vector<uint32_t>& spirv,
                 std::string* reason) {
    if (static_cast<uint64_t>(spirv.size()) > k_max_word_count) {
        *reason = "module is larger than the word count field can address";
        return false;
    }
    const size_t payload_bytes = spirv.size() * sizeof(uint32_t);

    Fnv1a64 hash;
    hash.feed(spirv.data(), payload_bytes);

    uint8_t header[k_disk_header_size];
    put_u32_le(header, k_disk_magic);
    put_u32_le(header + 4, k_disk_version);
    put_u32_le(header + 8, static_cast<uint32_t>(k_disk_header_size));
    put_u32_le(header + k_stage_offset, static_cast<uint32_t>(stage));
    std::memcpy(header + k_key_offset, key.data(), k_key_hex_length);
    put_u32_le(header + k_word_count_offset, static_cast<uint32_t>(spirv.size()));
    put_u64_le(header + k_checksum_offset, hash.value());

    const std::string pid = std::to_string(static_cast<long long>(::getpid()));
    std::string temp_path;
    int fd = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        temp_path = join_path(directory, temp_file_name(key, pid, next_temp_serial()));
        fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        *reason = std::string("create temporary: ") + std::strerror(errno);
        return false;
    }

    bool ok = write_all(fd, header, sizeof(header)) &&
              write_all(fd, spirv.data(), payload_bytes);
    if (ok && ::fsync(fd) != 0) {
        *reason = std::string("fsync: ") + std::strerror(errno);
        ok = false;
    }
    // WHY close()'s return value is ignored: everything that mattered was
    // already fsync()ed, and a close() that reports EINTR has already released
    // the descriptor, so retrying would risk closing an unrelated fd.
    ::close(fd);

    if (ok && ::rename(temp_path.c_str(), entry_path(directory, key).c_str()) != 0) {
        *reason = std::string("rename: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        // Never leave a half written temporary lying around; the entry itself
        // was never touched.
        ::unlink(temp_path.c_str());
        return false;
    }

    // The bytes are durable, but the directory entry naming them is not until
    // the directory is synced too. Best effort by design: losing the newest
    // entry to a power cut costs one recompile, which is cheaper than holding
    // a directory descriptor for the lifetime of the process.
    const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory_fd >= 0) {
        ::fsync(directory_fd);
        ::close(directory_fd);
    }
    return true;
}

/// Creates and removes one file to prove the directory really is writable.
/// Permissions and a read-only mount both look fine to a stat() and only show
/// up on a write, and discovering that on every store() is worse than finding
/// it once here.
bool probe_writable(const std::string& directory) {
    const std::string pid = std::to_string(static_cast<long long>(::getpid()));
    // WHY the probe is named like a temporary and not like an entry: if this
    // process dies between the create and the unlink below, what is left is a
    // file that the reaper in open() already knows how to recognise, and never
    // something that a scan would read as a module.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string path =
                join_path(directory, temp_file_name(std::string(k_key_hex_length, '0'), pid,
                                                   next_temp_serial()));
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            // EEXIST here means a leftover from a previous run of a process
            // that had this pid, not a broken directory, so try the next serial
            // before giving up and throwing away the disk cache.
            if (errno == EEXIST) {
                continue;
            }
            return false;
        }
        ::close(fd);
        ::unlink(path.c_str());
        return true;
    }
    return false;
}

} // namespace

class ShaderCache::Impl {
public:
    // WHY a list plus an index instead of a plain map with a tick counter:
    // promotion on a hit and eviction of the victim both become O(1), and there
    // is no scan of the whole map to find the oldest entry.
    using LruList = std::list<std::pair<std::string, std::vector<uint32_t>>>;

    bool open(const std::string& directory) {
        if (directory.empty()) {
            std::lock_guard<std::mutex> lock(mutex);
            disk_directory.clear();
            LOGI("open(\"\"): memory only");
            return true;
        }

        if (!make_directories(directory) || !probe_writable(directory)) {
            // Fall back rather than keep a path that every store() would retry
            // and fail: an unusable cache directory must cost one wasted call
            // per open(), not one per shader for the whole session.
            {
                std::lock_guard<std::mutex> lock(mutex);
                disk_directory.clear();
            }
            LOGW("open(%s) failed: not usable as a cache directory (%s); memory only",
                 directory.c_str(), std::strerror(errno));
            return false;
        }

        uint64_t budget = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            disk_directory = directory;
            budget = max_disk_bytes;
        }

        const std::string pid = std::to_string(static_cast<long long>(::getpid()));
        reap_temps(directory, pid);

        // The directory may have been left over budget by a build that ran with
        // a larger cap; trim before reporting what is in there.
        const size_t removed = trim_disk(directory, budget);

        std::vector<DiskEntryInfo> entries;
        uint64_t bytes = 0;
        if (scan_disk(directory, &entries)) {
            for (const DiskEntryInfo& entry : entries) {
                bytes += entry.bytes;
            }
        }
        LOGI("open(%s): %zu entries, %llu bytes, budget %llu bytes%s",
             directory.c_str(), entries.size(),
             static_cast<unsigned long long>(bytes),
             static_cast<unsigned long long>(budget),
             removed == 0 ? "" : " (trimmed on open)");
        return true;
    }

    bool lookup(const std::string& key, std::vector<uint32_t>* spirv) {
        if (spirv == nullptr) {
            LOGE("lookup: null output pointer");
            return false;
        }
        if (!is_key_hex(key, 0)) {
            LOGE("lookup: rejected malformed key '%s'", key.c_str());
            count_error();
            count_miss();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = index.find(key);
            if (it != index.end()) {
                // A hit promotes: splice under the lock, since it mutates the
                // order, and copy out under the same lock so the caller can
                // never observe a half promoted entry.
                lru.splice(lru.begin(), lru, it->second);
                *spirv = it->second->second;
                ++hits;
                return true;
            }
        }

        // Snapshot the directory, then do the I/O with the mutex released: a
        // missing file costs a readdir-less open() but a slow one must not
        // stall a store() on another thread.
        std::string directory;
        {
            std::lock_guard<std::mutex> lock(mutex);
            directory = disk_directory;
        }
        if (directory.empty()) {
            count_miss();
            return false;
        }

        std::vector<uint32_t> words;
        std::string reason;
        const EntryStatus status = read_entry(directory, key, &words, &reason);
        switch (status) {
            case EntryStatus::Ok:
                break;
            case EntryStatus::NotFound:
                count_miss();
                return false;
            case EntryStatus::Unreadable:
                // Deliberately no unlink: EACCES and EIO say nothing about
                // whether the bytes are good, and a full filesystem must not be
                // allowed to destroy a module that was fine a moment ago.
                LOGW("lookup(%s): kept unreadable entry: %s", key.c_str(), reason.c_str());
                count_error();
                count_miss();
                return false;
            case EntryStatus::Corrupt:
                LOGW("lookup(%s): discarding entry: %s", key.c_str(), reason.c_str());
                if (::unlink(entry_path(directory, key).c_str()) != 0 && errno != ENOENT) {
                    LOGW("lookup(%s): unlink failed: %s", key.c_str(), std::strerror(errno));
                }
                count_error();
                count_miss();
                return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            insert_locked(key, words);
            ++hits;
        }
        *spirv = std::move(words);
        return true;
    }

    bool store(const std::string& key, ShaderStage stage, const std::vector<uint32_t>& spirv) {
        if (!is_key_hex(key, 0)) {
            LOGE("store: rejected malformed key '%s'", key.c_str());
            count_error();
            return false;
        }
        if (!is_structurally_valid(spirv)) {
            // Refusing here rather than caching and letting vkCreateShaderModule
            // fail later: an entry this cache made can only ever be as good as
            // what was put into it.
            LOGE("store(%s): payload is not a SPIR-V module (%zu words, first word 0x%08x)",
                 key.c_str(), spirv.size(), spirv.empty() ? 0u : spirv.front());
            count_error();
            return false;
        }

        std::string directory;
        uint64_t budget = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            insert_locked(key, spirv);
            ++stores;
            directory = disk_directory;
            budget = max_disk_bytes;
        }
        if (directory.empty()) {
            return true;  // memory only is a complete success
        }

        std::string reason;
        if (!write_entry(directory, key, stage, spirv, &reason)) {
            LOGW("store(%s): disk write failed (%s); cached in memory only", key.c_str(),
                 reason.c_str());
            count_error();
            return false;
        }
        trim_disk(directory, budget);
        return true;
    }

    void set_max_memory_entries(size_t max_entries) {
        std::lock_guard<std::mutex> lock(mutex);
        max_memory_entries = max_entries;
        // Apply the new bound now rather than at the next insert, so a lowered
        // cap is immediately visible in memoryEntryCount().
        evict_to_locked(max_entries);
    }

    void set_max_disk_bytes(uint64_t max_bytes) {
        std::string directory;
        {
            std::lock_guard<std::mutex> lock(mutex);
            max_disk_bytes = max_bytes;
            directory = disk_directory;
        }
        if (directory.empty()) {
            return;
        }
        const size_t removed = trim_disk(directory, max_bytes);
        if (removed != 0) {
            LOGI("set_max_disk_bytes(%llu): trimmed %zu entries",
                 static_cast<unsigned long long>(max_bytes), removed);
        }
    }

    void clear() {
        std::string directory;
        {
            std::lock_guard<std::mutex> lock(mutex);
            lru.clear();
            index.clear();
            hits = 0;
            misses = 0;
            stores = 0;
            errors = 0;
            directory = disk_directory;
        }
        if (directory.empty()) {
            return;
        }
        std::vector<DiskEntryInfo> entries;
        if (!scan_disk(directory, &entries)) {
            LOGW("clear: %s could not be read: %s", directory.c_str(), std::strerror(errno));
            count_error();
            return;
        }
        size_t removed = 0;
        for (const DiskEntryInfo& entry : entries) {
            if (::unlink(entry.path.c_str()) == 0) {
                ++removed;
            } else if (errno != ENOENT) {
                LOGW("clear: unlink(%s) failed: %s", entry.name.c_str(), std::strerror(errno));
                count_error();
            }
        }
        LOGI("clear: %zu entries removed from %s", removed, directory.c_str());
    }

    uint64_t hit_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return hits;
    }

    uint64_t miss_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return misses;
    }

    uint64_t store_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stores;
    }

    uint64_t error_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return errors;
    }

    size_t memory_entry_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return lru.size();
    }

    size_t disk_entry_count() const {
        std::vector<DiskEntryInfo> entries;
        if (!scan_disk(snapshot_directory(), &entries)) {
            return 0;
        }
        return entries.size();
    }

    uint64_t disk_byte_count() const {
        std::vector<DiskEntryInfo> entries;
        if (!scan_disk(snapshot_directory(), &entries)) {
            return 0;
        }
        uint64_t total = 0;
        for (const DiskEntryInfo& entry : entries) {
            total += entry.bytes;
        }
        return total;
    }

private:
    std::string snapshot_directory() const {
        std::lock_guard<std::mutex> lock(mutex);
        return disk_directory;
    }

    /// Callers must NOT already hold `mutex`.
    void count_error() {
        std::lock_guard<std::mutex> lock(mutex);
        ++errors;
    }

    /// Callers must NOT already hold `mutex`.
    void count_miss() {
        std::lock_guard<std::mutex> lock(mutex);
        ++misses;
    }

    void insert_locked(const std::string& key, const std::vector<uint32_t>& spirv) {
        const auto existing = index.find(key);
        if (existing != index.end()) {
            existing->second->second = spirv;
            lru.splice(lru.begin(), lru, existing->second);
            return;
        }
        if (max_memory_entries == 0) {
            return;  // memory half switched off by configuration
        }
        lru.emplace_front(key, spirv);
        index[key] = lru.begin();
        evict_to_locked(max_memory_entries);
    }

    void evict_to_locked(size_t limit) {
        while (lru.size() > limit) {
            const auto victim = std::prev(lru.end());
            index.erase(victim->first);
            lru.erase(victim);
        }
    }

    void reap_temps(const std::string& directory, const std::string& pid) {
        DIR* handle = ::opendir(directory.c_str());
        if (handle == nullptr) {
            return;
        }
        size_t removed = 0;
        while (const dirent* item = ::readdir(handle)) {
            const std::string name(item->d_name);
            if (is_our_temp_name(name, pid)) {
                if (::unlink(join_path(directory, name).c_str()) == 0) {
                    ++removed;
                }
            }
        }
        ::closedir(handle);
        if (removed != 0) {
            LOGI("open(%s): removed %zu leftover temporary files", directory.c_str(), removed);
        }
    }

    // WHY one mutex for everything: a shader cache is touched a handful of
    // times per shader load and never per frame, so contention is irrelevant
    // and a single lock keeps the LRU order, the counters and the directory
    // snapshot trivially consistent.
    //
    // Lock order: this mutex is a LEAF. The cache is reached from inside
    // ShaderManager with the manager's own lock already held, so the order is
    // manager -> cache and never the other way round; nothing in this file calls
    // back into a manager. No lock is ever held across a syscall: file I/O runs
    // on a snapshot of `disk_directory` with the mutex released, and the
    // counters are moved once it is done.
    mutable std::mutex mutex;

    LruList lru;  // front == most recently used
    std::unordered_map<std::string, LruList::iterator> index;

    size_t max_memory_entries = 256;
    uint64_t max_disk_bytes = 64ull * 1024ull * 1024ull;
    std::string disk_directory;  // empty means memory only

    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t stores = 0;
    uint64_t errors = 0;
};

namespace {

// A cache is an optimisation, so nothing it does may take the process down: a
// bad_alloc from a vector resize or an unordered_map rehash, and a
// std::system_error from the mutex, all have to degrade into "miss this time".
// Every public method runs its body through one of these three.
template <typename Fn>
bool guarded(const char* what, Fn&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& error) {
        LOGE("ShaderCache::%s: %s", what, error.what());
    } catch (...) {
        LOGE("ShaderCache::%s: unknown exception", what);
    }
    return false;
}

template <typename Fn>
void guarded_void(const char* what, Fn&& body) noexcept {
    try {
        body();
    } catch (const std::exception& error) {
        LOGE("ShaderCache::%s: %s", what, error.what());
    } catch (...) {
        LOGE("ShaderCache::%s: unknown exception", what);
    }
}

// Result first, so a caller can name the return type - guarded_value<uint64_t>(...)
// - while Fn is still deduced from the lambda. With the parameters the other way
// round the explicit argument binds to Fn and every such call fails to
// instantiate.
template <typename Result, typename Fn>
Result guarded_value(const char* what, Fn&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& error) {
        LOGE("ShaderCache::%s: %s", what, error.what());
    } catch (...) {
        LOGE("ShaderCache::%s: unknown exception", what);
    }
    return Result{};
}

} // namespace

ShaderCache::ShaderCache() : pImpl(std::make_unique<Impl>()) {}
ShaderCache::~ShaderCache() = default;

bool ShaderCache::open(const std::string& directory) {
    return guarded("open", [&] { return pImpl != nullptr && pImpl->open(directory); });
}

std::string ShaderCache::buildKey(ShaderStage stage,
                                  const std::string& source,
                                  const std::vector<std::string>& defines,
                                  const std::string& target_api,
                                  uint32_t toolchain_version) {
    return guarded_value<std::string>("buildKey", [&] {
        return build_key(stage, source, defines, target_api, toolchain_version);
    });
}

bool ShaderCache::lookup(const std::string& key, std::vector<uint32_t>* spirv) {
    return guarded("lookup", [&] { return pImpl != nullptr && pImpl->lookup(key, spirv); });
}

bool ShaderCache::store(const std::string& key, ShaderStage stage,
                        const std::vector<uint32_t>& spirv) {
    return guarded("store", [&] { return pImpl != nullptr && pImpl->store(key, stage, spirv); });
}

void ShaderCache::setMaxMemoryEntries(size_t max_entries) {
    guarded_void("setMaxMemoryEntries",
                 [&] { if (pImpl != nullptr) pImpl->set_max_memory_entries(max_entries); });
}

void ShaderCache::setMaxDiskBytes(uint64_t max_bytes) {
    guarded_void("setMaxDiskBytes",
                 [&] { if (pImpl != nullptr) pImpl->set_max_disk_bytes(max_bytes); });
}

void ShaderCache::clear() {
    guarded_void("clear", [&] { if (pImpl != nullptr) pImpl->clear(); });
}

uint64_t ShaderCache::hitCount() const {
    return guarded_value<uint64_t>("hitCount",
                                   [&] { return pImpl != nullptr ? pImpl->hit_count() : 0u; });
}

uint64_t ShaderCache::missCount() const {
    return guarded_value<uint64_t>("missCount",
                                   [&] { return pImpl != nullptr ? pImpl->miss_count() : 0u; });
}

uint64_t ShaderCache::storeCount() const {
    return guarded_value<uint64_t>("storeCount",
                                   [&] { return pImpl != nullptr ? pImpl->store_count() : 0u; });
}

uint64_t ShaderCache::errorCount() const {
    return guarded_value<uint64_t>("errorCount",
                                   [&] { return pImpl != nullptr ? pImpl->error_count() : 0u; });
}

size_t ShaderCache::memoryEntryCount() const {
    return guarded_value<size_t>(
            "memoryEntryCount", [&] { return pImpl != nullptr ? pImpl->memory_entry_count() : 0u; });
}

size_t ShaderCache::diskEntryCount() const {
    return guarded_value<size_t>(
            "diskEntryCount", [&] { return pImpl != nullptr ? pImpl->disk_entry_count() : 0u; });
}

uint64_t ShaderCache::diskByteCount() const {
    return guarded_value<uint64_t>(
            "diskByteCount", [&] { return pImpl != nullptr ? pImpl->disk_byte_count() : 0u; });
}

} // namespace copper
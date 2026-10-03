#pragma once

#include "shader_manager.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace copper {

/// Persistent cache of compiled SPIR-V, keyed by a content hash.
///
/// It stores nothing but SPIR-V words and the key that identifies them: no
/// compiler, no backend, no device state. Keeping it that ignorant is what
/// lets the Vulkan and GL paths share one cache, and it is why a cache failure
/// is allowed to be nothing worse than "compile it again".
///
/// Until open() succeeds the cache is memory-only. Every public method is safe
/// to call concurrently from any thread and never throws; on a moved-from
/// instance the instance methods are no-ops (a lookup misses, a counter reads
/// zero), so a cache that got moved into is not a crash waiting to happen.
class ShaderCache {
public:
    ShaderCache();
    ~ShaderCache();

    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;
    ShaderCache(ShaderCache&&) noexcept = default;
    ShaderCache& operator=(ShaderCache&&) noexcept = default;

    /// Enables the on-disk half. `directory` is created if missing. An empty
    /// string keeps the cache in memory only.
    ///
    /// Returns false and falls back to memory only when `directory` cannot be
    /// created, is not a directory, or cannot be written: retrying every store
    /// against a path that will never work is worse than not caching at all.
    /// The memory half is left untouched either way, and an existing usable
    /// directory is replaced on success.
    bool open(const std::string& directory);

    /// The single definition of what identifies a compiled artifact; the
    /// on-disk filename is its hex form.
    /// `defines` may arrive in any order and may contain duplicates: normalise
    /// them (trim ASCII whitespace, drop empties, sort UNSIGNED bytewise
    /// ascending, de-duplicate) before hashing, because two call sites passing
    /// the same defines in a different order must produce the same key.
    /// `target_api` is an ASCII tag, currently "vulkan" or "gl".
    /// `toolchain_version` identifies the compiler; a change to it MUST produce
    /// a different key so stale artifacts are never reused.
    ///
    /// Returns 16 lowercase hex digits, or an empty string if the digest could
    /// not be produced (allocation failure); an empty string is not a valid key
    /// and is rejected everywhere else in this class.
    static std::string buildKey(ShaderStage stage,
                                const std::string& source,
                                const std::vector<std::string>& defines,
                                const std::string& target_api,
                                uint32_t toolchain_version);

    /// Returns true and fills `spirv` on a hit.
    ///
    /// `spirv` is left untouched on a miss. The memory half is consulted first;
    /// a disk hit is promoted into it. A disk entry that fails verification is
    /// reported as a miss and deleted, because its bytes can never be trusted
    /// again; an entry that merely cannot be read is kept, because a full or
    /// busy filesystem must not cost us bytes that were fine a moment ago.
    bool lookup(const std::string& key, std::vector<uint32_t>* spirv);

    /// Stores `spirv`. False when the payload is rejected as structurally
    /// invalid (wrong SPIR-V magic, empty).
    ///
    /// A valid payload is always admitted to the memory half first; false is
    /// also returned when the on-disk half refused it (unwritable directory,
    /// full filesystem), in which case the entry is in memory only. A malformed
    /// key is rejected before anything is stored.
    bool store(const std::string& key, ShaderStage stage, const std::vector<uint32_t>& spirv);

    /// Highest keys kept in memory; least recently used are evicted.
    ///
    /// The default is 256 entries, which is several times the number of
    /// distinct shaders a typical material set compiles to, so the cache only
    /// starts throwing work away once a project genuinely needs it. 0 disables
    /// the memory half; lowering the limit evicts immediately.
    void setMaxMemoryEntries(size_t max_entries);

    /// Highest bytes kept on disk; oldest files removed first.
    ///
    /// The default is 64 MiB, small enough to stay well inside a normal app
    /// cache quota and large enough that trimming is rare. Lowering it trims
    /// straight away, which may delete files before the next store. Only files
    /// that match this cache's own naming convention are ever considered.
    void setMaxDiskBytes(uint64_t max_bytes);

    /// Empties both halves and resets the diagnostics: every counter returns to
    /// zero, the memory half is dropped and, when a directory is open, the
    /// entry files are unlinked. Used by tests and by a "forget my shaders"
    /// path; it is not a way to trim.
    void clear();

    // Diagnostics, safe from any thread.
    //
    // hitCount() + missCount() equals the number of lookup() calls: a
    // malformed key counts as both a miss and an error, because it produced no
    // words. diskEntryCount() and diskByteCount() are read from the directory
    // rather than from a running total, so they include entries another
    // process sharing the directory wrote, and they report 0 when no usable
    // directory is open.
    uint64_t hitCount() const;
    uint64_t missCount() const;
    uint64_t storeCount() const;
    uint64_t errorCount() const;
    size_t memoryEntryCount() const;
    size_t diskEntryCount() const;
    uint64_t diskByteCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace copper
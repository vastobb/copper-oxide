#pragma once

#include <GLES3/gl32.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "buffer_manager.h"

namespace copper {

class GLESCRenderer;

// BufferManager describes the purpose of a buffer with an opaque uint32_t. GLES
// has no usage enum of its own, so the bits below are the GLES contract: they
// are declared here (instead of leaking GL names into callers) and translated
// to a buffer target plus a storage hint by the manager. The numbers only have
// to stay stable inside this backend.
enum class BufferUsage : uint32_t {
    None = 0,
    Vertex = 1u << 0,       // GL_ARRAY_BUFFER
    Index = 1u << 1,        // GL_ELEMENT_ARRAY_BUFFER
    Uniform = 1u << 2,      // GL_UNIFORM_BUFFER (GLES 3.1)
    Storage = 1u << 3,      // GL_SHADER_STORAGE_BUFFER (GLES 3.1)
    TransferSrc = 1u << 4,  // GL_COPY_READ_BUFFER (GLES 3.1)
    TransferDst = 1u << 5,  // GL_COPY_WRITE_BUFFER (GLES 3.1)
};

// Memory hints. GLES cannot promise anything about placement, so these only
// steer the driver's storage hint and the host mapping behaviour.
enum class BufferMemoryFlags : uint32_t {
    None = 0,
    Persistent = 1u << 0,  // mapped/unmapped every frame: use stream storage
    Readback = 1u << 1,     // GPU -> CPU reads are expected on this buffer
};

inline uint32_t operator|(BufferUsage lhs, BufferUsage rhs) {
    return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}

inline uint32_t operator|(BufferMemoryFlags lhs, BufferMemoryFlags rhs) {
    return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}

// Note: BufferUsage::None / BufferMemoryFlags::None always match because they
// are zero; callers that need "no such bit" should test the value directly.
inline bool hasFlag(uint32_t bits, BufferUsage flag) {
    const uint32_t mask = static_cast<uint32_t>(flag);
    return (bits & mask) == mask;
}

inline bool hasFlag(uint32_t bits, BufferMemoryFlags flag) {
    const uint32_t mask = static_cast<uint32_t>(flag);
    return (bits & mask) == mask;
}

// OpenGL ES backend for BufferManager.
//
// GLES has no persistent (GPU-mapped) buffers at all, so map/unmap is
// emulated with host staging memory: mapBuffer() hands out ordinary CPU memory,
// unmapBuffer() pushes it into the GL buffer and invalidateBuffer() pulls the GL
// contents back. Every GL entry point is gated on contextAvailable(), so the
// manager can be constructed without a live context (unit tests, shutdown order)
// and simply reports failure instead of crashing.
//
// Locking: the base class calls the on* hooks while holding its own mutex, so the
// hooks must never call back into a public base method (that would re-lock a
// non-recursive mutex). All state a hook needs is kept in buffers_, guarded by
// mutex_, and hooks take that lock for their whole body. Lock order is therefore
// always base -> backend, never the other way around.
class GLESBufferManager : public BufferManager {
public:
    // renderer may be null: every GL call is then skipped.
    explicit GLESBufferManager(GLESCRenderer* renderer);
    ~GLESBufferManager() override;

    GLESBufferManager(const GLESBufferManager&) = delete;
    GLESBufferManager& operator=(const GLESBufferManager&) = delete;
    GLESBufferManager(GLESBufferManager&&) = delete;
    GLESBufferManager& operator=(GLESBufferManager&&) = delete;

    // Adopts the renderer if the instance was built without one, so a manager
    // created by the backend factory before GLESCRenderer exists still works.
    bool initialize(RendererBase* renderer) override;

    // GL name of a handle, 0 when unknown. The state/command manager binds this
    // name directly, which is the whole point of keeping the mapping here.
    GLuint glObject(uint64_t handle) const;
    GLenum glTarget(uint64_t handle) const;
    // Host pointer returned by mapBuffer(), nullptr when not mapped.
    void* hostPointer(uint64_t handle) const;
    size_t liveBufferCount() const;

    // True when this thread could issue GL calls: the renderer must own a
    // context *and* a surface, because GLESCRenderer::make_current() refuses to
    // bind anything without both and a GL call without a current context is
    // undefined behaviour.
    bool contextAvailable() const;

    // Usage bit(s) -> GL buffer target. The first matching bit wins in the order
    // Index, Uniform, Storage, TransferSrc, TransferDst, Vertex; anything else
    // (including usage 0) falls back to GL_ARRAY_BUFFER, which is the only
    // target every GLES 3.x implementation supports. Uniform, Storage and the
    // copy targets are GLES 3.1 only; on 3.0 they degrade to GL_ARRAY_BUFFER at
    // creation so the buffer is still allocated.
    static GLenum glTargetForUsage(uint32_t usage);
    // Usage/memory hints -> GL_STATIC_DRAW / GL_DYNAMIC_DRAW / GL_STREAM_DRAW.
    static GLenum glStorageHint(uint32_t usage, uint32_t memory_flags);
    // Target -> the GL_*_BINDING query used to save and restore the binding, so
    // uploads do not invalidate the bindings the state cache tracks.
    static GLenum glBindingQueryFor(GLenum target);

protected:
    bool onCreateBuffer(uint64_t handle, uint64_t size, uint32_t usage, uint32_t memory_flags) override;
    void onDestroyBuffer(uint64_t handle) override;
    void* onMapBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onUnmapBuffer(uint64_t handle) override;
    void onFlushBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onInvalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) override;
    void onUpdateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) override;
    void onCopyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) override;
    void onSetBufferDebugName(uint64_t handle, const std::string& name) override;

private:
    struct BufferObject {
        uint64_t handle = 0;
        GLuint buffer = 0;
        GLenum target = GL_ARRAY_BUFFER;
        uint64_t size = 0;
        uint32_t usage = 0;
        uint32_t memory_flags = 0;
        // Host staging window handed out by mapBuffer(). Empty when unmapped.
        std::vector<uint8_t> staging;
        uint64_t staging_offset = 0;
        uint64_t staging_size = 0;
        std::string debug_name;
    };

    // Caller must hold mutex_.
    bool glesAtLeast(int major, int minor);
    // Currently bound buffer of a target, so uploads can leave the binding the
    // state cache expects intact.
    GLuint boundBuffer(GLenum target) const;
    // Returns a writable host pointer for the range starting at offset and spanning
    // size bytes, growing the staging window if needed. Fails when the object is
    // mapped and the new window would move memory the caller still holds.
    uint8_t* ensureHostWindow(BufferObject& object, uint64_t offset, uint64_t size);
    // Read-only counterpart; nullptr when the window does not cover the range.
    const uint8_t* hostRange(const BufferObject& object, uint64_t offset, uint64_t size) const;
    bool copyThroughHostMemory(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset);
    void destroyAllLocked();

    GLESCRenderer* renderer_ = nullptr;
    std::unordered_map<uint64_t, BufferObject> buffers_;
    mutable std::mutex mutex_;
    // GL_MAJOR_VERSION / GL_MINOR_VERSION queried once per context; the context
    // can be recreated on surface loss, so the cache is filled lazily and reset
    // whenever no context is available.
    bool gles_version_known_ = false;
    int gles_major_ = 0;
    int gles_minor_ = 0;
};

} // namespace copper
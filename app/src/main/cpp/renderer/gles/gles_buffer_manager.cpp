#include "gles_buffer_manager.h"

#include "gles_missing_es31.h"

#include "gles_renderer.h"

#include <EGL/egl.h>
#include <android/log.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#define LOG_TAG "CopperOxide-GLES-Buffer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// GL_KHR_debug is an extension in GLES, so glObjectLabel is not declared by
// <GLES3/gl32.h> and has to be resolved through EGL. The length argument is a
// GLsizei in the extension (core desktop GL uses the wider GLsizeiptr), so the
// typedef must keep the 32-bit width or 64-bit ABIs would misread it.
using PFN_glObjectLabel = void (*)(GLenum, GLuint, GLsizei, const GLchar*);

PFN_glObjectLabel resolveObjectLabel(GLESCRenderer* renderer) {
    if (!renderer || !renderer->isExtensionSupported("GL_KHR_debug")) {
        return nullptr;
    }
    return reinterpret_cast<PFN_glObjectLabel>(eglGetProcAddress("glObjectLabel"));
}

// Guard against silently truncating a size into GLsizeiptr, which is only 32
// bits wide on the 32-bit ABIs this renderer still ships for.
bool fitsInSizeiptr(uint64_t size) {
    return size <= static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max());
}

} // namespace

GLESBufferManager::GLESBufferManager(GLESCRenderer* renderer) : renderer_(renderer) {}

GLESBufferManager::~GLESBufferManager() {
    // BufferManager::~BufferManager() is defaulted and never runs shutdown(), so
    // the base records would simply be dropped. Running it here routes through
    // the on* hooks (base lock, then ours - never the reverse) and leaves the two
    // sides consistent. destroyAllLocked() is the safety net for anything that
    // reached the GL objects without a base record.
    BufferManager::shutdown();

    std::lock_guard<std::mutex> lock(mutex_);
    destroyAllLocked();
}

bool GLESBufferManager::initialize(RendererBase* renderer) {
    if (!BufferManager::initialize(renderer)) {
        return false;
    }
    if (!renderer_ && renderer) {
        // A manager built before the renderer existed adopts it here; a foreign
        // RendererBase would leave renderer_ null and every GL call inert.
        renderer_ = dynamic_cast<GLESCRenderer*>(renderer);
        if (!renderer_) {
            LOGW("buffer manager cannot use this renderer backend");
        }
    }
    return true;
}

bool GLESBufferManager::contextAvailable() const {
    if (!renderer_) {
        // Constructed without a renderer (unit tests): no GL call is legal.
        return false;
    }
    // GLESCRenderer::make_current() refuses to bind without a surface, and a GL
    // call on a thread without a current context is undefined, so both are
    // required before touching GL.
    if (renderer_->get_egl_context() == EGL_NO_CONTEXT) {
        return false;
    }
    return renderer_->get_egl_surface() != EGL_NO_SURFACE;
}

GLenum GLESBufferManager::glTargetForUsage(uint32_t usage) {
    // The bits are checked in a fixed order because a buffer is normally bound to
    // exactly one target; Index wins over Vertex so index buffers keep the
    // element-array target that the state cache binds for drawing.
    if (hasFlag(usage, GLESBufferUsage::Index)) {
        return GL_ELEMENT_ARRAY_BUFFER;
    }
    if (hasFlag(usage, GLESBufferUsage::Uniform)) {
        return GL_UNIFORM_BUFFER;
    }
    if (hasFlag(usage, GLESBufferUsage::Storage)) {
        return GL_SHADER_STORAGE_BUFFER;
    }
    if (hasFlag(usage, GLESBufferUsage::TransferSrc)) {
        return GL_COPY_READ_BUFFER;
    }
    if (hasFlag(usage, GLESBufferUsage::TransferDst)) {
        return GL_COPY_WRITE_BUFFER;
    }
    // GL_ARRAY_BUFFER is the fallback for usage 0 and for any bit this backend
    // does not know: it exists on every GLES 3.x implementation and is always a
    // legal binding point for glBufferData.
    return GL_ARRAY_BUFFER;
}

GLenum GLESBufferManager::glStorageHint(uint32_t usage, uint32_t memory_flags) {
    if (hasFlag(memory_flags, GLESBufferMemoryFlags::Persistent)) {
        return GL_STREAM_DRAW;
    }
    // Uniform/storage/copy-source buffers are rewritten regularly, which is
    // exactly what GL_DYNAMIC_DRAW describes.
    if (hasFlag(usage, GLESBufferUsage::Uniform) || hasFlag(usage, GLESBufferUsage::Storage) ||
        hasFlag(usage, GLESBufferUsage::TransferSrc)) {
        return GL_DYNAMIC_DRAW;
    }
    if (hasFlag(usage, GLESBufferUsage::TransferDst)) {
        // Written once, read once: the driver can discard it after the copy.
        return GL_STREAM_DRAW;
    }
    return GL_STATIC_DRAW;
}

GLenum GLESBufferManager::glBindingQueryFor(GLenum target) {
    switch (target) {
        case GL_ELEMENT_ARRAY_BUFFER:
            return GL_ELEMENT_ARRAY_BUFFER_BINDING;
        case GL_UNIFORM_BUFFER:
            return GL_UNIFORM_BUFFER_BINDING;
        case GL_SHADER_STORAGE_BUFFER:
            return GL_SHADER_STORAGE_BUFFER_BINDING;
        case GL_COPY_READ_BUFFER:
            return GL_COPY_READ_BUFFER_BINDING;
        case GL_COPY_WRITE_BUFFER:
            return GL_COPY_WRITE_BUFFER_BINDING;
        case GL_ARRAY_BUFFER:
        default:
            // Unknown targets fall back to the array binding; GL_ARRAY_BUFFER is
            // the target glTargetForUsage() returns for anything unknown.
            return GL_ARRAY_BUFFER_BINDING;
    }
}

GLuint GLESBufferManager::boundBuffer(GLenum target) const {
    GLint binding = 0;
    glGetIntegerv(glBindingQueryFor(target), &binding);
    return static_cast<GLuint>(binding);
}

GLuint GLESBufferManager::glObject(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    return it == buffers_.end() ? 0u : it->second.buffer;
}

GLenum GLESBufferManager::glTarget(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    return it == buffers_.end() ? GL_ARRAY_BUFFER : it->second.target;
}

void* GLESBufferManager::hostPointer(uint64_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end() || it->second.staging.empty()) {
        return nullptr;
    }
    // The map entry is const here, but the window is the manager's own host
    // staging memory: mapBuffer's contract is that the caller writes through it,
    // exactly as it would on Vulkan. const_cast is the honest expression of that.
    return const_cast<uint8_t*>(it->second.staging.data());
}

size_t GLESBufferManager::liveBufferCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.size();
}

// Helper for the GLES 3.1 features used below (glCopyBufferSubData,
// glGetBufferSubData and the indexed copy targets). The version is queried once
// and dropped whenever the context is gone, because the surface can be recreated
// under a new context with different capabilities.
bool GLESBufferManager::glesAtLeast(int major, int minor) {
    if (!contextAvailable()) {
        // The surface can be recreated under a new context with different
        // capabilities, so a cached version must not outlive the context it was
        // queried from.
        gles_version_known_ = false;
        return false;
    }
    if (!gles_version_known_) {
        GLint value = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &value);
        gles_major_ = value;
        glGetIntegerv(GL_MINOR_VERSION, &value);
        gles_minor_ = value;
        gles_version_known_ = true;
        LOGI("GLES %d.%d detected", gles_major_, gles_minor_);
    }
    if (gles_major_ != major) {
        return gles_major_ > major;
    }
    return gles_minor_ >= minor;
}

bool GLESBufferManager::onCreateBuffer(uint64_t handle, uint64_t size, uint32_t usage, uint32_t memory_flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!contextAvailable()) {
        return false;
    }
    if (size == 0 || !fitsInSizeiptr(size)) {
        LOGE("buffer %llu rejected: invalid size %llu", (unsigned long long)handle, (unsigned long long)size);
        return false;
    }

    GLenum target = glTargetForUsage(usage);
    if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER && !glesAtLeast(3, 1)) {
        // The uniform, storage and copy targets only exist in GLES 3.1. On 3.0 the
        // buffer is still allocated on the array target so the handle stays valid,
        // and copyBuffer() degrades to the host path instead of becoming a no-op.
        LOGW("usage %u needs GLES 3.1 targets, falling back to GL_ARRAY_BUFFER", usage);
        target = GL_ARRAY_BUFFER;
    }

    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    if (buffer == 0) {
        LOGE("glGenBuffers failed for buffer %llu", (unsigned long long)handle);
        return false;
    }

    const GLuint previous = boundBuffer(target);
    glBindBuffer(target, buffer);
    // Storage is allocated here and left uninitialised: GLES gives no way to
    // allocate and seed in one step, and the contents are undefined until the
    // first updateBuffer()/unmapBuffer().
    glBufferData(target, static_cast<GLsizeiptr>(size), nullptr, glStorageHint(usage, memory_flags));
    glBindBuffer(target, previous);

    if (glGetError() != GL_NO_ERROR) {
        // Draining the error here means a failed allocation is reported as a
        // failed createBuffer() instead of leaving a dead GL name behind.
        LOGE("glBufferData failed for buffer %llu (%llu bytes)", (unsigned long long)handle,
             (unsigned long long)size);
        glDeleteBuffers(1, &buffer);
        return false;
    }

    BufferObject object;
    object.handle = handle;
    object.buffer = buffer;
    object.target = target;
    object.size = size;
    object.usage = usage;
    object.memory_flags = memory_flags;
    buffers_[handle] = std::move(object);
    return true;
}

void GLESBufferManager::onDestroyBuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return;
    }
    if (it->second.buffer != 0 && contextAvailable()) {
        glDeleteBuffers(1, &it->second.buffer);
    }
    buffers_.erase(it);
}

void* GLESBufferManager::onMapBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return nullptr;
    }
    BufferObject& object = it->second;
    if (!object.staging.empty()) {
        // Already mapped: re-mapping would drop the window the caller holds.
        return nullptr;
    }
    // size 0 means "from offset to the end of the buffer", matching the base API.
    // The offset is validated first so that size == 0 cannot underflow.
    if (offset > object.size) {
        LOGW("map of buffer %llu starts outside it (offset %llu)", (unsigned long long)handle,
             (unsigned long long)offset);
        return nullptr;
    }
    if (size == 0) {
        size = object.size - offset;
    }
    if (size > object.size - offset) {
        LOGW("map of buffer %llu outside its range (offset %llu, size %llu)", (unsigned long long)handle,
             (unsigned long long)offset, (unsigned long long)size);
        return nullptr;
    }

    // GLES has no GPU-mapped buffers, so the map is plain host memory. It is
    // zero filled because a partially written window must not upload whatever
    // happened to be in the allocator's memory.
    object.staging.assign(static_cast<size_t>(size), 0u);
    object.staging_offset = offset;
    object.staging_size = size;
    return object.staging.data();
}

void GLESBufferManager::onUnmapBuffer(uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return;
    }
    BufferObject& object = it->second;
    if (object.staging.empty()) {
        return;
    }

    if (object.buffer != 0 && contextAvailable()) {
        // Unmap is where host writes become visible: the equivalent of making a
        // coherent mapping visible in Vulkan is a plain glBufferSubData here.
        const GLuint previous = boundBuffer(object.target);
        glBindBuffer(object.target, object.buffer);
        glBufferSubData(object.target, static_cast<GLintptr>(object.staging_offset),
                        static_cast<GLsizeiptr>(object.staging_size), object.staging.data());
        glBindBuffer(object.target, previous);
    }

    // The window is per-map, so it is released instead of being cached: a
    // persistent cache would pin host memory for the lifetime of the buffer.
    object.staging.clear();
    object.staging.shrink_to_fit();
    object.staging_offset = 0;
    object.staging_size = 0;
}

void GLESBufferManager::onFlushBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    // The mapping is ordinary host memory, which is coherent by construction, so
    // there is nothing to flush: unmapBuffer() performs the upload. The handle
    // and range are accepted (and ignored) for base-class compatibility.
    (void)handle;
    (void)offset;
    (void)size;
}

void GLESBufferManager::onInvalidateBuffer(uint64_t handle, uint64_t offset, uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return;
    }
    BufferObject& object = it->second;
    if (object.staging.empty()) {
        // Nowhere to deliver GPU data on the CPU side; the base class also
        // refuses this when the buffer is not mapped.
        return;
    }
    if (size == 0) {
        // size 0 means "everything the mapping covers".
        offset = object.staging_offset;
        size = object.staging_size;
    }
    uint8_t* window = const_cast<uint8_t*>(hostRange(object, offset, size));
    if (!window) {
        LOGW("invalidate of buffer %llu outside its mapping", (unsigned long long)handle);
        return;
    }
    if (!contextAvailable()) {
        return;
    }
    if (!glesAtLeast(3, 1)) {
        // glGetBufferSubData is GLES 3.1 core; ES 3.0 cannot read buffers back
        // at all, so the data is simply left stale.
        LOGW("invalidate of buffer %llu needs GLES 3.1", (unsigned long long)handle);
        return;
    }

    const GLuint previous = boundBuffer(object.target);
    glBindBuffer(object.target, object.buffer);
    // Resolved through eglGetProcAddress: see gles_missing_es31.h.
    PFNCO_GLES_GETBUFFERSUBDATA read_back = copper::gles::getBufferSubData();
    if (read_back == nullptr) {
        LOGW("glGetBufferSubData is unreachable; invalidateBuffer has no effect");
        glBindBuffer(object.target, previous);
        return;
    }
    read_back(object.target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), window);
    glBindBuffer(object.target, previous);
}

void GLESBufferManager::onUpdateBuffer(uint64_t handle, uint64_t offset, const void* data, uint64_t size) {
    if (!data || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return;
    }
    BufferObject& object = it->second;
    if (!contextAvailable() || object.buffer == 0) {
        return;
    }
    if (!fitsInSizeiptr(size) || offset > object.size || size > object.size - offset) {
        // The base class range-checks before it gets here; the hook is also
        // reachable directly, and a short write would corrupt neighbours.
        LOGW("update of buffer %llu outside its range (offset %llu, size %llu)", (unsigned long long)handle,
             (unsigned long long)offset, (unsigned long long)size);
        return;
    }

    const GLuint previous = boundBuffer(object.target);
    glBindBuffer(object.target, object.buffer);
    glBufferSubData(object.target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    glBindBuffer(object.target, previous);
}

void GLESBufferManager::onCopyBuffer(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset, uint64_t dst_offset) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto src_it = buffers_.find(src);
    auto dst_it = buffers_.find(dst);
    if (src_it == buffers_.end() || dst_it == buffers_.end()) {
        return;
    }
    if (!contextAvailable()) {
        return;
    }
    if (src_it->second.buffer == 0 || dst_it->second.buffer == 0) {
        return;
    }
    if (src == dst) {
        const uint64_t src_end = src_offset + size;
        const uint64_t dst_end = dst_offset + size;
        if (src_offset < dst_end && dst_offset < src_end) {
            // Overlapping ranges are undefined for glCopyBufferSubData, so they
            // are rejected instead of producing driver dependent garbage.
            LOGW("overlapping copy of buffer %llu rejected", (unsigned long long)src);
            return;
        }
    }

    if (glesAtLeast(3, 1)) {
        // glCopyBufferSubData is GLES 3.1 core (the GL_ARB_copy_buffer equivalent
        // the base interface was modelled after).
        const GLuint previous_read = boundBuffer(GL_COPY_READ_BUFFER);
        const GLuint previous_write = boundBuffer(GL_COPY_WRITE_BUFFER);
        glBindBuffer(GL_COPY_READ_BUFFER, src_it->second.buffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, dst_it->second.buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(src_offset), static_cast<GLintptr>(dst_offset),
                            static_cast<GLsizeiptr>(size));
        glBindBuffer(GL_COPY_READ_BUFFER, previous_read);
        glBindBuffer(GL_COPY_WRITE_BUFFER, previous_write);
        return;
    }

    copyThroughHostMemory(src, dst, size, src_offset, dst_offset);
}

bool GLESBufferManager::copyThroughHostMemory(uint64_t src, uint64_t dst, uint64_t size, uint64_t src_offset,
                                             uint64_t dst_offset) {
    auto src_it = buffers_.find(src);
    auto dst_it = buffers_.find(dst);
    if (src_it == buffers_.end() || dst_it == buffers_.end()) {
        return false;
    }
    BufferObject& source = src_it->second;
    BufferObject& destination = dst_it->second;

    const uint8_t* src_ptr = hostRange(source, src_offset, size);
    if (!src_ptr) {
        // GLES 3.0 has neither glGetBufferSubData nor glMapBufferRange, so the
        // only readable source is one the caller already has mapped.
        LOGW("copy of %llu bytes skipped: source buffer %llu is not host-mapped",
             (unsigned long long)size, (unsigned long long)src);
        return false;
    }

    uint8_t* dst_ptr = ensureHostWindow(destination, dst_offset, size);
    if (!dst_ptr) {
        return false;
    }
    std::memcpy(dst_ptr, src_ptr, static_cast<size_t>(size));

    if (destination.buffer == 0 || !contextAvailable()) {
        // The data is already visible in the destination's host window; the
        // unmap will push it.
        return true;
    }
    const GLuint previous = boundBuffer(destination.target);
    glBindBuffer(destination.target, destination.buffer);
    glBufferSubData(destination.target, static_cast<GLintptr>(dst_offset), static_cast<GLsizeiptr>(size), dst_ptr);
    glBindBuffer(destination.target, previous);
    return true;
}

uint8_t* GLESBufferManager::ensureHostWindow(BufferObject& object, uint64_t offset, uint64_t size) {
    if (size == 0 || offset > object.size || size > object.size - offset) {
        return nullptr;
    }
    // Only meaningful on 32-bit ABIs, where a buffer may be larger than the
    // address space the staging vector can hold.
    if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
        if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            return nullptr;
        }
    }
    if (!object.staging.empty()) {
        if (object.staging_offset <= offset && offset + size <= object.staging_offset + object.staging_size) {
            return object.staging.data() + static_cast<size_t>(offset - object.staging_offset);
        }
        // Growing the window would move memory the caller may still be holding,
        // so the copy is refused instead of invalidating the mapping.
        LOGW("host window of buffer %llu cannot grow while mapped", (unsigned long long)object.handle);
        return nullptr;
    }

    object.staging.assign(static_cast<size_t>(size), 0u);
    object.staging_offset = offset;
    object.staging_size = size;
    return object.staging.data();
}

const uint8_t* GLESBufferManager::hostRange(const BufferObject& object, uint64_t offset, uint64_t size) const {
    if (object.staging.empty() || offset < object.staging_offset) {
        return nullptr;
    }
    const uint64_t delta = offset - object.staging_offset;
    if (delta > object.staging_size || size > object.staging_size - delta) {
        return nullptr;
    }
    return object.staging.data() + static_cast<size_t>(delta);
}

void GLESBufferManager::onSetBufferDebugName(uint64_t handle, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return;
    }
    it->second.debug_name = name;

    // GLES has no core object naming. GL_KHR_debug adds one, and since it is an
    // extension the entry point has to be resolved through EGL. Resolved per call
    // on purpose: naming is a rare debug-only path and the extension set can
    // change when the surface (and with it the context) is recreated.
    PFN_glObjectLabel label = resolveObjectLabel(renderer_);
    if (label != nullptr && it->second.buffer != 0) {
        label(GL_BUFFER, it->second.buffer, static_cast<GLsizei>(name.size()), name.c_str());
    }
}

void GLESBufferManager::destroyAllLocked() {
    if (buffers_.empty()) {
        return;
    }
    if (contextAvailable()) {
        for (auto& entry : buffers_) {
            BufferObject& object = entry.second;
            if (object.buffer != 0) {
                glDeleteBuffers(1, &object.buffer);
                object.buffer = 0;
            }
        }
    } else {
        // Without a context the names cannot be deleted; they belong to the
        // context, which frees them when it is destroyed.
        LOGW("%zu buffer(s) left for context teardown", buffers_.size());
    }
    buffers_.clear();
}

} // namespace copper
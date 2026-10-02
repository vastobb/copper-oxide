#include "gles_profiler.h"

#include "renderer_base.h"

#include <android/log.h>

#define LOG_TAG "CopperOxide-GLESCProfiler"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

namespace {

// GL_TIME_ELAPSED_EXT from GL_EXT_disjoint_timer_query. Spelled out rather than
// taken from the header because the token only exists in the extension (ES 3.0
// and 3.1 core have no timer query at all); the value is unchanged if a future
// ES version promotes it to core.
constexpr GLenum k_time_elapsed = 0x88BF;

// glGetQueryObjectiv takes a GLint*, and GL_TIME_ELAPSED is in nanoseconds, so a
// 32-bit read covers ~2.1 s of GPU time per frame. Far beyond a frame, but it is
// why this is a GLint and not a GLuint.
constexpr uint64_t k_nanos_per_milli = 1000000ULL;

} // namespace

GLESCProfiler::GLESCProfiler() = default;

GLESCProfiler::~GLESCProfiler() {
    // Nothing to release here on purpose: the destructor may run after the EGL
    // context is gone, and touching GL then is undefined. releaseQueriesLocked()
    // is driven from shutdown(), which the renderer calls while the context is
    // still alive.
}

bool GLESCProfiler::enableGpuTimerQueries(RendererBase* renderer) {
    const bool present =
            renderer != nullptr &&
            renderer->isExtensionSupported(k_timer_query_extension);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        extension_present_ = present;
        timing_requested_ = present;
    }
    if (!present) {
        // Honest failure: GPU time stays 0 for this device.
        LOGW("%s not supported; GPU time will be reported as 0",
             k_timer_query_extension);
        return false;
    }
    LOGD("%s is available; GPU timings are queried with one frame of latency",
         k_timer_query_extension);
    return true;
}

void GLESCProfiler::disableGpuTimerQueries() {
    std::lock_guard<std::mutex> lock(mutex_);
    timing_requested_ = false;
    last_gpu_time_ms_ = 0.0;
    last_gpu_time_valid_ = false;
    endActiveQueryLocked();
}

bool GLESCProfiler::gpuTimerQueriesAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return extension_present_;
}

bool GLESCProfiler::gpuTimingActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && timing_requested_ && extension_present_ && context_available_;
}

void GLESCProfiler::setContextAvailable(bool available) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_available_ == available) {
        return;
    }
    context_available_ = available;
    if (!available) {
        // The query object died with the context; forget it instead of leaving
        // a stale name behind that glDeleteQueries would reject on the next
        // context.
        queries_[0] = 0;
        queries_[1] = 0;
        query_active_ = false;
        query_pending_ = false;
        last_gpu_time_valid_ = false;
        last_gpu_time_ms_ = 0.0;
    }
}

bool GLESCProfiler::contextAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return context_available_;
}

double GLESCProfiler::lastGpuTimeMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_gpu_time_ms_;
}

bool GLESCProfiler::lastGpuTimeValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_gpu_time_valid_;
}

GLenum GLESCProfiler::lastGlError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_gl_error_;
}

void GLESCProfiler::setEnabled(bool enabled) {
    // Call the base first: it takes and releases its own lock, so nothing is
    // nested here.
    Profiler::setEnabled(enabled);
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!enabled_) {
        // Leave no query open across a disabled stretch; an active
        // GL_TIME_ELAPSED query blocks glBeginQuery on the same target.
        endActiveQueryLocked();
    }
}

void GLESCProfiler::reset() {
    Profiler::reset();
    std::lock_guard<std::mutex> lock(mutex_);
    last_gpu_time_ms_ = 0.0;
    last_gpu_time_valid_ = false;
}

void GLESCProfiler::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        releaseQueriesLocked();
        timing_requested_ = false;
        extension_present_ = false;
        context_available_ = false;
        last_gpu_time_valid_ = false;
        last_gpu_time_ms_ = 0.0;
    }
    Profiler::shutdown();
}

// --- query object lifetime (mutex_ held) -----------------------------------

bool GLESCProfiler::ensureQueriesLocked() {
    if (queries_[0] != 0) {
        return true;
    }
    if (!context_available_) {
        return false;
    }
    glGenQueries(2, queries_);
    last_gl_error_ = glGetError();
    if (last_gl_error_ != 0 || queries_[0] == 0) {
        LOGW("glGenQueries failed (GL error 0x%x); GPU timing disabled",
             last_gl_error_);
        queries_[0] = 0;
        queries_[1] = 0;
        timing_requested_ = false;
        return false;
    }
    return true;
}

void GLESCProfiler::endActiveQueryLocked() {
    if (!query_active_ || queries_[current_slot_] == 0 || !context_available_) {
        // Without a current context the query is already gone; only the shadow
        // flag has to be cleared.
        query_active_ = false;
        return;
    }
    glEndQuery(k_time_elapsed);
    query_active_ = false;
    query_pending_ = true;
}

void GLESCProfiler::releaseQueriesLocked() {
    if (!context_available_) {
        queries_[0] = 0;
        queries_[1] = 0;
        query_active_ = false;
        query_pending_ = false;
        return;
    }
    endActiveQueryLocked();
    for (GLuint& query : queries_) {
        if (query != 0) {
            glDeleteQueries(1, &query);
            query = 0;
        }
    }
    query_pending_ = false;
}

void GLESCProfiler::pollPreviousQueryLocked(size_t current_slot) {
    if (!query_pending_) {
        return;
    }
    const size_t previous_slot = current_slot ^ 1u;
    const GLuint query = queries_[previous_slot];
    if (query == 0 || !context_available_) {
        return;
    }

    // Non-blocking poll. Reading GL_QUERY_RESULT directly would wait for the
    // query, which is the stall this class exists to avoid.
    GLint available = 0;
    glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
    if (available == 0) {
        return;
    }

    GLint elapsed_ns = 0;
    glGetQueryObjectiv(query, GL_QUERY_RESULT, &elapsed_ns);
    last_gl_error_ = glGetError();
    // GL_TIME_ELAPSED (unlike GL_TIMESTAMP_EXT) is defined to report the elapsed
    // time, so no disjoint-availability check is needed here; a driver that
    // cannot measure reports 0, which lastGpuTimeValid() still marks as a real
    // (zero) measurement.
    last_gpu_time_ms_ =
            static_cast<double>(static_cast<uint32_t>(elapsed_ns)) /
            static_cast<double>(k_nanos_per_milli);
    last_gpu_time_valid_ = true;
    query_pending_ = false;
}

// --- frame hooks -----------------------------------------------------------

// Correctly ordered no-op plus the timer-query bracket. No GL call happens
// unless the context has been announced as current, because
// RendererBase::beginFrame() calls the profiler BEFORE the backend hook that
// does eglMakeCurrent.
void GLESCProfiler::onBeginFrame(uint64_t /*frame_number*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !timing_requested_ || !extension_present_ || !context_available_) {
        return;
    }
    if (!ensureQueriesLocked()) {
        return;
    }

    // A frame that was abandoned between begin and end would leave the previous
    // slot's query open, and glBeginQuery fails with GL_INVALID_OPERATION while
    // another query of the same target is active.
    endActiveQueryLocked();

    current_slot_ ^= 1u;
    glBeginQuery(k_time_elapsed, queries_[current_slot_]);
    const GLenum error = glGetError();
    if (error != 0) {
        LOGW("glBeginQuery(GL_TIME_ELAPSED) failed (GL error 0x%x); GPU timing "
             "disabled for this device",
             error);
        last_gl_error_ = error;
        timing_requested_ = false;
        return;
    }
    query_active_ = true;
}

void GLESCProfiler::onEndFrame(uint64_t /*frame_number*/, double /*frame_time_ms*/,
                               double /*cpu_time_ms*/, double /*gpu_time_ms*/,
                               uint32_t /*draw_calls*/) {
    // gpu_time_ms arrives as 0.0 from RendererBase::endFrame(): the base has no
    // way to learn a real number, and this class is the place that would have to
    // fetch it. The measured value is therefore published through
    // lastGpuTimeMs()/lastGpuTimeValid(), not by rewriting the base's history.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !context_available_ || queries_[current_slot_] == 0) {
        return;
    }

    endActiveQueryLocked();
    // Poll the slot used by the previous frame, whose query has now had a whole
    // frame to retire.
    pollPreviousQueryLocked(current_slot_);
}

} // namespace copper

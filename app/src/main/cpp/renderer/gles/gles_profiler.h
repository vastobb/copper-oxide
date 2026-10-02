#pragma once

#include "profiler.h"

#include <GLES3/gl32.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace copper {

class RendererBase;

// GLES backend for Profiler.
//
// WHAT IS REAL HERE: the history, the statistics and the ring buffer all live
// in Profiler::Impl. This class is the small GL-specific part on top: it knows
// whether the device can time the GPU at all, and if so it runs a double-buffered
// GL_EXT_disjoint_timer_query pair and reports the result.
//
// GLES 3.0 AND 3.1 CORE HAVE NO TIMESTAMP QUERY. A driver only exposes one
// through GL_EXT_disjoint_timer_query (or the WebGL2 variant, which is not
// usable from native code). Therefore GPU time is reported as 0 until that
// extension path is explicitly enabled with enableGpuTimerQueries(), and
// lastGpuTimeValid() stays false until a query result has actually been read.
// RendererBase::endFrame() likewise feeds 0.0 as gpu_time_ms into the base, so
// nothing downstream should treat 0 as "the GPU was instant".
//
// SAFETY RULES for the GL calls below:
//   * no GL entry point is called unless setContextAvailable(true) has been
//     called. Profiler::beginFrame() runs from RendererBase::beginFrame() BEFORE
//     the backend hook that makes the EGL context current, so assuming a current
//     context at begin time would be wrong.
//   * the result of frame N is polled during frame N+1, so the query object is
//     never read while it is still active and the pipeline is never stalled.
//   * two query objects are alternated; a single one would have its result
//     invalidated by the next glBeginQuery.
//
// DEADLOCK NOTE: the base calls the on* hooks while holding its own mutex, so
// no hook here may call back into a public Profiler method (isEnabled(),
// getStats(), ...). The enabled flag is therefore mirrored in enabled_.
class GLESCProfiler : public Profiler {
public:
    GLESCProfiler();
    ~GLESCProfiler() override;

    // The extension that has to be present in the driver's extension string.
    static constexpr const char* k_timer_query_extension = "GL_EXT_disjoint_timer_query";

    // Opt in to GPU timing. Returns false (and logs) when the extension is not
    // advertised, in which case every GPU time stays 0. Safe to call before the
    // EGL context exists: the query object is created lazily on the first frame
    // that runs with a current context.
    bool enableGpuTimerQueries(RendererBase* renderer);
    void disableGpuTimerQueries();

    // True when the driver advertised the timer-query extension.
    bool gpuTimerQueriesAvailable() const;

    // True while queries are enabled AND a context is current, i.e. when real
    // GPU timings are being produced.
    bool gpuTimingActive() const;

    // Gate for every GL call this class makes. The backend has to set it once
    // the context is current and clear it when the context is destroyed or lost
    // (GLESCRenderer::onSurfaceDestroyed / context-loss path).
    void setContextAvailable(bool available);
    bool contextAvailable() const;

    // Last completed query result, in milliseconds. 0.0 when no valid result
    // has been read yet.
    double lastGpuTimeMs() const;

    // False until a query result has been read at least once; distinguishes
    // "0.0 because the GPU was fast" from "0.0 because we cannot measure".
    bool lastGpuTimeValid() const;

    // Last GL error seen by the optional per-frame error drain, or 0.
    GLenum lastGlError() const;

    // Profiler interface overrides. The base implementations are still called,
    // they just also update the backend shadow state.
    void setEnabled(bool enabled) override;
    void reset() override;

    // Profiler::shutdown() is NOT virtual in the base, so this HIDES it instead
    // of overriding it. A call through a Profiler* skips the GL cleanup below;
    // either call it through a GLESCProfiler* or make the base method virtual.
    // Calling it here (before the context goes away) is the only safe moment to
    // delete query objects anyway.
    void shutdown();

protected:
    void onBeginFrame(uint64_t frame_number) override;
    void onEndFrame(uint64_t frame_number, double frame_time_ms, double cpu_time_ms,
                    double gpu_time_ms, uint32_t draw_calls) override;

private:
    // All of the below require mutex_ to be held.
    bool ensureQueriesLocked();
    void endActiveQueryLocked();
    void releaseQueriesLocked();
    void pollPreviousQueryLocked(size_t current_slot);

    // Guards everything below. Never held while calling into the base.
    mutable std::mutex mutex_;
    bool enabled_ = true;
    bool timing_requested_ = false;
    bool extension_present_ = false;
    bool context_available_ = false;
    // Two query objects, used in rotation; see the class comment.
    GLuint queries_[2] = {0, 0};
    size_t current_slot_ = 0;
    bool query_active_ = false;
    bool query_pending_ = false;
    double last_gpu_time_ms_ = 0.0;
    bool last_gpu_time_valid_ = false;
    GLenum last_gl_error_ = 0;
};

} // namespace copper

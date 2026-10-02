#include "gles_command_buffer.h"

#include <android/log.h>

#define LOG_TAG "CopperOxide-GLESCCommandBuffer"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace copper {

GLESCCommandBuffer::GLESCCommandBuffer() = default;
GLESCCommandBuffer::~GLESCCommandBuffer() = default;

void GLESCCommandBuffer::setFinishOnWait(bool finish) {
    std::lock_guard<std::mutex> lock(mutex_);
    finish_on_wait_ = finish;
}

bool GLESCCommandBuffer::finishOnWait() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_on_wait_;
}

void GLESCCommandBuffer::setErrorChecking(bool checking) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_checking_ = checking;
}

bool GLESCCommandBuffer::errorChecking() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_checking_;
}

GLenum GLESCCommandBuffer::lastGlError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_gl_error_;
}

uint64_t GLESCCommandBuffer::beginCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return begin_count_;
}

uint64_t GLESCCommandBuffer::submitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return submit_count_;
}

uint64_t GLESCCommandBuffer::waitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wait_count_;
}

uint64_t GLESCCommandBuffer::finishWaitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_wait_count_;
}

bool GLESCCommandBuffer::isRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

// --- frame lifecycle -------------------------------------------------------

// State reset for the frame. GLES has no recording to undo, so the only real
// state is the shadow bookkeeping below; the per-draw GL state (program,
// bindings, RBO/VAO) is reset by the GLES state manager, not here.
//
// NOTE: no glFlush() here. A flush belongs at the end of a batch of work, and
// flushing twice per frame (here and in submit()) costs a driver round trip for
// nothing.
bool GLESCCommandBuffer::onBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = true;
    last_gl_error_ = 0;
    ++begin_count_;
    drainGlErrorsLocked();
    return true;
}

// Nothing to close: there is no render-pass object, no descriptor set heap and
// no query to end. The base's endRenderPass() is what would normally bracket a
// pass, and on GLES it is the framebuffer manager's glBindFramebuffer that does
// the work.
void GLESCCommandBuffer::onEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    drainGlErrorsLocked();
}

// On GLES the "submission" is a flush: the commands are already in the driver's
// stream, and glFlush() only asks for them to reach the GPU sooner. The real
// present is eglSwapBuffers() in GLESCRenderer::onPresent().
//
// Always returns true, deliberately. CommandBuffer::submit() latches
// pImpl->submitted = true BEFORE calling this hook, so a false here would leave
// the object in a state that can neither be retried nor reset cleanly. A GL
// error at this point does not un-issue the commands either, so it is reported
// in the log instead of faking a failure that did not happen.
bool GLESCCommandBuffer::onSubmit() {
    std::lock_guard<std::mutex> lock(mutex_);
    glFlush();
    ++submit_count_;
    return true;
}

// No stall by default. wait() is one of the few base entry points that calls its
// hook WITHOUT holding Impl::mutex, so glFinish() here is safe with respect to
// the base lock -- the cost is the pipeline stall itself, which is why it is
// opt-in.
void GLESCCommandBuffer::onWait() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++wait_count_;
    if (!finish_on_wait_) {
        return;
    }
    glFinish();
    ++finish_wait_count_;
    LOGD("wait() issued glFinish(): documented full stall");
}

// --- deferred work ---------------------------------------------------------

void GLESCCommandBuffer::defer(std::function<void()> command) {
    if (!command) {
        return;
    }

    // Run it here. The base stores these closures in Impl::deferred_commands and
    // never invokes them, so the closures (and anything they captured by
    // reference) would outlive the frame that created them. On an
    // immediate-mode backend "deferred" can only mean "at record time".
    //
    // The mutex is deliberately NOT held across the call: the closure is
    // arbitrary caller code (it records GL work) and re-entering one of this
    // class's accessors from inside it would self-deadlock on mutex_.
    command();
}

void GLESCCommandBuffer::drainGlErrorsLocked() {
    if (!error_checking_) {
        return;
    }
    // Bounded: a driver in a bad state can report the same error forever.
    for (int i = 0; i < 32; ++i) {
        const GLenum error = glGetError();
        if (error == 0) {
            last_gl_error_ = 0;
            return;
        }
        last_gl_error_ = error;
        LOGW("GL error 0x%x at frame boundary (recording %d)",
             error, recording_ ? 1 : 0);
    }
    LOGW("GL error queue still not empty after 32 reads; giving up");
}

} // namespace copper

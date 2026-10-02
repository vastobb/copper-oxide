#include "gles_missing_es31.h"

#include <EGL/egl.h>

#include <mutex>

namespace copper::gles {

namespace {

// eglGetProcAddr is a loader lookup rather than a driver call, so resolving once
// per process is right. Each entry point gets its own flag: sharing one would
// mean only the first resolution ever ran and the other slot stayed null.
std::once_flag g_get_tex_image_once;
std::once_flag g_get_buffer_sub_data_once;

PFNCO_GLES_GETTEXIMAGE g_get_tex_image = nullptr;
PFNCO_GLES_GETBUFFERSUBDATA g_get_buffer_sub_data = nullptr;

} // namespace

PFNCO_GLES_GETTEXIMAGE getTexImage() {
    std::call_once(g_get_tex_image_once, []() {
        g_get_tex_image =
            reinterpret_cast<PFNCO_GLES_GETTEXIMAGE>(eglGetProcAddress("glGetTexImage"));
    });
    return g_get_tex_image;
}

PFNCO_GLES_GETBUFFERSUBDATA getBufferSubData() {
    std::call_once(g_get_buffer_sub_data_once, []() {
        g_get_buffer_sub_data =
            reinterpret_cast<PFNCO_GLES_GETBUFFERSUBDATA>(eglGetProcAddress("glGetBufferSubData"));
    });
    return g_get_buffer_sub_data;
}

} // namespace copper::gles
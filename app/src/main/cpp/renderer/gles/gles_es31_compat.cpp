#include "gles_es31_compat.h"

#include <EGL/egl.h>

#include <mutex>

namespace copper::gles {

namespace {

// eglGetProcAddr is a loader lookup rather than a driver call, so resolving once
// per process is right. Each entry point gets its own flag: sharing one would
// mean only the first resolution ever ran and every other slot stayed null.

std::once_flag g_get_buffer_sub_data_once;
std::once_flag g_copy_buffer_sub_data_once;
std::once_flag g_get_tex_image_once;
std::once_flag g_copy_image_sub_data_once;

PFNGLGETBUFFERSUBDATAPROC g_get_buffer_sub_data = nullptr;
PFNGLCOPYBUFFERSUBDATAPROC g_copy_buffer_sub_data = nullptr;
PFNGLGETTEXIMAGEPROC g_get_tex_image = nullptr;
PFNGLCOPYIMAGESUBDATAPROC g_copy_image_sub_data = nullptr;

} // namespace

PFNGLGETBUFFERSUBDATAPROC getBufferSubData() {
    std::call_once(g_get_buffer_sub_data_once, []() {
        g_get_buffer_sub_data =
            reinterpret_cast<PFNGLGETBUFFERSUBDATAPROC>(eglGetProcAddress("glGetBufferSubData"));
    });
    return g_get_buffer_sub_data;
}

PFNGLCOPYBUFFERSUBDATAPROC copyBufferSubData() {
    std::call_once(g_copy_buffer_sub_data_once, []() {
        g_copy_buffer_sub_data =
            reinterpret_cast<PFNGLCOPYBUFFERSUBDATAPROC>(eglGetProcAddress("glCopyBufferSubData"));
    });
    return g_copy_buffer_sub_data;
}

PFNGLGETTEXIMAGEPROC getTexImage() {
    std::call_once(g_get_tex_image_once, []() {
        g_get_tex_image = reinterpret_cast<PFNGLGETTEXIMAGEPROC>(eglGetProcAddress("glGetTexImage"));
    });
    return g_get_tex_image;
}

PFNGLCOPYIMAGESUBDATAPROC copyImageSubData() {
    std::call_once(g_copy_image_sub_data_once, []() {
        g_copy_image_sub_data =
            reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(eglGetProcAddress("glCopyImageSubData"));
    });
    return g_copy_image_sub_data;
}

} // namespace copper::gles
#pragma once

// OpenGL ES 3.1 entry points that the Vulkan/GLES headers available to this
// build do not declare, even though every Android libGLESv3.so exports them.
//
// Which functions land here is decided by what the compiler actually rejects, not
// by what the specification says should be available: <GLES3/gl32.h> in the NDK
// sysroot this project builds against declares glCopyImageSubData and
// glShaderBinary but not glGetTexImage or glGetBufferSubData. Declaring the
// missing pair here keeps the difference in one place instead of scattered
// #defines, and each is resolved at run time through eglGetProcAddress so a
// context that cannot reach one degrades to "cannot do this" instead of
// crashing. A null result is a normal answer, never a fatal error.
//
// This header must not redeclare anything the GLES headers already provide: a
// duplicate declaration with a different signature is itself a compile error.

#include <GLES3/gl32.h>

#include <KHR/khrplatform.h>

// GL_SHADER_BINARY_FORMAT_SPIR_V is defined by GL_OES_gl_spirv / GL_ARB_gl_spirv
// rather than by the core ES headers, so it is supplied here under a guard.
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif

extern "C" {

// ES 3.1. Read one mip level of a texture back to host memory.
typedef void(KHRONOS_APIENTRY* PFNCO_GLES_GETTEXIMAGE)(GLenum target, GLint level, GLenum format,
                                                       GLenum type, GLsizei width, GLsizei height,
                                                       GLint border, void* pixels);

// ES 3.1. Read a range of a buffer object back to host memory.
typedef void(KHRONOS_APIENTRY* PFNCO_GLES_GETBUFFERSUBDATA)(GLenum target, GLintptr offset,
                                                            GLsizeiptr size, void* data);

} // extern "C"

namespace copper::gles {

// Null when the current context cannot reach the entry point.
PFNCO_GLES_GETTEXIMAGE getTexImage();
PFNCO_GLES_GETBUFFERSUBDATA getBufferSubData();

} // namespace copper::gles
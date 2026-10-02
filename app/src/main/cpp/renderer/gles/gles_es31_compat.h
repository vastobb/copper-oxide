#pragma once

// OpenGL ES 3.1/3.2 entry points and tokens used by Copper Oxide, declared here.
//
// Two separate problems are solved by this header:
//
//  1. Entry points that the ES 3.1/3.2 specifications define
//     (glGetBufferSubData, glCopyBufferSubData, glGetTexImage,
//     glCopyImageSubData) are not declared by every <GLES3/gl32.h> a build can
//     end up using - notably the one in the Android NDK sysroot this project
//     builds against. Every Android libGLESv3.so exports all four, so the
//     declarations are reproduced from the specifications and resolved at run
//     time through eglGetProcAddress. A null result means the ES version in use
//     is too old, and every call site treats that as "cannot do this" rather than
//     dereferencing it. This is a better check than the extension string anyway,
//     because the string and the reachable entry point can disagree.
//
//  2. A few sized-format tokens (GL_R16, GL_RG16, GL_RGBA16) are likewise absent
//     from that header even though they are ES 3.0 core. Their values are fixed
//     by the ES 3.0 specification, and are defined here only when the header has
//     not already provided them.
//
// This file is safe to include unconditionally: everything is a no-op once a
// complete header is in use. Note that this header deliberately does NOT decide
// the ES version - each manager already caches that through its own
// glesAtLeast(), which correctly invalidates when the context is recreated.

#include <GLES3/gl32.h>

#include <KHR/khrplatform.h>

// --- sized internal formats (ES 3.0 core) ---------------------------------
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RG16
#define GL_RG16 0x822B
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x8058
#endif

// --- copy buffer targets (ES 3.1 core) ------------------------------------
#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif

extern "C" {

// ES 3.1. Read a range of a buffer object back to host memory.
typedef void(KHRONOS_APIENTRY* PFNGLGETBUFFERSUBDATAPROC)(GLenum target, GLintptr offset,
                                                          GLsizeiptr size, void* data);

// ES 3.1. Device-side buffer copy.
typedef void(KHRONOS_APIENTRY* PFNGLCOPYBUFFERSUBDATAPROC)(GLenum readTarget, GLenum writeTarget,
                                                          GLintptr readOffset, GLintptr writeOffset,
                                                          GLsizeiptr size);

// ES 3.1. Read one mip level of a texture back to host memory.
typedef void(KHRONOS_APIENTRY* PFNGLGETTEXIMAGEPROC)(GLenum target, GLint level, GLenum format,
                                                     GLenum type, GLsizei width, GLsizei height,
                                                     GLint border, void* pixels);

// ES 3.2. Device-side image copy.
typedef void(KHRONOS_APIENTRY* PFNGLCOPYIMAGESUBDATAPROC)(GLuint srcTexture, GLint srcLevel,
                                                           GLint srcX, GLint srcY, GLint srcZ,
                                                           GLuint dstTexture, GLint dstLevel,
                                                           GLint dstX, GLint dstY, GLint dstZ,
                                                           GLsizei srcWidth, GLsizei srcHeight,
                                                           GLsizei srcDepth);

} // extern "C"

namespace copper::gles {

// Each of these resolves the entry point once per process and returns null when
// the current context cannot reach it. Null is a normal answer, not an error.
PFNGLGETBUFFERSUBDATAPROC getBufferSubData();
PFNGLCOPYBUFFERSUBDATAPROC copyBufferSubData();
PFNGLGETTEXIMAGEPROC getTexImage();
PFNGLCOPYIMAGESUBDATAPROC copyImageSubData();

} // namespace copper::gles
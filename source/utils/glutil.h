/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

/** Give vitaGL the PHYCONT pool gl_init() held back for the intro video. */
void gl_phycont_late_init();

void gl_swap(int has_common_dialog);

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name);
void glUseProgram_soloader(GLuint program);
void glUniform1f_soloader(GLint loc, GLfloat x);
void glUniform1fv_soloader(GLint loc, GLsizei c, const GLfloat *v);
void glUniform3fv_soloader(GLint loc, GLsizei c, const GLfloat *v);
void glUniform4fv_soloader(GLint loc, GLsizei c, const GLfloat *v);

void glCompileShader_soloader(GLuint shader);
void glLinkProgram_soloader(GLuint program);

void glTexParameterfv_soloader(GLenum target, GLenum pname, const GLfloat *params);
void glGetTexParameteriv_soloader(GLenum target, GLenum pname, GLint *params);
void glGetTexParameterfv_soloader(GLenum target, GLenum pname, GLfloat *params);
GLboolean glIsBuffer_soloader(GLuint buffer);

// Texture-upload diagnostics (Fase 15, temporal): pure parameter logging,
// NO glGetError inside (consuming it would break the engine's own upload
// error detection in CTexture::updateData). Remapped in dynlib.c.
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void *data);
void glCompressedTexImage2D_soloader(GLenum target, GLint level,
    GLenum internalformat, GLsizei width, GLsizei height, GLint border,
    GLsizei imageSize, const void *data);
void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
    GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *data);
void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param);
void glGenerateMipmap_soloader(GLenum target);
void glPixelStorei_soloader(GLenum pname, GLint param);
void glBindTexture_soloader(GLenum target, GLuint texture);
void glActiveTexture_soloader(GLenum texture);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

// Missing in system vitaGL headers: empty stubs, same as DH2 port
// (Dungeon-Hunter-2-vita/source/utils/glutil.c). Kept as no-ops until the
// hardware log shows the engine actually calling them.
void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset,
    GLint yoffset, GLsizei width, GLsizei height, GLenum format,
    GLsizei imageSize, const void *data);
void glDetachShader(GLuint program, GLuint shader);
void glValidateProgram(GLuint program);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H

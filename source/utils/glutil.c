/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/settings.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);
extern GLboolean fast_texture_compression; // vitaGL misc.c

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // No MSAA: the Android EGL config (GL2JNIView ConfigChooser) asks for
    // RGB565 / depth 16 / stencil 8 with no sample buffers, so 4x MSAA was
    // pure VRAM + fill-rate cost the original never paid (DH2 ended on
    // SCE_GXM_MULTISAMPLE_NONE too). DH2's custom CDRAM/PHYCONT thresholds
    // were for SceAvPlayer's intro video; DH3 has no video path at all.
    // Same pools as vglInitExtended(0, 960, 544, 6 MB, NONE) except PHYCONT:
    // its threshold is "everything", so vitaGL takes none of it yet. The
    // intro's SceAvPlayer decoder needs ~14 MB of physically contiguous RAM
    // (log_015/016: plain heap is accepted but the decoder STOPs with 0
    // frames); gl_phycont_late_init() hands PHYCONT to vitaGL after it.
    vglInitWithCustomThreshold(0, 960, 544, 6 * 1024 * 1024, 0, 0x7FFFFFFF,
                               0x8C6000, // SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE (vitaGL mem_utils.h): no CDLG pool, as vglInitExtended
                               SCE_GXM_MULTISAMPLE_NONE);

    // ~95% of DH3's textures are ETC1 (log_015: 108 of 114 uploads, 35 MB).
    // vitaGL is built with SAFE_ETC1=1 (no GXM hardware ETC1, which rendered
    // them broken), so it decodes them on the CPU; NICEST makes it recompress
    // to DXT1 (same 4 bpp as ETC1) instead of keeping 24-bit RGB (x6 memory).
    // fast_texture_compression keeps that recompression at stb_dxt's normal
    // quality so level loads don't stall.
    glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
    fast_texture_compression = GL_TRUE;
}

void gl_phycont_late_init() {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) return;
    size_t size = info.size_phycont & ~(size_t) (1024 * 1024 - 1);  // vitaGL rounds up
    if (size) vglPhycontMemLazyInit(size);
    l_info("[gl] PHYCONT pool handed to vitaGL after the intro: %u KB.", (unsigned) (size / 1024));
}

void gl_swap(int has_common_dialog) {
    vglSwapBuffers(has_common_dialog ? GL_TRUE : GL_FALSE);
}

void glTexParameterfv_soloader(GLenum target, GLenum pname, const GLfloat *params) {
    if (params) glTexParameterf(target, pname, params[0]);
}

static GLint default_tex_param(GLenum pname) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: return GL_NEAREST_MIPMAP_LINEAR;
        case GL_TEXTURE_MAG_FILTER: return GL_LINEAR;
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T: return GL_REPEAT;
        default: return 0;
    }
}

void glGetTexParameteriv_soloader(GLenum target, GLenum pname, GLint *params) {
    (void)target;
    if (params) *params = default_tex_param(pname);
}

void glGetTexParameterfv_soloader(GLenum target, GLenum pname, GLfloat *params) {
    (void)target;
    if (params) *params = (GLfloat)default_tex_param(pname);
}

GLboolean glIsBuffer_soloader(GLuint buffer) {
    return buffer != 0 ? GL_TRUE : GL_FALSE;
}

// Missing GL functions in system vitaGL headers: empty stubs, same as the
// DH2 sibling port. DH2's aquamarine-screen investigation proved these are
// safe as no-ops until the hardware log shows otherwise.
void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {}
// glitch CTexture::updateData (0xa78f..): first upload is
// glCompressedTexImage2D, later re-uploads of a dirty level go through
// glCompressedTexSubImage2D with offset (0,0) and the full level size.
// vitaGL has no compressed sub-upload, so a whole-level update is
// re-specified with glCompressedTexImage2D (same result); a true partial
// update can't be expressed and is logged once.
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data) {
    if (xoffset == 0 && yoffset == 0) {
        glCompressedTexImage2D(target, level, format, width, height, 0, imageSize, data);
        return;
    }
    static int warned = 0;
    if (!warned) {
        warned = 1;
        l_warn("glCompressedTexSubImage2D partial update (%d,%d %dx%d fmt 0x%x) unsupported, skipped",
               xoffset, yoffset, width, height, format);
    }
}
void glDetachShader(GLuint program, GLuint shader) {}

void glLinkProgram_soloader(GLuint program) {
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLchar info[1024];
        GLsizei info_len = 0;
        glGetProgramInfoLog(program, sizeof(info), &info_len, info);
        l_error("glLinkProgram(%u) FAILED: %s", program, info);
    }
}
void glValidateProgram(GLuint program) {}

// ---- Texture-upload diagnostics (Fase 15, temporal) ----
// Character select renders geometry but textures are missing (log_011
// reaches frame 600+, no shader FAILED, files exist in the OBB). These
// wrappers only LOG parameters and call through; they never call
// glGetError (the engine drains/checks it itself in updateData).
static unsigned s_teximg_n = 0, s_texcomp_n = 0, s_texpar_n = 0;
static unsigned s_genmip_n = 0, s_texsub_n = 0, s_pixstore_unpack = 0;

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void *data) {
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, data);
    if (level == 0) {
        s_teximg_n++;
        l_debug("[texdiag] glTexImage2D lvl=0 ifmt=0x%x %dx%d fmt=0x%x type=0x%x %s (#%u)",
                internalformat, width, height, format, type,
                data ? "data" : "NULL", s_teximg_n);
    }
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level,
    GLenum internalformat, GLsizei width, GLsizei height, GLint border,
    GLsizei imageSize, const void *data) {
    glCompressedTexImage2D(target, level, internalformat, width, height,
                            border, imageSize, data);
    if (level > 0 && s_texcomp_n <= 3)
        l_debug("[texdiag] glCompressedTexImage2D lvl=%d ifmt=0x%x %dx%d size=%d",
                level, internalformat, width, height, imageSize);
    if (level == 0) {
        s_texcomp_n++;
        l_debug("[texdiag] glCompressedTexImage2D lvl=0 ifmt=0x%x %dx%d size=%d %s (#%u)",
                internalformat, width, height, imageSize,
                data ? "data" : "NULL", s_texcomp_n);
    }
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
    GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *data) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                     format, type, data);
    if (s_texsub_n < 10) {
        s_texsub_n++;
        l_debug("[texdiag] glTexSubImage2D lvl=%d off=(%d,%d) %dx%d fmt=0x%x type=0x%x",
                level, xoffset, yoffset, width, height, format, type);
    }
}

void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param) {
    glTexParameteri(target, pname, param);
    // Only MIN/MAG filter + wrap modes matter for the incomplete-texture
    // (black) hypothesis; other pnames are noise.
    if ((pname == 0x2801 || pname == 0x2800 || pname == 0x2802 || pname == 0x2803) &&
        s_texpar_n < 30) {
        s_texpar_n++;
        l_debug("[texdiag] glTexParameteri pname=0x%x param=0x%x", pname, param);
    }
}

void glGenerateMipmap_soloader(GLenum target) {
    glGenerateMipmap(target);
    if (s_genmip_n < 5) {
        s_genmip_n++;
        l_debug("[texdiag] glGenerateMipmap called (total so far: %u)", s_genmip_n);
    } else {
        s_genmip_n++;
    }
}

void glPixelStorei_soloader(GLenum pname, GLint param) {
    glPixelStorei(pname, param);
    if (pname == 0x0CF5 && param != (GLint) s_pixstore_unpack) {
        s_pixstore_unpack = (unsigned) param;
        l_debug("[texdiag] GL_UNPACK_ALIGNMENT -> %d", param);
    }
}

static unsigned s_bind_n = 0, s_active_n = 0;

void glBindTexture_soloader(GLenum target, GLuint texture) {
    glBindTexture(target, texture);
    if (s_bind_n < 40) {
        s_bind_n++;
        l_debug("[texdiag] glBindTexture(0x%x, %u)", target, texture);
    }
}

void glActiveTexture_soloader(GLenum texture) {
    glActiveTexture(texture);
    if (s_active_n < 10) {
        s_active_n++;
        l_debug("[texdiag] glActiveTexture(0x%x)", texture);
    }
}

// Called by the vendored vitaGL (lib/vitagl, [DH3] patch in compile_shader)
// when shacccg rejects a shader; vitaGL then compiles a magenta/degenerate
// stand-in so glLinkProgram can't hit a NULL program (log_006 crash).
// `source` is vitaGL's GLSL->CG translation: the shacccg line numbers refer
// to it, so the full text is saved next to the log.
void vgl_shader_compile_failed(GLboolean is_fragment, const char *source,
                               uint32_t size, const char *log) {
    static int n = 0;
    char path[256];
    snprintf(path, sizeof(path), DATA_PATH "shader_fail/%03d_%s.cg", n++,
             is_fragment ? "fs" : "vs");
    if (source) {
        file_mkpath(path, 0777);
        file_save(path, (const uint8_t *) source, size);
    }
    l_error("[shader] %s shader FAILED to compile -> stand-in used. Source: %s",
            is_fragment ? "fragment" : "vertex", path);
    l_error("[shader] shacccg: %s", log ? log : "(no log)");
}

/*
 * Source-level fixes for genuine bugs in DH3's own GLSL (shaders.pak).
 * Edits blank statements out with spaces so the length never changes.
 *
 * GL_Diffuse_L1_iPhone_FS / CharNormalSpec_iPhone_FS, SP+TB variants:
 *   Specular = Specular * BlendRatio.x + Specular2 * BlendRatio.y;
 *   Specular += Specular3 * BlendRatio.z;
 * BlendRatio/Specular2/Specular3 are declared nowhere (the TB blend is dead
 * code in the body too), so shacccg rejects them (log_007: "undeclared
 * identifier 'BlendRatio'"); no GLES compiler would accept them either.
 * Dropping both statements keeps the single specular map, the only one
 * the shader actually samples.
 */
static void blank_statement(char *src, const char *stmt) {
    char *p;
    size_t n = strlen(stmt);
    while ((p = strstr(src, stmt)) != NULL) memset(p, ' ', n);
}

// Same-length in-place replace (pads with spaces), every occurrence.
static void replace_statement(char *src, const char *stmt, const char *with) {
    char *p;
    size_t n = strlen(stmt), m = strlen(with);
    if (m > n) return;
    while ((p = strstr(src, stmt)) != NULL) {
        memcpy(p, with, m);
        memset(p + m, ' ', n - m);
    }
}

void dh3_fix_shader_source(char *src) {
    // config.txt setting_dbgNoIrr (hero diag): drop the IR lighting term.
    if (setting_dbgNoIrr && strstr(src, "ird_boost")) {
        replace_statement(src, "vec3 irrColor = ird_boost * (x1 + x2);", "vec3 irrColor = vec3(0.0);");
        static int logged = 0;
        if (!logged++) l_warn("[irdiag] setting_dbgNoIrr: IR term removed from CharNormalSpec_VS.");
    }
    if (strstr(src, "BlendRatio") && !strstr(src, "vec4 BlendRatio") &&
        !strstr(src, "vec3 BlendRatio")) {
        blank_statement(src, "Specular = Specular * BlendRatio.x + Specular2 * BlendRatio.y;");
        blank_statement(src, "Specular += Specular3 * BlendRatio.z;");
        static int logged = 0;
        if (!logged++) l_info("[shader] patched undeclared BlendRatio specular blend (SP+TB variants).");
    }
}

/*
 * Hero diag (log_022/024): only the player's armour renders as a flat beige
 * "mannequin"; its textures load fine, the NULL guards of material.c never
 * fire, enemies (same CharNormalSpec shader) look right. The one player-only
 * shader path is the IR (iridescence) variant: irrColor = ird_boost * SH(ird_c*)
 * is added to the vertex lighting, so absurd ird_* values would wash the
 * diffuse out. Log which CharNormalSpec variants get compiled and the ird_*
 * values the engine uploads (bounded).
 */
#define UW_MAX 96
static struct { GLuint prog; GLint loc; char name[28]; } uw[UW_MAX];
static int uw_n = 0, uw_logs = 0;
static GLuint uw_prog = 0;

static int uw_watch(const char *name) {
    return strncmp(name, "ird", 3) == 0 || strcmp(name, "AmbientLightingControl") == 0 ||
           strcmp(name, "Glossiness") == 0;
}

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name) {
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0 && name && uw_watch(name) && uw_n < UW_MAX) {
        uw[uw_n].prog = program;
        uw[uw_n].loc = loc;
        snprintf(uw[uw_n].name, sizeof(uw[uw_n].name), "%s", name);
        uw_n++;
    }
    return loc;
}

void glUseProgram_soloader(GLuint program) {
    uw_prog = program;
    glUseProgram(program);
}

static const char *uw_name(GLint loc) {
    if (uw_logs >= 80) return NULL;
    for (int i = 0; i < uw_n; i++)
        if (uw[i].prog == uw_prog && uw[i].loc == loc) return uw[i].name;
    return NULL;
}

static void uw_log(GLint loc, int comps, GLsizei count, const GLfloat *v) {
    const char *n = uw_name(loc);
    if (!n || !v) return;
    uw_logs++;
    l_info("[irdiag] prog %u %s = (%g %g %g %g)%s", uw_prog, n, v[0],
           comps > 1 ? v[1] : 0.0, comps > 2 ? v[2] : 0.0, comps > 3 ? v[3] : 0.0,
           count > 1 ? " [array]" : "");
}

void glUniform1f_soloader(GLint loc, GLfloat x) { uw_log(loc, 1, 1, &x); glUniform1f(loc, x); }
void glUniform1fv_soloader(GLint loc, GLsizei c, const GLfloat *v) { uw_log(loc, 1, c, v); glUniform1fv(loc, c, v); }
void glUniform3fv_soloader(GLint loc, GLsizei c, const GLfloat *v) { uw_log(loc, 3, c, v); glUniform3fv(loc, c, v); }
void glUniform4fv_soloader(GLint loc, GLsizei c, const GLfloat *v) { uw_log(loc, 4, c, v); glUniform4fv(loc, c, v); }

// Leading "#define"s glitch prepends = the variant. Logged once each.
static void log_charspec_variant(const char *src) {
    static uint32_t seen[64];
    static int nseen = 0;
    int vs = strstr(src, "ird_cAr") != NULL, fs = strstr(src, "irdSpecIntensity") != NULL;
    if (!vs && !fs) return;
    char defs[160];
    size_t n = 0;
    const char *p = src;
    while (strncmp(p, "#define ", 8) == 0 && n < sizeof(defs) - 32) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        size_t len = (size_t) (e - p - 8);
        if (len > 24) len = 24;
        memcpy(defs + n, p + 8, len);
        n += len;
        defs[n++] = ' ';
        p = e + 1;
    }
    defs[n] = '\0';
    uint32_t h = 2166136261u ^ (uint32_t) vs;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t) defs[i]) * 16777619u;
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return;
    if (nseen < 64) seen[nseen++] = h;
    l_info("[irdiag] CharNormalSpec %s variant: %s", vs ? "VS" : "FS", n ? defs : "(no defines)");
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    dh3_fix_shader_source(str);
    log_charspec_variant(str);
    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);

        // Same check as DH2 (its log_058 proved this is the line to look for):
        // vitaGL's GLSL->CG translator + shacccg errors only surface here.
        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status == GL_FALSE) {
            GLchar info[1024];
            GLsizei info_len = 0;
            glGetShaderInfoLog(shader, sizeof(info), &info_len, info);
            l_error("glCompileShader(%u) FAILED (%s): %s", shader,
                    next_shader_fname[0] ? next_shader_fname : "?", info);
        }
#ifdef DUMP_COMPILED_SHADERS
        // Cache ONLY good binaries: load_shader() prefers a cached .gxp over
        // the source on every later boot, so caching a failed compile would
        // make that shader permanently broken until the file is deleted.
        else if (next_shader_fname[0]) {
            void *bin = vglMalloc(64 * 1024);
            GLsizei len = 0;
            vglGetShaderBinary(shader, 64 * 1024, &len, bin);
            if (len > 0) {
                file_mkpath(next_shader_fname, 0777);
                file_save(next_shader_fname, bin, len);
            }
            vglFree(bin);
        }
        next_shader_fname[0] = '\0';
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif

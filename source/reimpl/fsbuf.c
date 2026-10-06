/*
 * fsbuf.c -- guard for FileSystemBase::customCreateAndOpenFile's 512-byte
 * stack path buffer.
 *
 * Root-caused from log_009 + psp2dmp 1790991269 (2026-10-02), ARM disassembly
 * (NOT a guess):
 *
 *   FileSystemBase::customCreateAndOpenFile (0x610eb4, ARM) builds the file
 *   path in a 512-byte stack buffer and protects it with __stack_chk_guard:
 *
 *     610eb8: sub sp, sp, #524          ; frame: buf at sp+4, canary at sp+516
 *     610ee8: bl memset                ; memset(buf, 0, 512)
 *     610ef8: bl strcpy@plt            ; strcpy(buf, path)      <- UNBOUNDED
 *     ...
 *     611040: ldr r1, [pc, #20]        ; suffix via GOT (only when the name
 *     61104c: bl strcat@plt            ;  ends in "swf": strcat(buf, suffix)
 *     ...
 *     610f94: bne 611054               ; canary check at every exit
 *     611054: bl __stack_chk_fail@plt
 *
 *   At 33.2 s the canary check failed ("Stack collapsed at 0x98611058",
 *   return address = the word right after the bl at 0x611054). All logged
 *   paths are <= 107 chars, so the overflowing input never reaches our
 *   fopen/stat wrappers -- it dies inside this function first. The bounded
 *   replacements below truncate + log the culprit (length + head) so the
 *   next console log identifies it, and the engine gets the same graceful
 *   "file not found" it already tolerates everywhere (all those stat -1 /
 *   fopen 0x0 warnings).
 *
 *   Technique: redirect the two BL sites to absolute-call trampolines in
 *   so_util's arena (a direct BL cannot reach the loader: .so text is at
 *   0x98xxxxxx, loader at 0x81xxxxxx, BL range is +-32 MB). The trampoline
 *   sets lr = site+4 and bx's to the C replacement, which is ABI-identical
 *   to the libc call it replaces (args in r0-r3, result in r0, callee-saved
 *   regs preserved, ip clobber allowed -- a plain BL would clobber it too).
 */

#include <so_util/so_util.h>
#include <kubridge.h>
#include <stdint.h>
#include <string.h>

#include "utils/logger.h"

extern so_module so_mod;

// Not exported by so_util.h, but non-static in so_util.c (same as patch.c).
uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

#define FSBUF_CPY_OFF   0x610ef8u   // bl strcpy@plt  (strcpy(buf, path))
#define FSBUF_CPY_FIRST 0xebf3c453u
#define FSBUF_CAT_OFF   0x61104cu   // bl strcat@plt  (strcat(buf, suffix))
#define FSBUF_CAT_FIRST 0xebf3c437u

// The engine buffer is 512 bytes with room for the NUL: never let the total
// (input + any "swf" suffix the strcat site appends) exceed this.
#define FSBUF_CAP 511u

static unsigned s_trunc_reports = 0;

char *dh3_strcpy512(char *dst, const char *src) {
    size_t i = 0;
    while (i < FSBUF_CAP && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    if (src[i] && s_trunc_reports++ < 8)
        l_error("[fsbuf] strcpy saturated the 512-byte stack buf (%u chars kept, src longer): %.160s...",
                (unsigned) i, src);
    return dst;
}

char *dh3_strcat_lim(char *dst, const char *src) {
    size_t d = strlen(dst), i = 0;
    while (d + i < FSBUF_CAP && src[i]) {
        dst[d + i] = src[i];
        i++;
    }
    dst[d + i] = '\0';
    if (src[i] && s_trunc_reports++ < 8)
        l_error("[fsbuf] strcat saturated the 512-byte stack buf (%u chars kept): dst=%.120s... suffix=%.64s...",
                (unsigned) (d + i), dst, src);
    return dst;
}

#define ARM_B(from, to) (0xEA000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00FFFFFFu))

// Trampoline (all ARM, hand-encoded so it works regardless of the ISA the
// loader itself was compiled to -- bx interworks both ways):
//   w0: ldr lr, [pc, #8]   lr = [tramp+16] = site+4 (the real return address)
//   w1: ldr ip, [pc, #8]   ip = [tramp+20] = replacement
//   w2: bx ip
//   w3: padding
//   w4: site+4 (absolute, runtime)
//   w5: replacement (absolute)
// NOTE: LDR [pc] encodes Rn=0xF (0xE59F....). 0xE59D.... is [sp] and
// 0xE59C.... is [ip] -- getting the nibble wrong jumps to garbage (this
// exact typo killed the first boot with the hook: Data abort at 5.27 s,
// log_010). Verified against the engine's own `ldr r6, [pc, #404]`
// (0xE59F6194) in the same function.
static int redirect_bl(uint32_t off, uint32_t first_word, void *dst, const char *what) {
    uintptr_t site = so_mod.text_base + off;
    if (*(uint32_t *) site != first_word) {
        l_error("[fsbuf] %s: unexpected code 0x%08x at 0x%x (expected 0x%08x), skipped.",
                what, *(uint32_t *) site, off, first_word);
        return 0;
    }
    uint32_t code[6];
    code[0] = 0xE59FE008u;   // ldr lr, [pc, #8]
    code[1] = 0xE59FC008u;   // ldr ip, [pc, #8]
    code[2] = 0xE12FFF1Cu;   // bx ip
    code[3] = 0x00000000u;
    code[4] = (uint32_t) (site + 4);
    code[5] = (uint32_t) (uintptr_t) dst;

    uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, site + 8, sizeof(code));
    if (!tramp) {
        l_error("[fsbuf] %s: arena full, skipped.", what);
        return 0;
    }
    uint32_t jump = ARM_B(site, tramp);
    kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
    kuKernelCpuUnrestrictedMemcpy((void *) site, &jump, sizeof(jump));
    l_debug("[fsbuf] %s: site=0x%08x tramp=0x%08x -> %p", what, site, tramp, dst);
    return 1;
}

void patch_fsbuf(void) {
    int ok = 0;
    ok += redirect_bl(FSBUF_CPY_OFF, FSBUF_CPY_FIRST, dh3_strcpy512, "customCreateAndOpenFile:strcpy");
    ok += redirect_bl(FSBUF_CAT_OFF, FSBUF_CAT_FIRST, dh3_strcat_lim, "customCreateAndOpenFile:strcat");
    if (ok == 2)
        l_success("Guarded customCreateAndOpenFile 512-byte stack buf (strcpy+strcat bounded).");
}

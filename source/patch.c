/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/utils.h"
#include "utils/logger.h"
#include "utils/dialog.h"

#include <string.h>

extern so_module so_mod;

// Not exported by so_util.h, but non-static in so_util.c.
void patch_basic_file(void);  // reimpl/basic_file.c
void patch_pak(void);         // reimpl/pak.c
void patch_fsbuf(void);       // reimpl/fsbuf.c
void patch_material(void);    // reimpl/material.c

uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

/*
 * SWP -> LDREX/STREX.
 *
 * libDH3.so was built for ARMv5 atomics: glitch/gameswf spinlocks use inline
 * `swp r3, Rx, [Ry]` (178 sites, e.g. ~CMemoryReadFile 0xa2c7a4). Android
 * emulates SWP in the kernel; on Vita it is an Undefined instruction
 * (psp2dmp 2026-10-02, log_003, while loading menus854x480.pak).
 *
 * Each site becomes `b tramp`, and the trampoline does the same atomic swap
 * the ARMv7 way, preserving every register but Rt and the flags:
 *     push  {S, F}
 *     mrs   F, APSR
 *     dmb   ish
 *  1: ldrex Rt, [Rn]
 *     strex S, Rt2, [Rn]
 *     cmp   S, #0
 *     bne   1b
 *     dmb   ish
 *     msr   APSR_nzcvq, F
 *     pop   {S, F}
 *     b     site + 4
 * Offline check: a raw scan of .text with this mask finds exactly the same
 * 178 addresses objdump decodes as swp (no literal-pool false positives),
 * all unconditional, never swpb, Rt != Rt2 != Rn.
 */
#define SWP_MASK  0x0FB00FF0u
#define SWP_MATCH 0x01000090u
#define ARM_B(from, to) (0xEA000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00FFFFFFu))

static int patch_swp(uint32_t *site) {
    uint32_t ins = *site;
    uint32_t rt = (ins >> 12) & 15, rt2 = ins & 15, rn = (ins >> 16) & 15;

    // two scratch regs outside {Rt, Rt2, Rn}
    uint32_t sc[2], n = 0;
    for (uint32_t r = 0; r <= 12 && n < 2; r++)
        if (r != rt && r != rt2 && r != rn) sc[n++] = r;
    uint32_t S = sc[0], F = sc[1], mask = (1u << S) | (1u << F);

    uint32_t code[11];
    code[0]  = 0xE92D0000u | mask;                          // push {S, F}
    code[1]  = 0xE10F0000u | (F << 12);                     // mrs F, APSR
    code[2]  = 0xF57FF05Bu;                                 // dmb ish
    code[3]  = 0xE1900F9Fu | (rn << 16) | (rt << 12);       // ldrex Rt, [Rn]
    code[4]  = 0xE1800F90u | (rn << 16) | (S << 12) | rt2;  // strex S, Rt2, [Rn]
    code[5]  = 0xE3500000u | (S << 16);                     // cmp S, #0
    code[6]  = 0x1A000000u | ((uint32_t)(-5) & 0x00FFFFFFu);// bne code[3]
    code[7]  = 0xF57FF05Bu;                                 // dmb ish
    code[8]  = 0xE128F000u | F;                             // msr APSR_nzcvq, F
    code[9]  = 0xE8BD0000u | mask;                          // pop {S, F}

    uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, (uintptr_t) site + 8, sizeof(code));
    if (!tramp) return 0;
    code[10] = ARM_B(tramp + 40, (uintptr_t) site + 4);     // b back

    uint32_t jump = ARM_B((uintptr_t) site, tramp);
    kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
    kuKernelCpuUnrestrictedMemcpy(site, &jump, sizeof(jump));
    return 1;
}

static void patch_all_swp(void) {
    // Whole RX segment, not just .text: so_util frees the file image (and
    // with it so_mod.shdr) at the end of so_file_load. Offline check: the
    // segment has the same 178 matches as .text alone.
    uint32_t *p = (uint32_t *) so_mod.text_base;
    uint32_t *end = (uint32_t *)(so_mod.text_base + so_mod.text_size);
    int found = 0, patched = 0;
    for (; p < end; p++) {
        if ((*p & SWP_MASK) == SWP_MATCH && (*p >> 28) == 0xE) {
            found++;
            patched += patch_swp(p);
        }
    }
    if (patched == found)
        l_success("Patched %d SWP -> LDREX/STREX.", patched);
    else
        fatal_error("SWP patch: only %d of %d sites patched (arena full).", patched, found);
}

void so_patch(void) {
    // DRM: GDRMPolicy.nativeAllow(long)->int, licensed iff == 1
    // (installer/GDRMPolicy.java:86). Forced licensed; server gone anyway.
    // nativeStart()/initNative()/initNativeAP() deliberately UNTOUCHED:
    // they may perform real boot init beyond the license check.
    uintptr_t allow = so_symbol(&so_mod,
        "Java_com_gameloft_android_ANMP_GloftD3HM_installer_GDRMPolicy_nativeAllow");
    if (allow) {
        hook_addr(allow, (uintptr_t) &ret1);
        l_success("Patched GDRMPolicy.nativeAllow -> licensed.");
    } else {
        l_warn("GDRMPolicy.nativeAllow not found, DRM bypass skipped.");
    }

    patch_all_swp();
    patch_basic_file();
    patch_pak();
    patch_fsbuf();
    patch_material();

    // NOTE (2026-10-01): libDH3.so has NO boost sp_counted_base symbols
    // (different STL than DH2), but its SWP atomics are INLINE (see
    // patch_all_swp). No ALicenseCheck class either (DH2-style hooks N/A); the single BSS
    // `LicenseCheck` byte is driven by the GDRMPolicy path above.
}

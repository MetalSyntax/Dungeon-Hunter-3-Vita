/*
 * material.c -- NULL-tolerant material layer and vertex streams handling in
 * CColladaFactory::createMaterialVertexAttributeMap and
 * CVertexAttributeMap::set.
 *
 * Root-caused from:
 * 1. log_012 + psp2dmp 1791002796 (2026-10-03):
 *    In createMaterialVertexAttributeMap+0x164:
 *      87835c: add r0, sp, #36     ; out-param slot
 *      878370: blx r3              ; virtual layer query (getMeshBuffer)
 *      878374: ldr r0, [sp, #36]
 *      878378: ldr r3, [r0, #8]    <- Data abort here, r0 == 0
 *      87837c: cmp r3, #0
 *      878380: str r3, [sp, #32]
 *      878384: beq 878394          ; skips add_ref, keeps building map.
 *    Fix: r0 == 0 -> r3 = 0, routing into the legal "no streams" path.
 *
 * 2. log_013 + psp2dmp 1791003100 (2026-10-03):
 *    With r3 == 0 (no vertex streams in this buffer), [sp, #32] is 0.
 *    createMaterialVertexAttributeMap then calls:
 *      878444: bl 96cff0 <CVertexAttributeMap::set>
 *    passing r1 = sp + 32 (where *r1 == 0).
 *    Inside CVertexAttributeMap::set:
 *      96cff0: push {r4, r5, r6, r7, r8, r9, sl, fp}
 *      ...
 *      96d008: ldr r9, [r1]        ; r9 = *r1 = 0
 *      ...
 *      96d018: ldr r8, [r9, #16]   <- Data abort here (PC 0x9896d018)
 *
 *    Notice that CVertexAttributeMap's delegating ctor at 0x96cf58 already
 *    explicitly checks if *r1 == 0 (cmp r3, #0; beq 96cf80) to skip calling set().
 *    However, createMaterialVertexAttributeMap calls set() directly, and set()
 *    failed to check if *r1 != 0 before reading r9[16].
 *
 *    Fix: trampoline at 0x96cff0 checks if r1 == NULL || *r1 == NULL.
 *    If NULL, returns immediately via `bx lr` (void function, no regs pushed).
 *    Otherwise executes `push {r4-r9, sl, fp}` and branches to 0x96cff4.
 */

#include <so_util/so_util.h>
#include <kubridge.h>
#include <stdint.h>

#include "utils/logger.h"

extern so_module so_mod;

// Not exported by so_util.h, but non-static in so_util.c (same as patch.c).
uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

#define MATLAYER_OFF   0x878378u   // ldr r3, [r0, #8] in createMaterialVertexAttributeMap+0x164
#define MATLAYER_FIRST 0xE5903008u
#define MATLAYER_CONT  0x87837Cu

#define VAMAP_SET_OFF   0x96CFF0u  // push {r4, r5, r6, r7, r8, r9, sl, fp} in CVertexAttributeMap::set
#define VAMAP_SET_FIRST 0xE92D0FF0u
#define VAMAP_SET_CONT  0x96CFF4u

// Hit counters for the two NULL paths (log_022: hero still flat-shaded;
// if these fire, its mesh buffers are still missing when the material's
// vertex attribute map is built). Reported by material_report() from the
// main loop.
volatile uint32_t dh3_mat_hits[2];

#define ARM_B(from, to) (0xEA000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00FFFFFFu))

void patch_material(void) {
    // 1. Guard createMaterialVertexAttributeMap NULL layer
    uintptr_t site = so_mod.text_base + MATLAYER_OFF;
    if (*(uint32_t *) site != MATLAYER_FIRST) {
        l_error("[material] unexpected code 0x%08x at 0x%x (expected 0x%08x), skipped.",
                *(uint32_t *) site, MATLAYER_OFF, MATLAYER_FIRST);
    } else {
        uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, site + 8, 11 * 4);
        if (!tramp) {
            l_error("[material] arena full, skipped MATLAYER.");
        } else {
            // NULL layer: count the hit, leave r0 = r3 = 0 as before.
            uint32_t code[11];
            code[0]  = 0xE3500000u;  // cmp   r0, #0
            code[1]  = 0x15903008u;  // ldrne r3, [r0, #8]
            code[2]  = 0x1A000005u;  // bne   code[9]
            code[3]  = 0xE59F3014u;  // ldr   r3, [pc, #20]  -> code[10]
            code[4]  = 0xE5930000u;  // ldr   r0, [r3]
            code[5]  = 0xE2800001u;  // add   r0, r0, #1
            code[6]  = 0xE5830000u;  // str   r0, [r3]
            code[7]  = 0xE3A00000u;  // mov   r0, #0
            code[8]  = 0xE3A03000u;  // mov   r3, #0
            code[9]  = ARM_B(tramp + 36, so_mod.text_base + MATLAYER_CONT);
            code[10] = (uint32_t) (uintptr_t) &dh3_mat_hits[0];
            uint32_t jump = ARM_B(site, tramp);
            kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
            kuKernelCpuUnrestrictedMemcpy((void *) site, &jump, sizeof(jump));
            l_success("Guarded createMaterialVertexAttributeMap NULL layer (ldr->cond trampoline).");
        }
    }

    // 2. Guard CVertexAttributeMap::set against NULL stream (*r1 == NULL)
    uintptr_t vamap_site = so_mod.text_base + VAMAP_SET_OFF;
    if (*(uint32_t *) vamap_site != VAMAP_SET_FIRST) {
        l_error("[material] unexpected code 0x%08x at 0x%x (expected 0x%08x), skipped VAMAP_SET.",
                *(uint32_t *) vamap_site, VAMAP_SET_OFF, VAMAP_SET_FIRST);
    } else {
        uintptr_t vamap_tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, vamap_site + 8, 15 * 4);
        if (!vamap_tramp) {
            l_error("[material] arena full for VAMAP_SET, skipped.");
        } else {
            // NULL streams: count the hit (ip is call-clobbered) and return.
            uint32_t vcode[15];
            vcode[0]  = 0xE3510000u;    // cmp   r1, #0
            vcode[1]  = 0x0A000004u;    // beq   vcode[7]
            vcode[2]  = 0xE591C000u;    // ldr   ip, [r1]
            vcode[3]  = 0xE35C0000u;    // cmp   ip, #0
            vcode[4]  = 0x0A000001u;    // beq   vcode[7]
            vcode[5]  = VAMAP_SET_FIRST;// push  {r4-fp}
            vcode[6]  = ARM_B(vamap_tramp + 24, so_mod.text_base + VAMAP_SET_CONT);
            vcode[7]  = 0xE52D0004u;    // push  {r0}
            vcode[8]  = 0xE59FC010u;    // ldr   ip, [pc, #16] -> vcode[14]
            vcode[9]  = 0xE59C0000u;    // ldr   r0, [ip]
            vcode[10] = 0xE2800001u;    // add   r0, r0, #1
            vcode[11] = 0xE58C0000u;    // str   r0, [ip]
            vcode[12] = 0xE49D0004u;    // pop   {r0}
            vcode[13] = 0xE12FFF1Eu;    // bx    lr
            vcode[14] = (uint32_t) (uintptr_t) &dh3_mat_hits[1];

            uint32_t jump = ARM_B(vamap_site, vamap_tramp);
            kuKernelCpuUnrestrictedMemcpy((void *) vamap_tramp, vcode, sizeof(vcode));
            kuKernelCpuUnrestrictedMemcpy((void *) vamap_site, &jump, sizeof(jump));
            l_success("Guarded CVertexAttributeMap::set against NULL streams (*r1 == NULL -> bx lr).");
        }
    }
}

void material_report(void) {
    static uint32_t last[2];
    uint32_t h0 = dh3_mat_hits[0], h1 = dh3_mat_hits[1];
    if (h0 != last[0] || h1 != last[1]) {
        l_warn("[material] NULL-guard hits: mesh buffer missing=%u, CVertexAttributeMap::set skipped=%u",
               (unsigned) h0, (unsigned) h1);
        last[0] = h0;
        last[1] = h1;
    }
}

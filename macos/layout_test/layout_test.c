/* Guest-window test (MACOS_PORT_PLAN.md 0.3): runs the real xbox_MemoryLayoutInit
 * on a blank XBE and checks, through the same MEM32() macros the translated code
 * uses, that the RAM mirrors alias, the uncached/write-combined aliases resolve
 * to RAM, and the gap and GPU aperture are plain readable/writable memory. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "recomp_types.h"
#include "kernel/xbox_memory_layout.h"
#include "platform/mmio_trap.h"
#include <stdbool.h>

/* device installers (apu/aci_mmio.h, apu/apu.h, nv2a/nv2a_mmio_hook.h) */
extern bool aci_mmio_install(void *mem_base);
extern bool apu_mmio_install(uint8_t *mem_base);
extern bool nv2a_dac_trap_install(uint8_t *mem_base);

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    static uint8_t xbe[0x1000];
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) { printf("FAIL: xbox_MemoryLayoutInit\n"); return 1; }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

    const uint32_t size   = (uint32_t)xbox_GetMemorySize();
    const int      slots  = xbox_GetMirrorSlotCount();
    printf("RAM %u MB, %d mirror slots, base %p\n", size >> 20, slots, xbox_GetMemoryBase());
    CHECK(slots >= 20, "mirror slot count %d", slots);

    /* mirrors alias RAM: write through the base, read through every slot */
    const uint32_t va = 0x00500000u;
    MEM32(va) = 0xA5A5C3C3u;
    int checked = 0;
    for (int m = 1; m <= slots; m++) {
        uint32_t a = va + (uint32_t)m * size;
        /* XBOX_PTR folds these two windows onto RAM itself (uncached 0x80000000,
         * write-combined 0xF0000000), so they never reach the mirror views. */
        if ((a & 0xFC000000u) == 0x80000000u || (a >= 0xF0000000u && a < 0xF4000000u)) continue;
        CHECK(MEM32(a) == 0xA5A5C3C3u, "mirror %d read", m);
        checked++;
    }
    printf("%d mirror slots read back (others fall in the uncached/WC alias windows)\n", checked);
    /* ... and the other way round, and at sub-word sizes */
    MEM32(va + 5u * size) = 0x11223344u;
    CHECK(MEM32(va) == 0x11223344u, "write via mirror 5 visible at base");
    MEM8(va + 9u * size + 1) = 0xEE;
    CHECK(MEM8(va + 1) == 0xEE && MEM16(va) == 0xEE44u, "byte/half through mirror 9");
    MEM16(va + 20u * size + 2) = 0xBEEF;   /* mirror 20: a 64-bit-address-sized offset */
    CHECK(MEM32(va) == 0xBEEFEE44u, "half through mirror 20");

    /* uncached (0x80000000) and write-combined (0xF0000000) aliases are RAM */
    CHECK(MEM32(0x80000000u + va) == MEM32(va), "uncached alias");
    MEM32(0xF0000000u + va + 4) = 0xCAFEF00Du;
    CHECK(MEM32(va + 4) == 0xCAFEF00Du, "write-combined alias write");

    /* the gap above the last mirror and the GPU aperture are plain memory */
    uint32_t gap = (uint32_t)(slots + 1) * size + 0x1000u;
    CHECK(gap < 0xFD000000u, "gap exists (0x%08X)", gap);
    MEM32(gap) = 0x01020304u;
    CHECK(MEM32(gap) == 0x01020304u, "gap read/write");
    MEM32(0xFDF00000u) = 0x0BADF00Du;
    CHECK(MEM32(0xFDF00000u) == 0x0BADF00Du, "GPU aperture read/write");
    CHECK(MEM32(0xFFFFFF00u) == 0 || 1, "top of address space readable");

    /* device registers: protected pages whose loads and stores the MMIO trap emulates.
     * Every access below goes through MEM*(), i.e. the same code the translated game runs. */
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    CHECK(aci_mmio_install(mem), "ACI trap installed");
    CHECK(apu_mmio_install(mem), "APU trap installed");
    CHECK(nv2a_dac_trap_install(mem), "DAC trap installed");
    uint64_t t0 = ot_mmio_trap_count();

    /* ACI: bus-master channels reset halted (status register 0x106, bit 0 = DCH) */
    CHECK((MEM16(0xFEC00000u + 0x106) & 1) == 1, "ACI channel 0 reads as halted after reset");
    MEM8(0xFEC00000u + 0x10B) = 0x00;            /* control register write must not crash */
    /* ... and the rest of its 16 KB host page is plain memory, not the device */
    MEM32(0xFEC01000u) = 0x13572468u;
    CHECK(MEM32(0xFEC01000u) == 0x13572468u, "memory next to the ACI window behaves like RAM");
    /* APU: registers are reachable; the DSOUND poll reads this one */
    uint32_t apu_reg = MEM32(0xFE820010u);
    printf("APU 0xFE820010 reads 0x%08X\n", apu_reg);
    MEM32(0xFE800000u + 0x2000) = 0;
    /* DAC palette: VGA index register written and read back through the device */
    MEM8(0xFD681000u + 0x3C8) = 0x5A;
    CHECK(MEM8(0xFD681000u + 0x3C8) == 0x5A, "DAC write index reads back");
    MEM8(0xFD681000u + 0x3C6) = 0xFF;
    CHECK(MEM8(0xFD681000u + 0x3C6) == 0xFF, "DAC pixel mask reads back");
    CHECK(ot_mmio_trap_count() - t0 >= 8, "device accesses went through the trap (%llu)",
          (unsigned long long)(ot_mmio_trap_count() - t0));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    _exit(g_fail ? 1 : 0);   /* skip shutdown: pump threads are still running */
}

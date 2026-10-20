/*
 * QTest testcase for the 970's HSPRG0, HSPRG1, DSISR on a data segment
 * interrupt, and slbmte with RB's reserved bits set
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/*
 * A few 970 instructions run under TCG on mac99, as its firmware, at the
 * reset vector, so no OpenBIOS runs. The guest writes HSPRG0 and HSPRG1 and
 * reads them back, then sets DSISR to DSISR_NOPTE, as a DSI leaves it, turns
 * data translation on with an empty SLB, and loads from EA. That takes a
 * data segment interrupt, to 0x380 in RAM, where the test has put a handler.
 * In RAM at R:
 *   R + 0x00: HSPRG0;  R + 0x08: HSPRG1, as mfspr read them back.
 *   R + 0x10: DSISR;   R + 0x18: DAR, as the handler read them.
 *   R + 0x20: 1 once the handler is done.
 */
#define GUEST_RESULT_ADDR 0x10000
#define GUEST_CODE_OFFSET 0x100
#define GUEST_DSEG_VECTOR 0x380
#define GUEST_EA 0x30000000
#define HSPRG0_VALUE 0x1122334455667788ULL

static const uint32_t guest_code[] = {
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! r4 = R */
    0x3ca01122, /* lis     r5,0x1122 */
    0x60a53344, /* ori     r5,r5,0x3344 */
    0x78a507c6, /* sldi    r5,r5,32 */
    0x64a55566, /* oris    r5,r5,0x5566 */
    0x60a57788, /* ori     r5,r5,0x7788      ! r5 = HSPRG0_VALUE */
    0x7cb04ba6, /* mtspr   304,r5            ! HSPRG0 */
    0x7ca628f8, /* not     r6,r5 */
    0x7cd14ba6, /* mtspr   305,r6            ! HSPRG1 */
    0x38e00000, /* li      r7,0 */
    0x39000000, /* li      r8,0 */
    0x7cf04aa6, /* mfspr   r7,304 */
    0x7d114aa6, /* mfspr   r8,305 */
    0xf8e40000, /* std     r7,0(r4)          ! R + 0x00 */
    0xf9040008, /* std     r8,8(r4)          ! R + 0x08 */
    0x3ca04000, /* lis     r5,0x4000 */
    0x7cb203a6, /* mtdsisr r5                ! DSISR_NOPTE */
    0x38600000, /* li      r3,0 */
    0x64633000, /* oris    r3,r3,0x3000      ! r3 = EA */
    0x7cc000a6, /* mfmsr   r6 */
    0x60c60010, /* ori     r6,r6,0x10 */
    0x7cc00164, /* mtmsrd  r6                ! MSR[DR] */
    0x4c00012c, /* isync */
    0xe8a30000, /* ld      r5,0(r3)          ! a data segment interrupt */
    0x48000000, /* b       . */
};

static const uint32_t guest_dseg_handler[] = {
    0x7cb202a6, /* mfdsisr r5 */
    0x7cf302a6, /* mfdar   r7 */
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! r4 = R */
    0xf8a40010, /* std     r5,0x10(r4)       ! R + 0x10 */
    0xf8e40018, /* std     r7,0x18(r4)       ! R + 0x18 */
    0x38a00001, /* li      r5,1 */
    0x90a40020, /* stw     r5,0x20(r4)       ! done */
    0x48000000, /* b       . */
};

/*
 * The 970 runs xnu's slbmte r10,r9, for a fault in its copy window, with RB
 * built from the faulting address, so RB's reserved bits 37:51 and its
 * index's bits 52:57 can be set (xnu-1228.15.4, osfmk/ppc/hw_vm.s,
 * hpfLoadSeg64). The guest loads RB with each value below, runs slbmte into
 * slot 7, and reads slot 7 back with slbmfee and slbmfev. A Program
 * interrupt goes to 0x700, where the test has put a handler. In RAM at R:
 *   R + 0x00: ESID;  R + 0x08: VSID, as slbmfee and slbmfev read them.
 *   R + 0x18: SRR0, if the handler ran.
 *   R + 0x20: 1 once the guest is done, or 2 after a Program interrupt.
 */
#define GUEST_PROGRAM_VECTOR 0x700
#define SLBMTE_RB_ESID 0xe8000000ULL
#define SLBMTE_RS 0x01234c00ULL

/* 32-bit values, which oris and ori load */
static const uint32_t slbmte_rbs[] = {
    /* ESID 0xE, valid, slot 7, and bit 51, for DAR 0xE0001000 */
    0xe8001007,
    /* ... and for DAR 0xEFFFFFB4, which also leaves 0xf80 in the index */
    0xefffff87,
    /* ESID 0xE, valid, slot 7, and 0xc0, so that only a 6-bit index gives 7 */
    0xe80000c7,
};

static const uint32_t guest_program_handler[] = {
    0x7cba02a6, /* mfsrr0  r5 */
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! r4 = R */
    0xf8a40018, /* std     r5,0x18(r4)       ! R + 0x18 */
    0x38a00002, /* li      r5,2 */
    0x90a40020, /* stw     r5,0x20(r4)       ! a Program interrupt */
    0x48000000, /* b       . */
};

typedef struct SlbmteCase {
    const char *cpu;
    uint32_t rb;
} SlbmteCase;

/*
 * Run code as mac99's firmware, at the reset vector, with handler at vector
 * in RAM, and wait for the guest to write a nonzero word at R + 0x20.
 */
static QTestState *run_970_guest(const char *cpu,
                                 const uint32_t *code, size_t code_len,
                                 uint64_t vector,
                                 const uint32_t *handler, size_t handler_len)
{
    QTestState *qtest;
    GError *err = NULL;
    size_t rom_size = GUEST_CODE_OFFSET + 4 * code_len;
    g_autofree uint8_t *rom = g_malloc0(rom_size);
    g_autofree uint8_t *vec = g_malloc(4 * handler_len);
    char *rom_path;
    int fd;

    for (size_t i = 0; i < code_len; i++) {
        stl_be_p(&rom[GUEST_CODE_OFFSET + 4 * i], code[i]);
    }
    for (size_t i = 0; i < handler_len; i++) {
        stl_be_p(&vec[4 * i], handler[i]);
    }
    fd = g_file_open_tmp("qtest-ppc970.XXXXXX", &rom_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(rom_path, (const char *)rom, rom_size, &err);
    g_assert_no_error(err);

    /* Stopped, so that the handler is in place before the guest runs. */
    qtest = qtest_initf("-M mac99 -cpu %s -accel tcg -bios %s -S",
                        cpu, rom_path);
    /* QEMU has loaded the code by now. */
    unlink(rom_path);
    g_free(rom_path);
    qtest_memwrite(qtest, vector, vec, 4 * handler_len);
    qtest_qmp_assert_success(qtest, "{ 'execute': 'cont' }");

    for (int i = 0; i < 1000; i++) {
        if (qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20) != 0) {
            break;
        }
        g_usleep(10 * 1000);
    }
    return qtest;
}

static void test_970_sprs(const void *data)
{
    const char *cpu = data;
    QTestState *qtest;
    uint8_t res[0x20];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qtest = run_970_guest(cpu, guest_code, ARRAY_SIZE(guest_code),
                          GUEST_DSEG_VECTOR, guest_dseg_handler,
                          ARRAY_SIZE(guest_dseg_handler));
    g_assert_cmphex(qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20), ==, 1);
    qtest_memread(qtest, GUEST_RESULT_ADDR, res, sizeof(res));

    g_assert_cmphex(ldq_be_p(&res[0x00]), ==, HSPRG0_VALUE);
    g_assert_cmphex(ldq_be_p(&res[0x08]), ==, ~HSPRG0_VALUE);

    /* The 970MP's value, and not the last DSI's. */
    g_assert_cmphex(ldq_be_p(&res[0x10]), ==, 0x00200000);
    g_assert_cmphex(ldq_be_p(&res[0x18]), ==, GUEST_EA);

    qtest_quit(qtest);
}

static void test_970_slbmte(const void *data)
{
    const SlbmteCase *c = data;
    QTestState *qtest;
    uint8_t res[0x20];
    const uint32_t code[] = {
        0x38800000,                     /* li      r4,0 */
        0x64840001,                     /* oris    r4,r4,1     ! r4 = R */
        0x39200000,                     /* li      r9,0 */
        0x65290000 | (c->rb >> 16),     /* oris    r9,r9,rb@h */
        0x61290000 | (c->rb & 0xffff),  /* ori     r9,r9,rb@l  ! r9 = RB */
        0x39400000,                     /* li      r10,0 */
        0x654a0000 | (SLBMTE_RS >> 16), /* oris    r10,r10,rs@h */
        0x614a0000 | (SLBMTE_RS & 0xffff), /* ori  r10,r10,rs@l ! r10 = RS */
        0x7d404b24,                     /* slbmte  r10,r9 */
        0x39600007,                     /* li      r11,7 */
        0x7ca05f26,                     /* slbmfee r5,r11 */
        0x7cc05ea6,                     /* slbmfev r6,r11 */
        0xf8a40000,                     /* std     r5,0(r4)    ! R + 0x00 */
        0xf8c40008,                     /* std     r6,8(r4)    ! R + 0x08 */
        0x38a00001,                     /* li      r5,1 */
        0x90a40020,                     /* stw     r5,0x20(r4) ! done */
        0x48000000,                     /* b       . */
    };

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qtest = run_970_guest(c->cpu, code, ARRAY_SIZE(code),
                          GUEST_PROGRAM_VECTOR, guest_program_handler,
                          ARRAY_SIZE(guest_program_handler));
    qtest_memread(qtest, GUEST_RESULT_ADDR, res, sizeof(res));

    /* No Program interrupt, so SRR0 is still 0 */
    g_assert_cmphex(ldq_be_p(&res[0x18]), ==, 0);
    g_assert_cmphex(qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20), ==, 1);

    /* Slot 7 holds the ESID and valid bit, with no reserved bits */
    g_assert_cmphex(ldq_be_p(&res[0x00]), ==, SLBMTE_RB_ESID);
    g_assert_cmphex(ldq_be_p(&res[0x08]), ==, SLBMTE_RS);

    qtest_quit(qtest);
}

int main(int argc, char **argv)
{
    static const char *const cpus[] = {
        "970_v2.2", "970fx_v3.1", "970mp_v1.1",
    };

    g_test_init(&argc, &argv, NULL);

    for (size_t i = 0; i < ARRAY_SIZE(cpus); i++) {
        char *path = g_strdup_printf("ppc970/%s/sprs", cpus[i]);
        qtest_add_data_func(path, cpus[i], test_970_sprs);
        g_free(path);

        for (size_t j = 0; j < ARRAY_SIZE(slbmte_rbs); j++) {
            SlbmteCase *c = g_new(SlbmteCase, 1);

            c->cpu = cpus[i];
            c->rb = slbmte_rbs[j];
            path = g_strdup_printf("ppc970/%s/slbmte/%08" PRIx32,
                                   cpus[i], c->rb);
            qtest_add_data_func_full(path, c, test_970_slbmte, g_free);
            g_free(path);
        }
    }

    return g_test_run();
}

/*
 * QTest testcase for the 970's HSPRG0, HSPRG1 and DSISR on a data segment
 * interrupt
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

static void test_970_sprs(const void *data)
{
    const char *cpu = data;
    QTestState *qtest;
    GError *err = NULL;
    uint8_t rom[GUEST_CODE_OFFSET + sizeof(guest_code)] = { 0 };
    uint8_t handler[sizeof(guest_dseg_handler)];
    uint8_t res[0x20];
    char *rom_path;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(guest_code); i++) {
        stl_be_p(&rom[GUEST_CODE_OFFSET + 4 * i], guest_code[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(guest_dseg_handler); i++) {
        stl_be_p(&handler[4 * i], guest_dseg_handler[i]);
    }
    fd = g_file_open_tmp("qtest-ppc970.XXXXXX", &rom_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(rom_path, (const char *)rom, sizeof(rom), &err);
    g_assert_no_error(err);

    /* Stopped, so that the handler is in place before the guest runs. */
    qtest = qtest_initf("-M mac99 -cpu %s -accel tcg -bios %s -S",
                        cpu, rom_path);
    /* QEMU has loaded the code by now. */
    unlink(rom_path);
    g_free(rom_path);
    qtest_memwrite(qtest, GUEST_DSEG_VECTOR, handler, sizeof(handler));
    qtest_qmp_assert_success(qtest, "{ 'execute': 'cont' }");

    for (int i = 0; i < 1000; i++) {
        if (qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20) == 1) {
            break;
        }
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20), ==, 1);
    qtest_memread(qtest, GUEST_RESULT_ADDR, res, sizeof(res));

    g_assert_cmphex(ldq_be_p(&res[0x00]), ==, HSPRG0_VALUE);
    g_assert_cmphex(ldq_be_p(&res[0x08]), ==, ~HSPRG0_VALUE);

    /* The 970MP's value, and not the last DSI's. */
    g_assert_cmphex(ldq_be_p(&res[0x10]), ==, 0x00200000);
    g_assert_cmphex(ldq_be_p(&res[0x18]), ==, GUEST_EA);

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
    }

    return g_test_run();
}

/*
 * QTest testcase for the macio NVRAM's 8-byte accesses
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/*
 * qtest's own accesses are split to the device's maximum access size before
 * they reach it, so this runs a few 970 instructions under TCG on mac99,
 * whose ld and std reach the NVRAM as one 8-byte access. The code is the
 * firmware, at mac99's reset vector, so no OpenBIOS runs. mac99 maps the
 * NVRAM at 0xfff04000 with an it_shift of 1, so NVRAM byte k is at 2k and
 * 2k + 1. The guest writes 0xa0 to 0xa7 to bytes 0 to 7, then stores, in
 * RAM at R:
 *   R + 0x00: an 8-byte load from the NVRAM; R + 0x08: eight byte loads of
 *             the same 8 bytes.
 *   R + 0x10: byte loads of what an 8-byte store of Q left at NVRAM + 0x20;
 *   R + 0x18: byte loads of what eight byte stores of Q left at NVRAM + 0x28.
 *   R + 0x20: 1 once the guest is done.
 */
#define GUEST_RESULT_ADDR 0x10000
#define GUEST_CODE_OFFSET 0x100

static const uint32_t guest_code[] = {
    0x38600000, /* li      r3,0 */
    0x6463fff0, /* oris    r3,r3,0xfff0 */
    0x60634000, /* ori     r3,r3,0x4000      ! r3 = the NVRAM */
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! r4 = R */
    0x38a000a0, /* li      r5,0xa0 */
    0x38c00000, /* li      r6,0 */
    0x7ca331ae, /* 1: stbx r5,r3,r6          ! NVRAM byte r6 / 2 */
    0x38a50001, /* addi    r5,r5,1 */
    0x38c60002, /* addi    r6,r6,2 */
    0x2c060010, /* cmpwi   r6,16 */
    0x4180fff0, /* blt     1b */
    0xe8a30000, /* ld      r5,0(r3)          ! 8-byte load */
    0xf8a40000, /* std     r5,0(r4)          ! R + 0x00 */
    0x38c00000, /* li      r6,0 */
    0x39240008, /* addi    r9,r4,8 */
    0x7ca330ae, /* 2: lbzx r5,r3,r6 */
    0x7ca931ae, /* stbx    r5,r9,r6          ! R + 0x08 */
    0x38c60001, /* addi    r6,r6,1 */
    0x2c060008, /* cmpwi   r6,8 */
    0x4180fff0, /* blt     2b */
    0x39000000, /* li      r8,0 */
    0x6508fff0, /* oris    r8,r8,0xfff0 */
    0x610801b0, /* ori     r8,r8,0x1b0       ! r8 = Q, in the ROM */
    0xe8a80000, /* ld      r5,0(r8) */
    0xf8a30020, /* std     r5,0x20(r3)       ! 8-byte store */
    0x39230028, /* addi    r9,r3,0x28 */
    0x38c00000, /* li      r6,0 */
    0x7ce830ae, /* 3: lbzx r7,r8,r6 */
    0x7ce931ae, /* stbx    r7,r9,r6          ! eight byte stores */
    0x38c60001, /* addi    r6,r6,1 */
    0x2c060008, /* cmpwi   r6,8 */
    0x4180fff0, /* blt     3b */
    0x39030020, /* addi    r8,r3,0x20 */
    0x39240010, /* addi    r9,r4,0x10 */
    0x38c00000, /* li      r6,0 */
    0x7ca830ae, /* 4: lbzx r5,r8,r6 */
    0x7ca931ae, /* stbx    r5,r9,r6          ! R + 0x10 and R + 0x18 */
    0x38c60001, /* addi    r6,r6,1 */
    0x2c060010, /* cmpwi   r6,16 */
    0x4180fff0, /* blt     4b */
    0x38a00001, /* li      r5,1 */
    0x90a40020, /* stw     r5,0x20(r4)       ! done */
    0x48000000, /* b       . */
    0x01020304, 0x05060708, /* 0x1b0: Q */
};

static void test_guest_8byte_access(void)
{
    static const uint8_t loaded[8] = {
        0xa0, 0xa0, 0xa1, 0xa1, 0xa2, 0xa2, 0xa3, 0xa3
    };
    static const uint8_t stored[8] = {
        0x02, 0x02, 0x04, 0x04, 0x06, 0x06, 0x08, 0x08
    };
    QTestState *qtest;
    GError *err = NULL;
    uint8_t rom[GUEST_CODE_OFFSET + sizeof(guest_code)] = { 0 };
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
    fd = g_file_open_tmp("qtest-macio-nvram.XXXXXX", &rom_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(rom_path, (const char *)rom, sizeof(rom), &err);
    g_assert_no_error(err);

    qtest = qtest_initf("-M mac99 -cpu 970fx -accel tcg -bios %s", rom_path);
    /* QEMU has loaded the code by now. */
    unlink(rom_path);
    g_free(rom_path);

    for (int i = 0; i < 1000; i++) {
        if (qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20) == 1) {
            break;
        }
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20), ==, 1);
    qtest_memread(qtest, GUEST_RESULT_ADDR, res, sizeof(res));

    /* The byte loads read the pattern, each byte twice, and so does ld. */
    g_assert_cmpmem(&res[0x08], 8, loaded, 8);
    g_assert_cmphex(ldq_be_p(&res[0x00]), ==, ldq_be_p(&res[0x08]));

    /*
     * The byte stores leave Q's odd bytes, since the later byte of each pair
     * wins, and std, split into bytes, leaves the same.
     */
    g_assert_cmpmem(&res[0x18], 8, stored, 8);
    g_assert_cmphex(ldq_be_p(&res[0x10]), ==, ldq_be_p(&res[0x18]));

    qtest_quit(qtest);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("macio-nvram/guest-8byte-access", test_guest_8byte_access);

    return g_test_run();
}

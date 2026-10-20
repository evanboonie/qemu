/*
 * qtest for powermac7_3 with two 970s
 *
 * CPU 1 starts powered off, and a strobe of its soft-reset GPIO, as Mac OS
 * X's MacRISC4CPU::startCPU drives it, takes it to its system reset vector
 * at 0x100 (AppleMacRISC4PE-204.0.9, MacRISC4CPU.cpp:422, :632-658). Each
 * CPU has its own MPIC outputs, so an IPI reaches only the CPUs it's sent
 * to. A few instructions run under TCG as the firmware, so no OpenBIOS
 * runs, and the test maps mac-io's registers itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h"

/* U3's AGP host bridge's config registers, little-endian */
#define U3_AGP_CONFIG_ADDR      0xf0800000
#define U3_AGP_CONFIG_DATA      0xf0c00000

/* mac-io, at device 12 on U3's AGP bus, and where the test maps it */
#define MACIO_SLOT              12
#define PCI_DEVICE_ID_APPLE_UNI_N_KEYL  0x0022
#define MACIO_BASE              0x80000000

/* Each CPU's soft-reset GPIO, as the real PowerMac7,3's cpu nodes give it */
#define K2_SOFT_RESET_CPU0      0x71
#define K2_SOFT_RESET_CPU1      0x72
#define GPIO_OUTPUT_ENABLE      0x04

/* The MPIC, at mac-io + 0x40000, little-endian */
#define MPIC_BASE               (MACIO_BASE + 0x40000)
#define MPIC_IPI_IVPR(n)        (MPIC_BASE + 0x10a0 + 0x10 * (n))
#define MPIC_CPU(c)             (MPIC_BASE + 0x20000 + 0x1000 * (c))
#define MPIC_IPI_DISPATCH(c, n) (MPIC_CPU(c) + 0x40 + 0x10 * (n))
#define MPIC_CTPR(c)            (MPIC_CPU(c) + 0x80)
#define MPIC_IPI_PRIORITY       8

/*
 * In RAM at R:
 *   R + 0x10: how many times the firmware's entry ran.
 *   R + 0x40 + 4 * id: how many times a CPU took a system reset to 0x100.
 *   R + 0x80 + 4 * id: how many times a CPU took an external interrupt.
 * id is r20: 1 on CPU 0, which sets it in the firmware, and 0 on CPU 1,
 * which never runs the firmware and has r20 clear from reset.
 */
#define GUEST_RESULT_ADDR       0x10000
#define ROM_ENTRY_COUNT         (GUEST_RESULT_ADDR + 0x10)
#define RESET_COUNT(id)         (GUEST_RESULT_ADDR + 0x40 + 4 * (id))
#define EXT_COUNT(id)           (GUEST_RESULT_ADDR + 0x80 + 4 * (id))
#define ID_CPU0                 1
#define ID_CPU1                 0

#define ROM_CODE_OFFSET         0x100
#define RESET_VECTOR            0x100
#define EXT_VECTOR              0x500

/*
 * The firmware, at the 970's reset address: count the entry atomically,
 * so that two CPUs running it would count 2, then wait for interrupts.
 */
static const uint32_t rom_code[] = {
    0x3a800001, /* li      r20,1             ! id: CPU 0 */
    0x39200000, /* li      r9,0 */
    0x65290001, /* oris    r9,r9,1           ! R */
    0x39290010, /* addi    r9,r9,0x10        ! R + 0x10 */
    0x7cc04828, /* 1: lwarx r6,0,r9 */
    0x38c60001, /* addi    r6,r6,1 */
    0x7cc0492d, /* stwcx.  r6,0,r9 */
    0x40c2fff4, /* bne-    1b */
    0x3ce07fff, /* lis     r7,0x7fff */
    0x7cf603a6, /* mtdec   r7                ! no decrementer interrupt */
    0x7d0000a6, /* mfmsr   r8 */
    0x61088000, /* ori     r8,r8,0x8000 */
    0x7d000164, /* mtmsrd  r8                ! MSR[EE] */
    0x48000000, /* b       . */
};

/* At 0x100: count the reset, then wait for interrupts */
static const uint32_t reset_code[] = {
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! R */
    0x7a851764, /* sldi    r5,r20,2 */
    0x7c842a14, /* add     r4,r4,r5 */
    0x80c40040, /* lwz     r6,0x40(r4) */
    0x38c60001, /* addi    r6,r6,1 */
    0x90c40040, /* stw     r6,0x40(r4)       ! R + 0x40 + 4 * id */
    0x3ce07fff, /* lis     r7,0x7fff */
    0x7cf603a6, /* mtdec   r7 */
    0x7d0000a6, /* mfmsr   r8 */
    0x61088000, /* ori     r8,r8,0x8000 */
    0x7d000164, /* mtmsrd  r8                ! MSR[EE] */
    0x48000000, /* b       . */
};

/* At 0x500: count the interrupt, and stay with MSR[EE] clear */
static const uint32_t ext_code[] = {
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! R */
    0x7a851764, /* sldi    r5,r20,2 */
    0x7c842a14, /* add     r4,r4,r5 */
    0x80c40080, /* lwz     r6,0x80(r4) */
    0x38c60001, /* addi    r6,r6,1 */
    0x90c40080, /* stw     r6,0x80(r4)       ! R + 0x80 + 4 * id */
    0x48000000, /* b       . */
};

static void write_code(QTestState *qts, uint64_t addr, const uint32_t *code,
                       size_t len)
{
    for (size_t i = 0; i < len; i++) {
        qtest_writel(qts, addr + 4 * i, code[i]);
    }
}

/* Config dwords through U3's registers: bit 31 selects x86-style fields */
static uint32_t u3_config_readl(QTestState *qts, int slot, int reg)
{
    qtest_writel(qts, U3_AGP_CONFIG_ADDR,
                 bswap32((1u << 31) | (slot << 11) | reg));
    return bswap32(qtest_readl(qts, U3_AGP_CONFIG_DATA));
}

static void u3_config_writel(QTestState *qts, int slot, int reg,
                             uint32_t val)
{
    qtest_writel(qts, U3_AGP_CONFIG_ADDR,
                 bswap32((1u << 31) | (slot << 11) | reg));
    qtest_writel(qts, U3_AGP_CONFIG_DATA, bswap32(val));
}

static void mpic_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

/* Wait up to 10 s for a word to reach a value */
static uint32_t wait_for(QTestState *qts, uint64_t addr, uint32_t val)
{
    uint32_t v = 0;

    for (int i = 0; i < 1000; i++) {
        v = qtest_readl(qts, addr);
        if (v == val) {
            break;
        }
        g_usleep(10 * 1000);
    }
    return v;
}

/* MacRISC4CPU's strobe: output enabled with data 0, then released */
static void strobe_soft_reset(QTestState *qts, int k2_offset)
{
    qtest_writeb(qts, MACIO_BASE + k2_offset, GPIO_OUTPUT_ENABLE);
    qtest_writeb(qts, MACIO_BASE + k2_offset, 0);
}

static void test_two_cpus(void)
{
    QTestState *qts;
    GError *err = NULL;
    size_t rom_size = ROM_CODE_OFFSET + sizeof(rom_code);
    g_autofree uint8_t *rom = g_malloc0(rom_size);
    char *rom_path;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    for (size_t i = 0; i < ARRAY_SIZE(rom_code); i++) {
        stl_be_p(&rom[ROM_CODE_OFFSET + 4 * i], rom_code[i]);
    }
    fd = g_file_open_tmp("qtest-mac-g5-smp.XXXXXX", &rom_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(rom_path, (const char *)rom, rom_size, &err);
    g_assert_no_error(err);

    /* Stopped, so that the vectors are in place before the guest runs */
    qts = qtest_initf("-M powermac7_3 -smp 2 -accel tcg -bios %s -S",
                      rom_path);
    unlink(rom_path);
    g_free(rom_path);
    write_code(qts, RESET_VECTOR, reset_code, ARRAY_SIZE(reset_code));
    write_code(qts, EXT_VECTOR, ext_code, ARRAY_SIZE(ext_code));

    /* mac-io's registers, as the firmware would map them */
    g_assert_cmphex(u3_config_readl(qts, MACIO_SLOT, PCI_VENDOR_ID), ==,
                    PCI_DEVICE_ID_APPLE_UNI_N_KEYL << 16 |
                    PCI_VENDOR_ID_APPLE);
    u3_config_writel(qts, MACIO_SLOT, PCI_BASE_ADDRESS_0, MACIO_BASE);
    u3_config_writel(qts, MACIO_SLOT, PCI_COMMAND, PCI_COMMAND_MEMORY);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    /* CPU 0 runs the firmware, and CPU 1 runs nothing */
    g_assert_cmpuint(wait_for(qts, ROM_ENTRY_COUNT, 1), ==, 1);
    g_usleep(200 * 1000);
    g_assert_cmpuint(qtest_readl(qts, ROM_ENTRY_COUNT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 0);

    /* CPU 1's strobe starts CPU 1 at 0x100, and leaves CPU 0 alone */
    strobe_soft_reset(qts, K2_SOFT_RESET_CPU1);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU1), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU0)), ==, 0);

    /* CPU 0's strobe resets CPU 0 alone */
    strobe_soft_reset(qts, K2_SOFT_RESET_CPU0);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU0), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, ROM_ENTRY_COUNT), ==, 1);

    /*
     * IPIs, as AppleMPIC sends them: IPI n goes to CPU n, through the
     * sender's dispatch register (AppleMPIC-1.5.3, AppleMPIC.cpp:618-629).
     * Both CPUs take any priority.
     */
    for (int n = 0; n < 2; n++) {
        mpic_writel(qts, MPIC_IPI_IVPR(n),
                    MPIC_IPI_PRIORITY << 16 | (0x40 + n));
        mpic_writel(qts, MPIC_CTPR(n), 0);
    }

    mpic_writel(qts, MPIC_IPI_DISPATCH(0, 0), 1 << 0);
    g_assert_cmpuint(wait_for(qts, EXT_COUNT(ID_CPU0), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, EXT_COUNT(ID_CPU1)), ==, 0);

    mpic_writel(qts, MPIC_IPI_DISPATCH(0, 1), 1 << 1);
    g_assert_cmpuint(wait_for(qts, EXT_COUNT(ID_CPU1), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, EXT_COUNT(ID_CPU0)), ==, 1);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("mac-g5-smp/two-cpus", test_two_cpus);
    return g_test_run();
}

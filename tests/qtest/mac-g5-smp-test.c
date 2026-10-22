/*
 * qtest for powermac7_3 with two 970s
 *
 * CPU 1 starts powered off, and a strobe of its soft-reset GPIO, as Mac OS
 * X's MacRISC4CPU::startCPU drives it, takes it to its system reset vector
 * at 0x100 (AppleMacRISC4PE-204.0.9, MacRISC4CPU.cpp:422, :632-658). Each
 * CPU has its own MPIC outputs, so an IPI reaches only the CPUs it's sent
 * to, and a machine reset releases the soft-reset lines. One K2 GPIO
 * drives every CPU's TBEN: driven low it freezes their time bases, which
 * Mac OS X does while it copies CPU 0's to a CPU it starts (xnu-1228.15.4,
 * cpu.c:710-790), and released it restarts them. A few instructions run
 * under TCG as the firmware, so no OpenBIOS runs, and the test maps
 * mac-io's registers itself.
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

/* Every CPU's TBEN (hw/ppc/mac_g5.c), and powermac7_3's time-base frequency */
#define K2_TIMEBASE_ENABLE      0x77
#define TBFREQ                  25000000ULL

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
 * which never runs the firmware. CPU 1's r20 is 0 because the CPU object
 * is zero-filled when it's created: QEMU's ppc CPU reset leaves the GPRs
 * alone.
 */
#define GUEST_RESULT_ADDR       0x10000
#define ROM_ENTRY_COUNT         (GUEST_RESULT_ADDR + 0x10)
#define RESET_COUNT(id)         (GUEST_RESULT_ADDR + 0x40 + 4 * (id))
#define EXT_COUNT(id)           (GUEST_RESULT_ADDR + 0x80 + 4 * (id))
#define ID_CPU0                 1
#define ID_CPU1                 0

/* The firmware, where the 970 enters it (ppc_core99_reset()) */
#define PROM_BASE               0xfff00000ULL
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

/*
 * At 0x500: count the interrupt, acknowledge it and end it through the
 * MPIC's registers for the current CPU (IACK and EOI, at MPIC_BASE + 0xa0
 * and + 0xb0), and return to the loop, whose MSR[EE] is set again from
 * SRR1. So each CPU keeps taking external interrupts between IPIs, and an
 * IPI that reached the wrong CPU would be counted there.
 */
static const uint32_t ext_code[] = {
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! R */
    0x7a851764, /* sldi    r5,r20,2 */
    0x7c842a14, /* add     r4,r4,r5 */
    0x80c40080, /* lwz     r6,0x80(r4) */
    0x38c60001, /* addi    r6,r6,1 */
    0x90c40080, /* stw     r6,0x80(r4)       ! R + 0x80 + 4 * id */
    0x39200000, /* li      r9,0 */
    0x65298004, /* oris    r9,r9,0x8004 */
    0x612900a0, /* ori     r9,r9,0xa0        ! MPIC_BASE + 0xa0 */
    0x7d404c2c, /* lwbrx   r10,0,r9          ! IACK */
    0x39600000, /* li      r11,0 */
    0x39290010, /* addi    r9,r9,0x10 */
    0x7d604d2c, /* stwbrx  r11,0,r9          ! EOI */
    0x7c0004ac, /* sync */
    0x4c000024, /* rfid */
};

/*
 * The time-base test's firmware and reset vector. CPU 0 enters the
 * firmware, which sets its id and goes to 0x100, where CPU 1 starts from
 * its soft reset. There, each CPU stores its time base again and again,
 * and when asked writes a value to it:
 *   R + 0x100 + 8 * id: the time base, as mftb last read it.
 *   R + 0x200 + 8 * id: nonzero asks the CPU to write its time base, and
 *     it's cleared once that's done.
 *   R + 0x300 + 8 * id: the value to write, as mttbl, mttbu and mttbl write
 *     it, as Mac OS X does (xnu-1228.15.4, cpu.c:771-776).
 * MSR[EE] stays clear, so no decrementer interrupt comes.
 */
#define TB_NOW(id)              (GUEST_RESULT_ADDR + 0x100 + 8 * (id))
#define TB_WRITE(id)            (GUEST_RESULT_ADDR + 0x200 + 8 * (id))
#define TB_VALUE(id)            (GUEST_RESULT_ADDR + 0x300 + 8 * (id))

static const uint32_t tb_rom_code[] = {
    0x3a800001, /* li      r20,1             ! id: CPU 0 */
    0x48000102, /* ba      0x100 */
};

static const uint32_t tb_code[] = {
    0x38800000, /* li      r4,0 */
    0x64840001, /* oris    r4,r4,1           ! R */
    0x1cb40008, /* mulli   r5,r20,8 */
    0x7c842a14, /* add     r4,r4,r5          ! R + 8 * id */
    0x7ccc42e6, /* 1: mftb r6 */
    0xf8c40100, /* std     r6,0x100(r4) */
    0x80e40200, /* lwz     r7,0x200(r4) */
    0x2c070000, /* cmpwi   r7,0 */
    0x4182fff0, /* beq     1b */
    0x81240300, /* lwz     r9,0x300(r4)      ! the value's upper half */
    0x81440304, /* lwz     r10,0x304(r4)     ! and its lower half */
    0x39600000, /* li      r11,0 */
    0x7d7c43a6, /* mttbl   r11 */
    0x7d3d43a6, /* mttbu   r9 */
    0x7d5c43a6, /* mttbl   r10 */
    0x38e00000, /* li      r7,0 */
    0x90e40200, /* stw     r7,0x200(r4)      ! done */
    0x4bffffcc, /* b       1b */
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

/* mac-io's registers, as the firmware would map them */
static void map_macio(QTestState *qts)
{
    g_assert_cmphex(u3_config_readl(qts, MACIO_SLOT, PCI_VENDOR_ID), ==,
                    PCI_DEVICE_ID_APPLE_UNI_N_KEYL << 16 |
                    PCI_VENDOR_ID_APPLE);
    u3_config_writel(qts, MACIO_SLOT, PCI_BASE_ADDRESS_0, MACIO_BASE);
    u3_config_writel(qts, MACIO_SLOT, PCI_COMMAND, PCI_COMMAND_MEMORY);
}

/*
 * A CPU's NIP, from "info registers <cpu>". Neither "info cpus" nor
 * query-cpus-fast says whether a CPU is halted, so the tests check that
 * CPU 1's NIP is still its reset value, where the firmware's own loop
 * never is, and later that it's in the reset vector's code.
 */
static uint64_t cpu_nip(QTestState *qts, int cpu)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers %d", cpu);
    char *nip = strstr(regs, "NIP ");

    g_assert_nonnull(nip);
    return g_ascii_strtoull(nip + 4, NULL, 16);
}

/*
 * Start powermac7_3 with extra arguments and code as its firmware, stopped,
 * so that the vectors can be put in place before the guest runs.
 */
static QTestState *start_stopped(const char *args, const uint32_t *code,
                                 size_t len)
{
    QTestState *qts;
    GError *err = NULL;
    size_t rom_size = ROM_CODE_OFFSET + 4 * len;
    g_autofree uint8_t *rom = g_malloc0(rom_size);
    char *rom_path;
    int fd;

    for (size_t i = 0; i < len; i++) {
        stl_be_p(&rom[ROM_CODE_OFFSET + 4 * i], code[i]);
    }
    fd = g_file_open_tmp("qtest-mac-g5-smp.XXXXXX", &rom_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(rom_path, (const char *)rom, rom_size, &err);
    g_assert_no_error(err);

    qts = qtest_initf("-M powermac7_3 %s -accel tcg -bios %s -S", args,
                      rom_path);
    unlink(rom_path);
    g_free(rom_path);
    return qts;
}

/*
 * Start powermac7_3 with two CPUs, the firmware above and the vectors in
 * RAM, and mac-io mapped, and wait for CPU 0 to enter the firmware.
 */
static QTestState *start_two_cpus(void)
{
    QTestState *qts;

    qts = start_stopped("-smp 2", rom_code, ARRAY_SIZE(rom_code));
    write_code(qts, RESET_VECTOR, reset_code, ARRAY_SIZE(reset_code));
    write_code(qts, EXT_VECTOR, ext_code, ARRAY_SIZE(ext_code));
    map_macio(qts);

    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmpuint(wait_for(qts, ROM_ENTRY_COUNT, 1), ==, 1);
    return qts;
}

/*
 * Send IPI n to CPU n, as AppleMPIC does, through the sender's dispatch
 * register (AppleMPIC-1.5.3, AppleMPIC.cpp:618-629), and check that the
 * target took one more external interrupt and the other CPU none.
 */
static void check_ipi(QTestState *qts, int target)
{
    int other = !target;
    uint32_t target_id = target ? ID_CPU1 : ID_CPU0;
    uint32_t other_id = other ? ID_CPU1 : ID_CPU0;
    uint32_t target_count = qtest_readl(qts, EXT_COUNT(target_id));
    uint32_t other_count = qtest_readl(qts, EXT_COUNT(other_id));

    mpic_writel(qts, MPIC_IPI_DISPATCH(0, target), 1 << target);
    g_assert_cmpuint(wait_for(qts, EXT_COUNT(target_id), target_count + 1),
                     ==, target_count + 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, EXT_COUNT(target_id)), ==,
                     target_count + 1);
    g_assert_cmpuint(qtest_readl(qts, EXT_COUNT(other_id)), ==, other_count);
}

static void test_two_cpus(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qts = start_two_cpus();

    /* CPU 0 runs the firmware, and CPU 1 runs nothing */
    g_usleep(200 * 1000);
    g_assert_cmpuint(qtest_readl(qts, ROM_ENTRY_COUNT), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 0);
    g_assert_cmphex(cpu_nip(qts, 1), ==, PROM_BASE + ROM_CODE_OFFSET);

    /* CPU 1's strobe starts CPU 1 at 0x100, and leaves CPU 0 alone */
    strobe_soft_reset(qts, K2_SOFT_RESET_CPU1);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU1), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU0)), ==, 0);
    g_assert_cmphex(cpu_nip(qts, 1), >=, RESET_VECTOR);
    g_assert_cmphex(cpu_nip(qts, 1), <, RESET_VECTOR + sizeof(reset_code));

    /* CPU 0's strobe resets CPU 0 alone */
    strobe_soft_reset(qts, K2_SOFT_RESET_CPU0);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU0), 1), ==, 1);
    g_usleep(100 * 1000);
    g_assert_cmpuint(qtest_readl(qts, RESET_COUNT(ID_CPU1)), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, ROM_ENTRY_COUNT), ==, 1);

    /* Both CPUs take any priority */
    for (int n = 0; n < 2; n++) {
        mpic_writel(qts, MPIC_IPI_IVPR(n),
                    MPIC_IPI_PRIORITY << 16 | (0x40 + n));
        mpic_writel(qts, MPIC_CTPR(n), 0);
    }

    /* Each CPU's IPIs reach it alone, more than once */
    check_ipi(qts, 0);
    check_ipi(qts, 1);
    check_ipi(qts, 0);
    check_ipi(qts, 1);

    qtest_quit(qts);
}

/*
 * A machine reset releases the soft-reset lines, so a strobe after it
 * still starts CPU 1, even if the line was held low when it came.
 */
static void test_soft_reset_after_system_reset(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qts = start_two_cpus();

    /* Hold CPU 1's line low: that starts it once */
    qtest_writeb(qts, MACIO_BASE + K2_SOFT_RESET_CPU1, GPIO_OUTPUT_ENABLE);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU1), 1), ==, 1);

    /* RAM keeps the counts and the vectors; PCI forgets mac-io's BAR */
    qtest_system_reset(qts);
    g_assert_cmpuint(wait_for(qts, ROM_ENTRY_COUNT, 2), ==, 2);
    g_assert_cmphex(cpu_nip(qts, 1), ==, PROM_BASE + ROM_CODE_OFFSET);
    map_macio(qts);

    strobe_soft_reset(qts, K2_SOFT_RESET_CPU1);
    g_assert_cmpuint(wait_for(qts, RESET_COUNT(ID_CPU1), 2), ==, 2);

    qtest_quit(qts);
}

/* A CPU's time base, as its loop last stored it */
static uint64_t tb_now(QTestState *qts, int id)
{
    return qtest_readq(qts, TB_NOW(id));
}

/* Wait up to 10 s for a CPU's stored time base to move from a value */
static uint64_t wait_tb_moves(QTestState *qts, int id, uint64_t from)
{
    uint64_t tb = from;

    for (int i = 0; i < 1000 && tb == from; i++) {
        g_usleep(10 * 1000);
        tb = tb_now(qts, id);
    }
    return tb;
}

/*
 * Start powermac7_3 with the time-base test's firmware and smp CPUs, each
 * a cpu if it's given, and wait until each CPU stores its time base. CPU 1
 * starts from its soft reset.
 */
static QTestState *start_tb_test(const char *cpu, int smp)
{
    g_autofree char *args = g_strdup_printf("-smp %d%s%s", smp,
                                            cpu ? " -cpu " : "",
                                            cpu ? cpu : "");
    QTestState *qts = start_stopped(args, tb_rom_code,
                                    ARRAY_SIZE(tb_rom_code));

    write_code(qts, RESET_VECTOR, tb_code, ARRAY_SIZE(tb_code));
    map_macio(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU0, 0), !=, 0);
    if (smp > 1) {
        strobe_soft_reset(qts, K2_SOFT_RESET_CPU1);
        g_assert_cmphex(wait_tb_moves(qts, ID_CPU1, 0), !=, 0);
    }
    return qts;
}

/*
 * Freeze a CPU's time base, after reading it, and check that it holds at a
 * value no lower, and within a second of it. Returns that value.
 */
static uint64_t check_frozen(QTestState *qts, int id, uint64_t before)
{
    uint64_t frozen = tb_now(qts, id);

    g_test_message("CPU id %d: 0x%" PRIx64 " before, 0x%" PRIx64
                   " frozen", id, before, frozen);
    g_assert_cmphex(frozen, >=, before);
    g_assert_cmphex(frozen - before, <, TBFREQ);
    g_usleep(200 * 1000);
    g_assert_cmphex(tb_now(qts, id), ==, frozen);
    return frozen;
}

/*
 * Mac OS X's handshake on powermac7_3 -smp 2: K2 0x77 driven low freezes
 * both CPUs' time bases, a time base written on CPU 1
 * while frozen holds, and the line released restarts both, CPU 1's from
 * the value it wrote.
 */
static void test_timebase_enable(void)
{
    QTestState *qts;
    uint64_t before0, before1, frozen0, frozen1, value, run0, run1;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qts = start_tb_test(NULL, 2);

    /* Both count */
    before0 = tb_now(qts, ID_CPU0);
    before1 = tb_now(qts, ID_CPU1);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU0, before0), >, before0);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU1, before1), >, before1);

    /*
     * Driven low: both hold, at nearly the same value. Each freezes as of
     * when its TBEN fell, and mac_g5.c drives them one after the other,
     * so they can differ by the ticks that takes, well under 1 ms.
     */
    before0 = tb_now(qts, ID_CPU0);
    before1 = tb_now(qts, ID_CPU1);
    qtest_writeb(qts, MACIO_BASE + K2_TIMEBASE_ENABLE, GPIO_OUTPUT_ENABLE);
    g_usleep(100 * 1000);
    frozen0 = check_frozen(qts, ID_CPU0, before0);
    frozen1 = check_frozen(qts, ID_CPU1, before1);
    g_assert_cmphex(MAX(frozen0, frozen1) - MIN(frozen0, frozen1), <,
                    TBFREQ / 1000);

    /* CPU 1 writes its time base, which holds; CPU 0's still holds */
    value = frozen1 + 1000 * TBFREQ;
    qtest_writel(qts, TB_VALUE(ID_CPU1), value >> 32);
    qtest_writel(qts, TB_VALUE(ID_CPU1) + 4, (uint32_t)value);
    qtest_writel(qts, TB_WRITE(ID_CPU1), 1);
    g_assert_cmpuint(wait_for(qts, TB_WRITE(ID_CPU1), 0), ==, 0);
    g_usleep(100 * 1000);
    g_assert_cmphex(tb_now(qts, ID_CPU1), ==, value);
    g_usleep(200 * 1000);
    g_assert_cmphex(tb_now(qts, ID_CPU1), ==, value);
    g_assert_cmphex(tb_now(qts, ID_CPU0), ==, frozen0);

    /*
     * Released: both count again, from where they held, and together: the
     * time since is the same on both, give or take how long the two reads
     * of guest memory take, well under 10 ms.
     */
    qtest_writeb(qts, MACIO_BASE + K2_TIMEBASE_ENABLE, 0);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU0, frozen0), >, frozen0);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU1, value), >, value);
    g_usleep(200 * 1000);
    run0 = tb_now(qts, ID_CPU0) - frozen0;
    run1 = tb_now(qts, ID_CPU1) - value;
    g_test_message("since the release: 0x%" PRIx64 " on CPU 0, 0x%" PRIx64
                   " on CPU 1", run0, run1);
    g_assert_cmphex(run0, >=, TBFREQ / 5);
    g_assert_cmphex(run0, <, 10 * TBFREQ);
    g_assert_cmphex(MAX(run0, run1) - MIN(run0, run1), <, TBFREQ / 100);

    qtest_quit(qts);
}

/* On each 970 model, TBEN low freezes the time base and high restarts it */
static void test_timebase_enable_cpu(const void *data)
{
    const char *cpu = data;
    QTestState *qts;
    uint64_t before, frozen;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qts = start_tb_test(cpu, 1);
    before = tb_now(qts, ID_CPU0);
    qtest_writeb(qts, MACIO_BASE + K2_TIMEBASE_ENABLE, GPIO_OUTPUT_ENABLE);
    g_usleep(100 * 1000);
    frozen = check_frozen(qts, ID_CPU0, before);
    qtest_writeb(qts, MACIO_BASE + K2_TIMEBASE_ENABLE, 0);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU0, frozen), >, frozen);
    qtest_quit(qts);
}

/*
 * A machine reset releases K2's GPIOs and resets the CPUs, whose TBEN then
 * reads high, so time bases frozen when it comes count again after it.
 */
static void test_timebase_after_system_reset(void)
{
    QTestState *qts;
    uint64_t before, frozen;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    qts = start_tb_test(NULL, 2);
    before = tb_now(qts, ID_CPU0);
    qtest_writeb(qts, MACIO_BASE + K2_TIMEBASE_ENABLE, GPIO_OUTPUT_ENABLE);
    g_usleep(100 * 1000);
    frozen = check_frozen(qts, ID_CPU0, before);

    /* CPU 0 runs the firmware again, and its loop from 0x100 */
    qtest_system_reset(qts);
    g_assert_cmphex(wait_tb_moves(qts, ID_CPU0, frozen), >, frozen);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const char *const cpus[] = {
        "970_v2.2", "970fx_v3.1", "970mp_v1.1",
    };

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("mac-g5-smp/two-cpus", test_two_cpus);
    qtest_add_func("mac-g5-smp/soft-reset-after-system-reset",
                   test_soft_reset_after_system_reset);
    qtest_add_func("mac-g5-smp/timebase-enable", test_timebase_enable);
    for (size_t i = 0; i < ARRAY_SIZE(cpus); i++) {
        g_autofree char *path =
            g_strdup_printf("mac-g5-smp/timebase-enable/%s", cpus[i]);
        qtest_add_data_func(path, cpus[i], test_timebase_enable_cpu);
    }
    qtest_add_func("mac-g5-smp/timebase-after-system-reset",
                   test_timebase_after_system_reset);
    return g_test_run();
}

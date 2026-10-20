/*
 * qtest for the Power Mac G5 machine, powermac7_3
 *
 * Checks what OpenBIOS and the guest see before any firmware runs: the
 * machine ID and VIA configuration in fw_cfg, the devices in U3's AGP
 * bus, U3's version register, where its PCI I/O window is, its host
 * bridge's AGP capability, and where its NVRAM is. mac99's 970 path is
 * checked the same way, since the two share their init. powermac7_3's
 * flash NVRAM, which it takes in place of macio-nvram when given a pflash
 * drive, is then erased and programmed as Mac OS X's driver does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqos/fw_cfg.h"
#include "libqtest.h"
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_regs.h"
#include "standard-headers/linux/qemu_fw_cfg.h"

/* From include/hw/ppc/ppc.h */
#define ARCH_MAC99_U3           3
#define ARCH_POWERMAC7_3        4
#define FW_CFG_PPC_NVRAM_ADDR   (FW_CFG_ARCH_LOCAL + 0x08)
#define FW_CFG_PPC_NVRAM_FLAT   (FW_CFG_ARCH_LOCAL + 0x0a)
#define FW_CFG_PPC_VIACONFIG    (FW_CFG_ARCH_LOCAL + 0x0b)

/* From hw/ppc/mac_newworld.h */
#define CORE99_VIA_CONFIG_CUDA  0
#define CORE99_VIA_CONFIG_PMU   1

#define NEWWORLD_FW_CFG_BASE    0xf0000510

/*
 * The NVRAM, where both machines map it. On powermac7_3 with a pflash
 * drive it's a flash of two 8 KB banks.
 */
#define NVRAM_BASE              0xfff04000
#define NVRAM_BANK_SIZE         0x2000
#define NVRAM_SIZE              (2 * NVRAM_BANK_SIZE)

/* U3's AGP host bridge, as ppc_newworld_init() maps it */
#define U3_AGP_CONFIG_ADDR      0xf0800000
#define U3_AGP_CONFIG_DATA      0xf0c00000

/* U3's own registers, and their version register at offset 0 */
#define U3_REG_BASE             0xf8000000

/* From include/hw/pci-host/uninorth.h */
#define UNINORTH_VERSION_10A    0x7
#define U3_VERSION_POWERMAC7_3  0x35

/* From include/hw/pci/pci.h */
#define PCI_DEVICE_ID_APPLE_UNI_N_KEYL  0x0022

/* U3's AGP capability, from u3_agp_pci_host_realize() */
#define U3_AGP_CAP              0x80

/*
 * Read a config dword through U3's registers, which are little-endian.
 * Bit 31 of the address selects x86-style bus, devfn and register fields
 * (unin_get_config_reg() in hw/pci-host/uninorth.c).
 */
static uint32_t u3_config_readl(QTestState *qts, int slot, int reg)
{
    uint32_t addr = (1u << 31) | (slot << 11) | reg;

    qtest_writel(qts, U3_AGP_CONFIG_ADDR, bswap32(addr));
    return bswap32(qtest_readl(qts, U3_AGP_CONFIG_DATA));
}

/*
 * U3's 8 MB PCI I/O window, as "info mtree" lists it, and where the other
 * machine has it. On powermac7_3 the window lies under fw_cfg, at a lower
 * priority, so the fw_cfg reads below also show that fw_cfg still answers.
 */
#define IO_WINDOW_POWERMAC7_3 \
    "00000000f0000000-00000000f07fffff (prio -1, i/o): unin-pci-isa-mmio"
#define IO_WINDOW_MAC99 \
    "00000000f2000000-00000000f27fffff (prio 0, i/o): unin-pci-isa-mmio"

/* "info mtree" lists one region and not another */
static void check_mtree(QTestState *qts, const char *region,
                        const char *not_region)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree");

    g_assert_nonnull(strstr(mtree, region));
    g_assert_null(strstr(mtree, not_region));
}

/* Which NVRAM device is mapped: macio-nvram, or powermac7_3's flash */
#define NVRAM_MACIO \
    "00000000fff04000-00000000fff07fff (prio 0, i/o): macio-nvram"
#define NVRAM_FLASH \
    "00000000fff04000-00000000fff07fff (prio 0, romd): powermac7_3.nvram"
/* The flash out of ROMD mode, so reads go through the device */
#define NVRAM_FLASH_IO \
    "00000000fff04000-00000000fff07fff (prio 0, i/o): powermac7_3.nvram"

static void u3_config_writel(QTestState *qts, int slot, int reg,
                             uint32_t val)
{
    uint32_t addr = (1u << 31) | (slot << 11) | reg;

    qtest_writel(qts, U3_AGP_CONFIG_ADDR, bswap32(addr));
    qtest_writel(qts, U3_AGP_CONFIG_DATA, bswap32(val));
}

/* The AGP capability Mac OS X's AppleMacRiscAGP needs on U3's host bridge */
static void check_agp_capability(QTestState *qts)
{
    g_assert_true((u3_config_readl(qts, 11, PCI_COMMAND) >> 16) &
                  PCI_STATUS_CAP_LIST);
    g_assert_cmphex(u3_config_readl(qts, 11, PCI_CAPABILITY_LIST) & 0xff, ==,
                    U3_AGP_CAP);

    /* AGP's ID, version 3.0, and the last capability in the list */
    g_assert_cmphex(u3_config_readl(qts, 11, U3_AGP_CAP) & 0xffffff, ==,
                    0x30 << 16 | PCI_CAP_ID_AGP);
    g_assert_cmphex(u3_config_readl(qts, 11, U3_AGP_CAP + PCI_AGP_STATUS), ==,
                    0x1f00020a);

    /*
     * The command register keeps its fields, but not the AGP 3.0 mode bit
     * Mac OS X also writes, and a reset clears it
     */
    g_assert_cmphex(u3_config_readl(qts, 11, U3_AGP_CAP + PCI_AGP_COMMAND), ==,
                    0);
    u3_config_writel(qts, 11, U3_AGP_CAP + PCI_AGP_COMMAND, 0x1f00030a);
    g_assert_cmphex(u3_config_readl(qts, 11, U3_AGP_CAP + PCI_AGP_COMMAND), ==,
                    0x1f000302);
    qtest_system_reset(qts);
    g_assert_cmphex(u3_config_readl(qts, 11, U3_AGP_CAP + PCI_AGP_COMMAND), ==,
                    0);
}

static QTestState *check_machine(const char *machine, uint16_t machine_id,
                                 uint32_t via_config, uint32_t u3_version,
                                 const char *io_window,
                                 const char *not_io_window)
{
    QTestState *qts = qtest_initf("-M %s", machine);
    QFWCFG *fw_cfg = mm_fw_cfg_init(qts, NEWWORLD_FW_CFG_BASE);

    g_assert_cmpuint(qfw_cfg_get_u16(fw_cfg, FW_CFG_MACHINE_ID), ==,
                     machine_id);
    g_assert_cmpuint(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_VIACONFIG), ==,
                     via_config);
    g_assert_cmphex(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_NVRAM_ADDR), ==,
                    NVRAM_BASE);
    /* No flat key with macio-nvram: fw_cfg reads a missing key as 0 */
    g_assert_cmpuint(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_NVRAM_FLAT), ==, 0);
    mm_fw_cfg_uninit(fw_cfg);

    /* U3's host bridge at device 11, and macio at 12, as on mac99 */
    g_assert_cmphex(u3_config_readl(qts, 11, 0), ==,
                    PCI_DEVICE_ID_APPLE_U3_AGP << 16 | PCI_VENDOR_ID_APPLE);
    g_assert_cmphex(u3_config_readl(qts, 12, 0), ==,
                    PCI_DEVICE_ID_APPLE_UNI_N_KEYL << 16 |
                    PCI_VENDOR_ID_APPLE);

    g_assert_cmphex(qtest_readl(qts, U3_REG_BASE), ==, u3_version);
    check_mtree(qts, io_window, not_io_window);

    /* Last, since it resets the machine */
    check_agp_capability(qts);

    return qts;
}

/*
 * The real PowerMac7,3's U3: device-rev 0x35, PCI I/O at 0xf0000000.
 * Without a pflash drive it keeps mac99's macio-nvram, shifted.
 */
static void test_powermac7_3(void)
{
    QTestState *qts = check_machine("powermac7_3", ARCH_POWERMAC7_3,
                                    CORE99_VIA_CONFIG_PMU,
                                    U3_VERSION_POWERMAC7_3,
                                    IO_WINDOW_POWERMAC7_3, IO_WINDOW_MAC99);

    check_mtree(qts, NVRAM_MACIO, NVRAM_FLASH);
    qtest_quit(qts);
}

/* mac99's 970 keeps the G4's UniNorth version and I/O at 0xf2000000 */
static void test_mac99_970(void)
{
    QTestState *qts = check_machine("mac99 -cpu 970fx", ARCH_MAC99_U3,
                                    CORE99_VIA_CONFIG_CUDA,
                                    UNINORTH_VERSION_10A, IO_WINDOW_MAC99,
                                    IO_WINDOW_POWERMAC7_3);

    check_mtree(qts, NVRAM_MACIO, NVRAM_FLASH);
    qtest_quit(qts);
}

/*
 * AppleFlashNVRAMAMD's commands and status bits (AppleFlashNVRAM.h:41-65,
 * AppleFlashNVRAM-105.4.0). Each command goes to an offset in the bank it
 * erases or programs, the driver's mNVRAMNext.
 */
#define AMD_CMD_ADDR1           0x555
#define AMD_CMD_ADDR2           0x2aa
#define AMD_STATUS_ERROR        0x20    /* DQ5 */
#define AMD_STATUS_TOGGLE       0x40    /* DQ6 */

static void nvram_writeb(QTestState *qts, uint32_t bank, uint32_t offset,
                         uint8_t value)
{
    qtest_writeb(qts, NVRAM_BASE + bank + offset, value);
}

/*
 * As AppleFlashNVRAMAMD::waitForCommandDone(): read the bank's first byte
 * twice, until DQ6 stops toggling. The driver spins; here the virtual clock
 * moves on between reads. Returns how many times DQ6 toggled.
 *
 * When DQ5 shows, the driver reads twice more and fails only if DQ6 still
 * toggles (AppleFlashNVRAM.cpp:615-645). pflash_cfi02 never sets DQ5, so
 * failing on DQ5 at once is stricter than the driver, and unreachable.
 */
static int nvram_wait(QTestState *qts, uint32_t bank)
{
    int toggles = 0;

    for (;;) {
        uint8_t last = qtest_readb(qts, NVRAM_BASE + bank);
        uint8_t current = qtest_readb(qts, NVRAM_BASE + bank);

        if (!((last ^ current) & AMD_STATUS_TOGGLE)) {
            return toggles;
        }
        g_assert_false(current & AMD_STATUS_ERROR);
        g_assert_cmpint(++toggles, <, 1000);
        qtest_clock_step(qts, 10 * 1000);
    }
}

/*
 * Autoselect: AMD's manufacturer ID and the device's, which the driver
 * doesn't read, then the reset. A flash left in unlock bypass mode would
 * take the unlock cycles for data and fail this.
 */
static void nvram_check_ids(QTestState *qts)
{
    nvram_writeb(qts, 0, AMD_CMD_ADDR1, 0xaa);
    nvram_writeb(qts, 0, AMD_CMD_ADDR2, 0x55);
    nvram_writeb(qts, 0, AMD_CMD_ADDR1, 0x90);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, NVRAM_BASE + 1), ==, 0x37);
    nvram_writeb(qts, 0, 0, 0xf0);
}

/* As AppleFlashNVRAMAMD::eraseBlock() */
static void nvram_erase_bank(QTestState *qts, uint32_t bank)
{
    static const uint16_t cmds[][2] = {
        { AMD_CMD_ADDR1, 0xaa }, { AMD_CMD_ADDR2, 0x55 },
        { AMD_CMD_ADDR1, 0x80 },
        { AMD_CMD_ADDR1, 0xaa }, { AMD_CMD_ADDR2, 0x55 },
        { 0, 0x30 },
    };

    for (int i = 0; i < ARRAY_SIZE(cmds); i++) {
        nvram_writeb(qts, bank, cmds[i][0], cmds[i][1]);
    }
    /* The driver's IODelay(55), past the 50 us sector erase timeout */
    qtest_clock_step(qts, 55 * 1000);
    /* The erase takes time, so DQ6 toggles a while */
    g_assert_cmpint(nvram_wait(qts, bank), >, 0);
}

/* As AppleFlashNVRAMAMD::writeBlock(), in unlock bypass mode */
static void nvram_program_bank(QTestState *qts, uint32_t bank,
                               const uint8_t *data)
{
    nvram_writeb(qts, bank, AMD_CMD_ADDR1, 0xaa);
    nvram_writeb(qts, bank, AMD_CMD_ADDR2, 0x55);
    nvram_writeb(qts, bank, AMD_CMD_ADDR1, 0x20);
    for (int i = 0; i < NVRAM_BANK_SIZE; i++) {
        nvram_writeb(qts, bank, AMD_CMD_ADDR1, 0xa0);
        nvram_writeb(qts, bank, i, data[i]);
        g_assert_cmpint(nvram_wait(qts, bank), ==, 0);
    }
    /* Unlock bypass reset */
    nvram_writeb(qts, bank, 0, 0x90);
    nvram_writeb(qts, bank, 0, 0x00);
}

static void check_bank(const uint8_t *nvram, uint32_t bank,
                       const uint8_t *expected)
{
    for (int i = 0; i < NVRAM_BANK_SIZE; i++) {
        g_assert_cmphex(nvram[bank + i], ==, expected[i]);
    }
}

/*
 * The flash, backed by a file, as Mac OS X's AppleFlashNVRAMAMD drives it:
 * it erases the bank it isn't using, bank 1 here, programs it, and reads
 * both back. The firmware reads it flat. The file's bytes show at the
 * start, rather than the zeros in the firmware image under the flash.
 * Bank 0 keeps them, bank 1 gets the new ones, and so does the file.
 */
static void test_powermac7_3_nvram(void)
{
    const uint32_t bank0 = 0, bank1 = NVRAM_BANK_SIZE;
    g_autofree uint8_t *file = g_malloc(NVRAM_SIZE);
    g_autofree uint8_t *data = g_malloc(NVRAM_BANK_SIZE);
    g_autofree uint8_t *blank = g_malloc(NVRAM_BANK_SIZE);
    g_autofree uint8_t *buf = g_malloc(NVRAM_SIZE);
    g_autofree char *path = NULL;
    g_autoptr(GError) err = NULL;
    QTestState *qts;
    QFWCFG *fw_cfg;
    gsize len;
    int fd;

    for (int i = 0; i < NVRAM_BANK_SIZE; i++) {
        file[bank0 + i] = i * 7 + 1;
        file[bank1 + i] = 0;    /* so the erase shows */
        data[i] = i * 13 + 0x5a;
    }
    memset(blank, 0xff, NVRAM_BANK_SIZE);
    fd = g_file_open_tmp("qtest-powermac7_3-nvram-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(write(fd, file, NVRAM_SIZE), ==, NVRAM_SIZE);
    close(fd);

    qts = qtest_initf("-M powermac7_3 "
                      "-drive if=pflash,format=raw,file=%s", path);
    fw_cfg = mm_fw_cfg_init(qts, NEWWORLD_FW_CFG_BASE);
    g_assert_cmphex(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_NVRAM_ADDR), ==,
                    NVRAM_BASE);
    g_assert_cmpuint(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_NVRAM_FLAT), ==, 1);
    mm_fw_cfg_uninit(fw_cfg);
    check_mtree(qts, NVRAM_FLASH, NVRAM_MACIO);

    qtest_memread(qts, NVRAM_BASE, buf, NVRAM_SIZE);
    check_bank(buf, bank0, file + bank0);
    check_bank(buf, bank1, file + bank1);

    nvram_check_ids(qts);

    nvram_erase_bank(qts, bank1);
    qtest_memread(qts, NVRAM_BASE, buf, NVRAM_SIZE);
    check_bank(buf, bank0, file + bank0);
    check_bank(buf, bank1, blank);

    nvram_program_bank(qts, bank1, data);
    /* The unlock bypass reset left bypass mode */
    nvram_check_ids(qts);
    /*
     * Out of ROMD mode for now, so this goes through the device: a 4-byte
     * read keeps the bytes' order on this big-endian machine
     */
    check_mtree(qts, NVRAM_FLASH_IO, NVRAM_FLASH);
    g_assert_cmphex(qtest_readl(qts, NVRAM_BASE + bank1), ==, ldl_be_p(data));
    qtest_memread(qts, NVRAM_BASE, buf, NVRAM_SIZE);
    check_bank(buf, bank0, file + bank0);
    check_bank(buf, bank1, data);

    qtest_quit(qts);

    g_free(buf);
    buf = NULL;
    g_assert_true(g_file_get_contents(path, (char **)&buf, &len, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(len, ==, NVRAM_SIZE);
    check_bank(buf, bank0, file + bank0);
    check_bank(buf, bank1, data);
    unlink(path);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("powermac7_3")) {
        qtest_add_func("mac-g5/powermac7_3", test_powermac7_3);
        qtest_add_func("mac-g5/powermac7_3/nvram", test_powermac7_3_nvram);
    }
    if (qtest_has_machine("mac99")) {
        qtest_add_func("mac-g5/mac99-970", test_mac99_970);
    }

    return g_test_run();
}

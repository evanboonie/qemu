/*
 * qtest for the Power Mac G5 machine, powermac7_3
 *
 * Checks what OpenBIOS and the guest see before any firmware runs: the
 * machine ID and VIA configuration in fw_cfg, the devices in U3's AGP
 * bus, U3's version register, where its PCI I/O window is, and its host
 * bridge's AGP capability. mac99's 970 path is checked the same way, since
 * the two share their init.
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
#define FW_CFG_PPC_VIACONFIG    (FW_CFG_ARCH_LOCAL + 0x0b)

/* From hw/ppc/mac_newworld.h */
#define CORE99_VIA_CONFIG_CUDA  0
#define CORE99_VIA_CONFIG_PMU   1

#define NEWWORLD_FW_CFG_BASE    0xf0000510

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

static void check_io_window(QTestState *qts, const char *window,
                            const char *not_window)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree");

    g_assert_nonnull(strstr(mtree, window));
    g_assert_null(strstr(mtree, not_window));
}

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

static void check_machine(const char *machine, uint16_t machine_id,
                          uint32_t via_config, uint32_t u3_version,
                          const char *io_window, const char *not_io_window)
{
    QTestState *qts = qtest_initf("-M %s", machine);
    QFWCFG *fw_cfg = mm_fw_cfg_init(qts, NEWWORLD_FW_CFG_BASE);

    g_assert_cmpuint(qfw_cfg_get_u16(fw_cfg, FW_CFG_MACHINE_ID), ==,
                     machine_id);
    g_assert_cmpuint(qfw_cfg_get_u32(fw_cfg, FW_CFG_PPC_VIACONFIG), ==,
                     via_config);
    mm_fw_cfg_uninit(fw_cfg);

    /* U3's host bridge at device 11, and macio at 12, as on mac99 */
    g_assert_cmphex(u3_config_readl(qts, 11, 0), ==,
                    PCI_DEVICE_ID_APPLE_U3_AGP << 16 | PCI_VENDOR_ID_APPLE);
    g_assert_cmphex(u3_config_readl(qts, 12, 0), ==,
                    PCI_DEVICE_ID_APPLE_UNI_N_KEYL << 16 |
                    PCI_VENDOR_ID_APPLE);

    g_assert_cmphex(qtest_readl(qts, U3_REG_BASE), ==, u3_version);
    check_io_window(qts, io_window, not_io_window);

    /* Last, since it resets the machine */
    check_agp_capability(qts);

    qtest_quit(qts);
}

/* The real PowerMac7,3's U3: device-rev 0x35, PCI I/O at 0xf0000000 */
static void test_powermac7_3(void)
{
    check_machine("powermac7_3", ARCH_POWERMAC7_3, CORE99_VIA_CONFIG_PMU,
                  U3_VERSION_POWERMAC7_3, IO_WINDOW_POWERMAC7_3,
                  IO_WINDOW_MAC99);
}

/* mac99's 970 keeps the G4's UniNorth version and I/O at 0xf2000000 */
static void test_mac99_970(void)
{
    check_machine("mac99 -cpu 970fx", ARCH_MAC99_U3, CORE99_VIA_CONFIG_CUDA,
                  UNINORTH_VERSION_10A, IO_WINDOW_MAC99,
                  IO_WINDOW_POWERMAC7_3);
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_machine("powermac7_3")) {
        qtest_add_func("mac-g5/powermac7_3", test_powermac7_3);
    }
    if (qtest_has_machine("mac99")) {
        qtest_add_func("mac-g5/mac99-970", test_mac99_970);
    }

    return g_test_run();
}

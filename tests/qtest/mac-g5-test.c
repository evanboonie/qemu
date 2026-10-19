/*
 * qtest for the Power Mac G5 machine, powermac7_3
 *
 * Checks what OpenBIOS and the guest see before any firmware runs: the
 * machine ID and VIA configuration in fw_cfg, and the devices in U3's
 * AGP bus. mac99's 970 path is checked the same way, since the two share
 * their init.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqos/fw_cfg.h"
#include "libqtest.h"
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

#define PCI_VENDOR_ID_APPLE             0x106b
#define PCI_DEVICE_ID_APPLE_U3_AGP      0x004b
#define PCI_DEVICE_ID_APPLE_UNI_N_KEYL  0x0022

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

static void check_machine(const char *machine, uint16_t machine_id,
                          uint32_t via_config)
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

    qtest_quit(qts);
}

static void test_powermac7_3(void)
{
    check_machine("powermac7_3", ARCH_POWERMAC7_3, CORE99_VIA_CONFIG_PMU);
}

static void test_mac99_970(void)
{
    check_machine("mac99", ARCH_MAC99_U3, CORE99_VIA_CONFIG_CUDA);
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

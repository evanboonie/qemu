/*
 * QEMU Power Mac G5 (PowerMac7,3) hardware System Emulator
 *
 * The AGP G5, with U3. For now it's mac99's 970 machine with the PMU
 * fixed on, as the real PowerMac7,3 has a via-pmu, and with a machine ID
 * of its own, ARCH_POWERMAC7_3, so OpenBIOS can describe it as a
 * PowerMac7,3 rather than mac99's PowerMac3,1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ppc/ppc.h"
#include "hw/core/boards.h"
#include "hw/core/fw-path-provider.h"
#include "mac_newworld.h"

static void powermac7_3_init(MachineState *machine)
{
    ppc_newworld_init(machine, CORE99_VIA_CONFIG_PMU, true);
}

static void powermac7_3_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);

    ppc_newworld_machine_class_init(mc, fwc);
    mc->desc = "Power Mac G5 (PowerMac7,3)";
    mc->init = powermac7_3_init;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
    mc->default_ram_id = "powermac7_3.ram";
}

static const TypeInfo powermac7_3_machine_info = {
    .name          = MACHINE_TYPE_NAME("powermac7_3"),
    .parent        = TYPE_MACHINE,
    .class_init    = powermac7_3_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_FW_PATH_PROVIDER },
        { }
    },
};

static void mac_g5_register_types(void)
{
    type_register_static(&powermac7_3_machine_info);
}

type_init(mac_g5_register_types)

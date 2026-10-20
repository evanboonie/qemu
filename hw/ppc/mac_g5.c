/*
 * QEMU Power Mac G5 (PowerMac7,3) hardware System Emulator
 *
 * The AGP G5, with U3. For now it's mac99's 970 machine with the PMU
 * fixed on, as the real PowerMac7,3 has a via-pmu, and with a machine ID
 * of its own, ARCH_POWERMAC7_3, so OpenBIOS can describe it as a
 * PowerMac7,3 rather than mac99's PowerMac3,1. Its U3 is laid out as the
 * real one: PCI I/O at 0xf0000000, and version 0x35. Given -drive
 * if=pflash, its NVRAM is the real one's 16 KB of AMD-style flash at
 * 0xfff04000, a pflash_cfi02, in place of mac99's macio-nvram.
 * ppc_newworld_init() builds both.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/qtest.h"
#include "hw/ppc/ppc.h"
#include "hw/core/boards.h"
#include "hw/core/fw-path-provider.h"
#include "mac_newworld.h"

static void powermac7_3_init(MachineState *machine)
{
    /*
     * The bundled openbios-ppc is upstream's, whose entry() indexes its
     * known_arch[] with the machine ID unchecked, so ARCH_POWERMAC7_3 runs
     * it past the array's end. Only an OpenBIOS built from the fork's g5
     * knows the ID. Under the qtest accelerator no guest code runs, and
     * the qtests that start every machine pass no -bios, so they keep the
     * bundled image. A qtest that runs TCG needs -bios like anyone else.
     */
    if (!machine->firmware && !qtest_enabled()) {
        error_report("powermac7_3 needs -bios with an OpenBIOS built from "
                     "the g5 branch of "
                     "https://github.com/evanboonie/openbios; the bundled "
                     "openbios-ppc doesn't support it");
        exit(1);
    }
    ppc_newworld_init(machine, CORE99_VIA_CONFIG_PMU, true);
}

static void powermac7_3_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);
    /*
     * Each 970 model, not "970-family": a list of one would also become
     * the default CPU, and the family type is abstract.
     */
    static const char * const valid_cpu_types[] = {
        POWERPC_CPU_TYPE_NAME("970_v2.2"),
        POWERPC_CPU_TYPE_NAME("970fx_v1.0"),
        POWERPC_CPU_TYPE_NAME("970fx_v2.0"),
        POWERPC_CPU_TYPE_NAME("970fx_v2.1"),
        POWERPC_CPU_TYPE_NAME("970fx_v3.0"),
        POWERPC_CPU_TYPE_NAME("970fx_v3.1"),
        POWERPC_CPU_TYPE_NAME("970mp_v1.0"),
        POWERPC_CPU_TYPE_NAME("970mp_v1.1"),
        NULL,
    };

    ppc_newworld_machine_class_init(mc, fwc);
    mc->desc = "Power Mac G5 (PowerMac7,3)";
    mc->init = powermac7_3_init;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
    mc->valid_cpu_types = valid_cpu_types;
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

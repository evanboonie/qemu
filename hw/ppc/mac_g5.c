/*
 * QEMU Power Mac G5 (PowerMac7,3) hardware System Emulator
 *
 * The AGP G5, with U3. For now it's mac99's 970 machine with the PMU
 * fixed on, as the real PowerMac7,3 has a via-pmu, and with a machine ID
 * of its own, ARCH_POWERMAC7_3, so OpenBIOS can describe it as a
 * PowerMac7,3 rather than mac99's PowerMac3,1. Its U3 is laid out as the
 * real one: PCI I/O at 0xf0000000, and version 0x35. Given -drive
 * if=pflash, its NVRAM is the real one's 16 KB of AMD-style flash at
 * 0xfff04000, a pflash_cfi02, in place of mac99's macio-nvram. Its K2
 * GPIOs start CPUs and freeze their time bases.
 * ppc_newworld_init() builds both.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/qtest.h"
#include "hw/ppc/ppc.h"
#include "hw/core/boards.h"
#include "hw/core/fw-path-provider.h"
#include "hw/core/irq.h"
#include "hw/misc/macio/gpio.h"
#include "target/ppc/cpu.h"
#include "mac_newworld.h"

/*
 * Each CPU's SRESET, active low, is a K2 GPIO, which its cpu node's
 * soft-reset property names: K2 0x71 for cpu@0 and 0x72 for cpu@1, as on
 * the real PowerMac7,3. In the macio GPIO block, whose registers start at
 * K2 0x58, those are GPIOs 0x19 and 0x1a. Mac OS X's MacRISC4CPU starts a
 * CPU by driving its line low and releasing it, back to back
 * (AppleMacRISC4PE-204.0.9, MacRISC4CPU.cpp:632-658), and the CPU then
 * takes a system reset to its vector at 0x100.
 */
#define POWERMAC7_3_GPIO_SOFT_RESET0    0x19
#define POWERMAC7_3_MAX_CPUS            2

/*
 * Every CPU's TBEN is one K2 GPIO, at K2 0x77: offset 0x27 in the gpio
 * block at K2 0x50, as OpenBIOS's timebase-enable@27 node gives it, and
 * QEMU's GPIO 0x1f, since the GPIO registers start at K2 0x58. The real
 * PowerMac7,3's GPIOs don't use that offset (its headphone-mute@1f is
 * offset 0x1f, QEMU's GPIO 0x17). Mac OS X freezes the time bases while it
 * copies CPU 0's to a CPU it starts (xnu-1228.15.4, cpu.c:710-790), with
 * MacRISC4CPU::enableCPUTimeBase (AppleMacRISC4PE-204.0.9,
 * MacRISC4CPU.cpp:777-785), which calls the platform-cpu-timebase function
 * that AppleGPIO publishes for a gpio node named timebase-enable, as on the
 * PCIe G5's timebase-enable@26. Driven low, the time bases hold; released,
 * the line reads high and they count.
 */
#define POWERMAC7_3_GPIO_TIMEBASE_ENABLE 0x1f

/*
 * The falling edge requests the reset, and the release doesn't cancel it:
 * the CPU may run on another thread and not have taken it yet. It's
 * cleared once taken, and it wakes a CPU that's powered off. A 970 halted
 * in power-save (MSR[POW] set) that takes it aborts QEMU instead
 * (powerpc_excp_books(), target/ppc/excp_helper.c), which isn't handled
 * yet (evanboonie/qemu-g5#94).
 */
static void powermac7_3_soft_reset(void *opaque, int n, int level)
{
    if (!level) {
        ppc_set_irq(POWERPC_CPU(opaque), PPC_INTERRUPT_RESET, 1);
    }
}

static void powermac7_3_timebase_enable(void *opaque, int n, int level)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(cs), PPC970_INPUT_TBEN), level);
    }
}

static void powermac7_3_init(MachineState *machine)
{
    DeviceState *gpio;
    int i;

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

    gpio = DEVICE(object_resolve_type_unambiguous(TYPE_MACIO_GPIO,
                                                  &error_abort));
    for (i = 0; i < machine->smp.cpus; i++) {
        qdev_connect_gpio_out_named(gpio, MACIO_GPIO_OUT,
                                    POWERMAC7_3_GPIO_SOFT_RESET0 + i,
                                    qemu_allocate_irq(powermac7_3_soft_reset,
                                                      qemu_get_cpu(i), 0));
    }
    qdev_connect_gpio_out_named(gpio, MACIO_GPIO_OUT,
                                POWERMAC7_3_GPIO_TIMEBASE_ENABLE,
                                qemu_allocate_irq(powermac7_3_timebase_enable,
                                                  NULL, 0));
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
    mc->max_cpus = POWERMAC7_3_MAX_CPUS;
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

/*
 * QEMU NewWorld PowerMac machines: what mac99 and powermac7_3 share
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HW_PPC_MAC_NEWWORLD_H
#define HW_PPC_MAC_NEWWORLD_H

#include "hw/core/boards.h"
#include "hw/core/fw-path-provider.h"

typedef enum {
    CORE99_VIA_CONFIG_CUDA = 0,
    CORE99_VIA_CONFIG_PMU,
    CORE99_VIA_CONFIG_PMU_ADB
} Core99ViaConfig;

/*
 * Build a NewWorld machine. The CPU picks the host bridges and the
 * machine ID: UniNorth's buses and ARCH_MAC99 for a G4, or U3's AGP bus
 * and ARCH_MAC99_U3 for a 970. With powermac7_3, which only takes a 970
 * (its valid_cpu_types), the ID is ARCH_POWERMAC7_3 instead.
 */
void ppc_newworld_init(MachineState *machine, Core99ViaConfig via_config,
                       bool powermac7_3);

/* Set the MachineClass fields mac99 and powermac7_3 have in common. */
void ppc_newworld_machine_class_init(MachineClass *mc,
                                     FWPathProviderClass *fwc);

#endif

/*
 * U3's DART, the Power Mac G5's DMA address relocation table
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_HOST_U3_DART_H
#define HW_PCI_HOST_U3_DART_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_U3_DART "u3-dart"
OBJECT_DECLARE_SIMPLE_TYPE(U3DARTState, U3_DART)

#define TYPE_U3_DART_IOMMU_MEMORY_REGION "u3-dart-iommu-memory-region"

/* Where the real PowerMac7,3 has it, and its node's reg size */
#define U3_DART_BASE            0xf8033000
#define U3_DART_SIZE            0x7000

struct U3DARTState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    IOMMUMemoryRegion iommu;
    AddressSpace iommu_as;

    uint32_t cntl;
};

/*
 * Put a PCI bus's DMA behind the DART. Call it before any device that
 * does DMA is added to the bus, since a device takes its address space
 * when it's realized.
 */
void u3_dart_attach_pci_bus(U3DARTState *s, PCIBus *bus);

#endif

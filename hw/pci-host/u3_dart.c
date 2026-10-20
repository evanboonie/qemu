/*
 * U3's DART, the Power Mac G5's DMA address relocation table
 *
 * The DART turns the bus addresses PCI devices use for DMA into physical
 * ones, through a table of 32-bit entries in main memory, one per 4 KB
 * page of bus address space. U3 has one register for it, DARTCNTL, at
 * offset 0 of its window at 0xf8033000:
 *
 *   bits 31-12  the table's physical page number
 *   bit  10     IDARTTLB: invalidate the DART's TLB
 *   bit   9     DARTEN: translate
 *   bits  8-0   the table's size in 4 KB pages
 *
 * An entry is big-endian: bit 31 valid, and the physical page number in
 * the bits below it. Entry n maps bus page n.
 *
 * As read in Mac OS X 10.5.8's AppleDART (AppleMacRiscPCI-3.4,
 * AppleDART.cpp:47-87, :102-106, :246-252, :256-348) and Linux's
 * arch/powerpc/sysdev/dart.h and dart_iommu.c. Both make a 512-page
 * table, which maps 2 GiB of bus addresses, and both mask 512 with 0x1ff
 * before writing the size field, so a size of 0 is taken to mean 512
 * pages (inferred). Both set IDARTTLB after changing the table and wait
 * for it to read 0, and AppleDART waits forever (AppleDART.cpp:472-502),
 * so here the invalidation completes at once.
 *
 * This model has no TLB: it reads the table on every access, so it can't
 * show a missed invalidation. A DMA through an invalid entry, or past the
 * table's window, fails, and is logged as a guest error and traced. The
 * exception register at 0x10 reads 0: its fields' layout isn't published
 * (evanboonie/qemu-g5#82).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/u3_dart.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

#define DART_CNTL               0x0
#define DART_EXCP               0x10

#define DART_CNTL_BASE_SHIFT    12
#define DART_CNTL_IDARTTLB      (1u << 10)
#define DART_CNTL_DARTEN        (1u << 9)
#define DART_CNTL_SIZE_MASK     0x1ffu

#define DART_PAGE_SHIFT         12
#define DART_PAGE_SIZE          (1ull << DART_PAGE_SHIFT)
#define DART_PAGE_MASK          (DART_PAGE_SIZE - 1)

#define DART_ENTRY_VALID        0x80000000u
#define DART_ENTRY_RPN_MASK     0x00ffffffu

/* Entries in a table page */
#define DART_ENTRIES_PER_PAGE   (DART_PAGE_SIZE / 4)

static uint64_t u3_dart_window(uint32_t cntl)
{
    uint32_t pages = cntl & DART_CNTL_SIZE_MASK;

    if (pages == 0) {
        pages = DART_CNTL_SIZE_MASK + 1;
    }
    return (uint64_t)pages * DART_ENTRIES_PER_PAGE * DART_PAGE_SIZE;
}

static IOMMUTLBEntry u3_dart_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                       IOMMUAccessFlags flag, int iommu_idx)
{
    U3DARTState *s = container_of(iommu, U3DARTState, iommu);
    uint32_t cntl = s->cntl;
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~DART_PAGE_MASK,
        .translated_addr = 0,
        .addr_mask = DART_PAGE_MASK,
        .perm = IOMMU_NONE,
    };
    const char *dir = flag == IOMMU_WO ? "write" : "read";
    hwaddr entry_addr;
    uint32_t entry;

    if (!(cntl & DART_CNTL_DARTEN)) {
        ret.translated_addr = addr & ~DART_PAGE_MASK;
        ret.perm = IOMMU_RW;
        return ret;
    }

    if (addr >= u3_dart_window(cntl)) {
        trace_u3_dart_fault(addr, dir, 0);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "u3-dart: DMA %s at bus address 0x%" HWADDR_PRIx
                      " past the table's 0x%" PRIx64 "-byte window\n",
                      dir, addr, u3_dart_window(cntl));
        return ret;
    }

    entry_addr = ((hwaddr)(cntl >> DART_CNTL_BASE_SHIFT) << DART_PAGE_SHIFT) +
                 (addr >> DART_PAGE_SHIFT) * 4;
    entry = address_space_ldl_be(&address_space_memory, entry_addr,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(entry & DART_ENTRY_VALID)) {
        trace_u3_dart_fault(addr, dir, entry);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "u3-dart: DMA %s at bus address 0x%" HWADDR_PRIx
                      " through invalid entry 0x%08" PRIx32 "\n",
                      dir, addr, entry);
        return ret;
    }

    ret.translated_addr = (hwaddr)(entry & DART_ENTRY_RPN_MASK) <<
                          DART_PAGE_SHIFT;
    ret.perm = IOMMU_RW;
    trace_u3_dart_translate(addr, dir, entry, ret.translated_addr);
    return ret;
}

static uint64_t u3_dart_read(void *opaque, hwaddr addr, unsigned size)
{
    U3DARTState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case DART_CNTL:
        val = s->cntl;
        break;
    case DART_EXCP:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "u3-dart: unimplemented read at 0x%"
                      HWADDR_PRIx "\n", addr);
        break;
    }
    trace_u3_dart_read(addr, val);
    return val;
}

static void u3_dart_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    U3DARTState *s = opaque;

    trace_u3_dart_write(addr, val);
    switch (addr) {
    case DART_CNTL:
        /* There's no TLB, so an invalidation is over at once */
        s->cntl = val & ~DART_CNTL_IDARTTLB;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "u3-dart: unimplemented write at 0x%"
                      HWADDR_PRIx ", 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps u3_dart_ops = {
    .read = u3_dart_read,
    .write = u3_dart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static AddressSpace *u3_dart_pci_address_space(PCIBus *bus, void *opaque,
                                               int devfn)
{
    U3DARTState *s = opaque;

    return &s->iommu_as;
}

static const PCIIOMMUOps u3_dart_pci_iommu_ops = {
    .get_address_space = u3_dart_pci_address_space,
};

void u3_dart_attach_pci_bus(U3DARTState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &u3_dart_pci_iommu_ops, s);
}

static void u3_dart_reset(DeviceState *dev)
{
    U3DARTState *s = U3_DART(dev);

    s->cntl = 0;
}

static void u3_dart_realize(DeviceState *dev, Error **errp)
{
    U3DARTState *s = U3_DART(dev);
    Object *obj = OBJECT(dev);

    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_U3_DART_IOMMU_MEMORY_REGION, obj,
                             "u3-dart-iommu", UINT64_MAX);
    address_space_init(&s->iommu_as, MEMORY_REGION(&s->iommu), "u3-dart");

    memory_region_init_io(&s->mem, obj, &u3_dart_ops, s, "u3-dart",
                          U3_DART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mem);
}

static const VMStateDescription vmstate_u3_dart = {
    .name = "u3-dart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntl, U3DARTState),
        VMSTATE_END_OF_LIST()
    },
};

static void u3_dart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = u3_dart_realize;
    device_class_set_legacy_reset(dc, u3_dart_reset);
    dc->vmsd = &vmstate_u3_dart;
    dc->desc = "U3 DART (Power Mac G5 DMA address relocation table)";
    dc->user_creatable = false;
}

static void u3_dart_iommu_memory_region_class_init(ObjectClass *oc,
                                                   const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = u3_dart_translate;
}

static const TypeInfo u3_dart_types[] = {
    {
        .name          = TYPE_U3_DART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(U3DARTState),
        .class_init    = u3_dart_class_init,
    },
    {
        .name          = TYPE_U3_DART_IOMMU_MEMORY_REGION,
        .parent        = TYPE_IOMMU_MEMORY_REGION,
        .class_init    = u3_dart_iommu_memory_region_class_init,
    },
};

DEFINE_TYPES(u3_dart_types)

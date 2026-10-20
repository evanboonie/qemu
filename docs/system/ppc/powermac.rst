PowerMac family boards (``g3beige``, ``mac99``, ``powermac7_3``)
==================================================================

Use the executable ``qemu-system-ppc`` to simulate a complete PowerMac
PowerPC system.

- ``g3beige``              Heathrow based PowerMac
- ``mac99``                Mac99 based PowerMac
- ``powermac7_3``          Power Mac G5 (PowerMac7,3), in
  ``qemu-system-ppc64`` only

``powermac7_3`` is the machine ``mac99`` builds for a 970, with U3's AGP
bus, the PMU and a machine ID of its own for the firmware. Its U3 is laid
out as the real PowerMac7,3's: its PCI I/O window is at ``0xf0000000``,
where ``mac99`` has it at ``0xf2000000``, and its version register reads
``0x35``. It takes only a 970 CPU. The bundled OpenBIOS doesn't support
it, so it needs ``-bios`` with an OpenBIOS built from the ``g5`` branch
of https://github.com/evanboonie/openbios, and refuses to start without
one.

Given a 16 KB raw file with ``-drive if=pflash,format=raw,file=nvram.img``,
``powermac7_3``'s NVRAM is the real one's AMD-style flash at
``0xfff04000``, which keeps its contents in the file, in place of
``mac99``'s plain NVRAM. An erased flash is a file of ``0xFF`` bytes,
such as ``head -c 16384 /dev/zero | tr '\0' '\377' > nvram.img``, since
``truncate`` and ``qemu-img create`` give zeros. The OpenBIOS fork
doesn't support the flash yet, and ``-prom-env`` reaches OpenBIOS only
through the plain NVRAM, so without the option the machine keeps that,
and with it ``-prom-env`` is ignored, with a warning.

Supported devices
-----------------

QEMU emulates the following PowerMac peripherals:

 *  UniNorth or Grackle PCI Bridge
 *  PCI VGA compatible card with VESA Bochs Extensions
 *  2 PMAC IDE interfaces with hard disk and CD-ROM support
 *  NE2000 PCI adapters
 *  Non Volatile RAM
 *  VIA-CUDA with ADB keyboard and mouse.


Missing devices
---------------

 * To be identified

Firmware
--------

Since version 0.9.1, QEMU uses OpenBIOS https://www.openbios.org/ for
the g3beige and mac99 PowerMac and the 40p machines. OpenBIOS is a free
(GPL v2) portable firmware implementation. The goal is to implement a
100% IEEE 1275-1994 (referred to as Open Firmware) compliant firmware.

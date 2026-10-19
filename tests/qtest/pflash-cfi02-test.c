/*
 * QTest testcase for parallel flash with AMD command set
 *
 * Copyright (c) 2019 Stephen Checkoway
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

/*
 * To test the pflash_cfi02 device, we run QEMU with the sh4 r2d machine with
 * a pflash drive. This enables us to test some flash configurations, but not
 * all. In particular, we're limited to a 16-bit wide flash device.
 */

/*
 * These need to match the flash size and address in r2d.c.
 * If the flash size changes then the sector_len[] and nb_blocs[]
 * values in configuration[] below will need to be updated to match.
 */
#define BASE_ADDR 0x00000000

#define UNIFORM_FLASH_SIZE (16 * 1024 * 1024)
#define UNIFORM_FLASH_SECTOR_SIZE (64 * 1024)

/* These must match the id0, id1 args to pflash_cfi02_register() in r2d.c */
#define FLASH_ID0 0x0001
#define FLASH_ID1 0x227E

/* For the TCG guest: r2d's SDRAM past the code -kernel loads, and the data. */
#define GUEST_RESULT_ADDR 0x0c900000
#define GUEST_PATTERN_OFFSET 0x100
#define GUEST_PATTERN "\x01\x02\x03\x04\x05\x06\x07\x08"

/* Use a newtype to keep flash addresses separate from byte addresses. */
typedef struct {
    uint64_t addr;
} faddr;
#define FLASH_ADDR(x) ((faddr) { .addr = (x) })

#define CFI_ADDR FLASH_ADDR(0x55)
#define UNLOCK0_ADDR FLASH_ADDR(0x555)
#define UNLOCK1_ADDR FLASH_ADDR(0x2AA)

#define CFI_CMD 0x98
#define UNLOCK0_CMD 0xAA
#define UNLOCK1_CMD 0x55
#define SECOND_UNLOCK_CMD 0x80
#define AUTOSELECT_CMD 0x90
#define RESET_CMD 0xF0
#define PROGRAM_CMD 0xA0
#define SECTOR_ERASE_CMD 0x30
#define CHIP_ERASE_CMD 0x10
#define UNLOCK_BYPASS_CMD 0x20
#define UNLOCK_BYPASS_RESET_CMD 0x00
#define ERASE_SUSPEND_CMD 0xB0
#define ERASE_RESUME_CMD SECTOR_ERASE_CMD

typedef struct {
    int bank_width;

    /* Nonuniform block size. */
    int nb_blocs[4];
    int sector_len[4];

    QTestState *qtest;
} FlashConfig;

static char *image_path;
static char *guest_code_path;
static char *guest_image_path;

/*
 * The pflash implementation allows some parameters to be unspecified. We want
 * to test those configurations but we also need to know the real values in
 * our testing code. So after we launch qemu, we'll need a new FlashConfig
 * with the correct values filled in.
 */
static FlashConfig expand_config_defaults(const FlashConfig *c)
{
    FlashConfig ret = *c;

    if (ret.bank_width == 0) {
        ret.bank_width = 2;
    }
    if (ret.nb_blocs[0] == 0 && ret.sector_len[0] == 0) {
        ret.sector_len[0] = UNIFORM_FLASH_SECTOR_SIZE;
        ret.nb_blocs[0] = UNIFORM_FLASH_SIZE / UNIFORM_FLASH_SECTOR_SIZE;
    }

    /* XXX: Limitations of test harness. */
    assert(ret.bank_width == 2);
    return ret;
}

/*
 * Return a bit mask suitable for extracting the least significant
 * status/query response from an interleaved response.
 */
static inline uint64_t device_mask(const FlashConfig *c)
{
    return (uint64_t)-1;
}

/*
 * Return a bit mask exactly as long as the bank_width.
 */
static inline uint64_t bank_mask(const FlashConfig *c)
{
    if (c->bank_width == 8) {
        return (uint64_t)-1;
    }
    return (1ULL << (c->bank_width * 8)) - 1ULL;
}

static inline void flash_write(const FlashConfig *c, uint64_t byte_addr,
                               uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~bank_mask(c)) == 0);
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        qtest_writeb(c->qtest, addr, data);
        break;
    case 2:
        qtest_writew(c->qtest, addr, data);
        break;
    case 4:
        qtest_writel(c->qtest, addr, data);
        break;
    case 8:
        qtest_writeq(c->qtest, addr, data);
        break;
    default:
        abort();
    }
}

static inline uint64_t flash_read(const FlashConfig *c, uint64_t byte_addr)
{
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        return qtest_readb(c->qtest, addr);
    case 2:
        return qtest_readw(c->qtest, addr);
    case 4:
        return qtest_readl(c->qtest, addr);
    case 8:
        return qtest_readq(c->qtest, addr);
    default:
        abort();
    }
}

/*
 * Convert a flash address expressed in the maximum width of the device as a
 * byte address.
 */
static inline uint64_t as_byte_addr(const FlashConfig *c, faddr flash_addr)
{
    /*
     * Command addresses are always given as addresses in the maximum
     * supported bus size for the flash chip. So an x8/x16 chip in x8 mode
     * uses addresses 0xAAA and 0x555 to unlock because the least significant
     * bit is ignored. (0x555 rather than 0x554 is traditional.)
     *
     * In general we need to multiply by the maximum device width.
     */
    return flash_addr.addr * c->bank_width;
}

/*
 * Return the command value or expected status replicated across all devices.
 */
static inline uint64_t replicate(const FlashConfig *c, uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~device_mask(c)) == 0);
    return data;
}

static inline void flash_cmd(const FlashConfig *c, faddr cmd_addr,
                             uint8_t cmd)
{
    flash_write(c, as_byte_addr(c, cmd_addr), replicate(c, cmd));
}

static inline uint64_t flash_query(const FlashConfig *c, faddr query_addr)
{
    return flash_read(c, as_byte_addr(c, query_addr));
}

static inline uint64_t flash_query_1(const FlashConfig *c, faddr query_addr)
{
    return flash_query(c, query_addr) & device_mask(c);
}

static void unlock(const FlashConfig *c)
{
    flash_cmd(c, UNLOCK0_ADDR, UNLOCK0_CMD);
    flash_cmd(c, UNLOCK1_ADDR, UNLOCK1_CMD);
}

static void reset(const FlashConfig *c)
{
    flash_cmd(c, FLASH_ADDR(0), RESET_CMD);
}

static void sector_erase(const FlashConfig *c, uint64_t byte_addr)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, SECOND_UNLOCK_CMD);
    unlock(c);
    flash_write(c, byte_addr, replicate(c, SECTOR_ERASE_CMD));
}

static void wait_for_completion(const FlashConfig *c, uint64_t byte_addr)
{
    /* If DQ6 is toggling, step the clock and ensure the toggle stops. */
    const uint64_t dq6 = replicate(c, 0x40);
    if ((flash_read(c, byte_addr) & dq6) ^ (flash_read(c, byte_addr) & dq6)) {
        /* Wait for erase or program to finish. */
        qtest_clock_step_next(c->qtest);
        /* Ensure that DQ6 has stopped toggling. */
        g_assert_cmphex(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));
    }
}

static void bypass_program(const FlashConfig *c, uint64_t byte_addr,
                           uint16_t data)
{
    flash_cmd(c, UNLOCK0_ADDR, PROGRAM_CMD);
    flash_write(c, byte_addr, data);
    /*
     * Data isn't valid until DQ6 stops toggling. We don't model this as
     * writes are immediate, but if this changes in the future, we can wait
     * until the program is complete.
     */
    wait_for_completion(c, byte_addr);
}

static void program(const FlashConfig *c, uint64_t byte_addr, uint16_t data)
{
    unlock(c);
    bypass_program(c, byte_addr, data);
}

static void chip_erase(const FlashConfig *c)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, SECOND_UNLOCK_CMD);
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, CHIP_ERASE_CMD);
}

static void erase_suspend(const FlashConfig *c)
{
    flash_cmd(c, FLASH_ADDR(0), ERASE_SUSPEND_CMD);
}

static void erase_resume(const FlashConfig *c)
{
    flash_cmd(c, FLASH_ADDR(0), ERASE_RESUME_CMD);
}

/*
 * Test flash commands with a variety of device geometry.
 */
static void test_geometry(const void *opaque)
{
    const FlashConfig *config = opaque;
    QTestState *qtest;
    qtest = qtest_initf("-M r2d"
                        " -drive if=pflash,file=%s,format=raw,copy-on-read=on"
                        /* Device geometry properties. */
                        " -global driver=cfi.pflash02,"
                        "property=num-blocks0,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=sector-length0,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=num-blocks1,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=sector-length1,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=num-blocks2,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=sector-length2,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=num-blocks3,value=%d"
                        " -global driver=cfi.pflash02,"
                        "property=sector-length3,value=%d",
                        image_path,
                        config->nb_blocs[0],
                        config->sector_len[0],
                        config->nb_blocs[1],
                        config->sector_len[1],
                        config->nb_blocs[2],
                        config->sector_len[2],
                        config->nb_blocs[3],
                        config->sector_len[3]);
    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    /* Check the IDs. */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, AUTOSELECT_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));
    if (c->bank_width >= 2) {
        /*
         * XXX: The ID returned by the r2d flash chip is 16 bits which
         * wouldn't happen with an 8-bit device. It would probably be best to
         * prohibit addresses larger than the device width in pflash_cfi02.c,
         * but then we couldn't test smaller device widths at all.
         */
        g_assert_cmphex(flash_query(c, FLASH_ADDR(1)), ==,
                        replicate(c, FLASH_ID1));
    }
    reset(c);

    /* Check the erase blocks. */
    flash_cmd(c, CFI_ADDR, CFI_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x10)), ==, replicate(c, 'Q'));
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x11)), ==, replicate(c, 'R'));
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x12)), ==, replicate(c, 'Y'));

    /* Num erase regions. */
    int nb_erase_regions = flash_query_1(c, FLASH_ADDR(0x2C));
    g_assert_cmphex(nb_erase_regions, ==,
                    !!c->nb_blocs[0] + !!c->nb_blocs[1] + !!c->nb_blocs[2] +
                    !!c->nb_blocs[3]);

    /* Check device length. */
    uint32_t device_len = 1 << flash_query_1(c, FLASH_ADDR(0x27));
    g_assert_cmphex(device_len, ==, UNIFORM_FLASH_SIZE);

    /* Check that erase suspend to read/write is supported. */
    uint16_t pri = flash_query_1(c, FLASH_ADDR(0x15)) +
                   (flash_query_1(c, FLASH_ADDR(0x16)) << 8);
    g_assert_cmpint(pri, >=, 0x2D + 4 * nb_erase_regions);
    g_assert_cmpint(flash_query(c, FLASH_ADDR(pri + 0)), ==, replicate(c, 'P'));
    g_assert_cmpint(flash_query(c, FLASH_ADDR(pri + 1)), ==, replicate(c, 'R'));
    g_assert_cmpint(flash_query(c, FLASH_ADDR(pri + 2)), ==, replicate(c, 'I'));
    g_assert_cmpint(flash_query_1(c, FLASH_ADDR(pri + 6)), ==, 2); /* R/W */
    reset(c);

    const uint64_t dq7 = replicate(c, 0x80);
    const uint64_t dq6 = replicate(c, 0x40);
    const uint64_t dq3 = replicate(c, 0x08);
    const uint64_t dq2 = replicate(c, 0x04);

    uint64_t byte_addr = 0;
    for (int region = 0; region < nb_erase_regions; ++region) {
        uint64_t base = 0x2D + 4 * region;
        flash_cmd(c, CFI_ADDR, CFI_CMD);
        uint32_t nb_sectors = flash_query_1(c, FLASH_ADDR(base + 0)) +
                              (flash_query_1(c, FLASH_ADDR(base + 1)) << 8) + 1;
        uint32_t sector_len = (flash_query_1(c, FLASH_ADDR(base + 2)) << 8) +
                              (flash_query_1(c, FLASH_ADDR(base + 3)) << 16);
        g_assert_cmphex(nb_sectors, ==, c->nb_blocs[region]);
        g_assert_cmphex(sector_len, ==, c->sector_len[region]);
        reset(c);

        /* Erase and program sector. */
        for (uint32_t i = 0; i < nb_sectors; ++i) {
            sector_erase(c, byte_addr);

            /* Check that DQ3 is 0. */
            g_assert_cmphex(flash_read(c, byte_addr) & dq3, ==, 0);
            qtest_clock_step_next(c->qtest); /* Step over the 50 us timeout. */

            /* Check that DQ3 is 1. */
            uint64_t status0 = flash_read(c, byte_addr);
            g_assert_cmphex(status0 & dq3, ==, dq3);

            /* DQ7 is 0 during an erase. */
            g_assert_cmphex(status0 & dq7, ==, 0);
            uint64_t status1 = flash_read(c, byte_addr);

            /* DQ6 toggles during an erase. */
            g_assert_cmphex(status0 & dq6, ==, ~status1 & dq6);

            /* Wait for erase to complete. */
            wait_for_completion(c, byte_addr);

            /* Ensure DQ6 has stopped toggling. */
            g_assert_cmphex(flash_read(c, byte_addr), ==,
                            flash_read(c, byte_addr));

            /* Now the data should be valid. */
            g_assert_cmphex(flash_read(c, byte_addr), ==, bank_mask(c));

            /* Program a bit pattern. */
            program(c, byte_addr, 0x55);
            g_assert_cmphex(flash_read(c, byte_addr) & 0xFF, ==, 0x55);
            program(c, byte_addr, 0xA5);
            g_assert_cmphex(flash_read(c, byte_addr) & 0xFF, ==, 0x05);
            byte_addr += sector_len;
        }
    }

    /* Erase the chip. */
    chip_erase(c);
    /* Read toggle. */
    uint64_t status0 = flash_read(c, 0);
    /* DQ7 is 0 during an erase. */
    g_assert_cmphex(status0 & dq7, ==, 0);
    uint64_t status1 = flash_read(c, 0);
    /* DQ6 toggles during an erase. */
    g_assert_cmphex(status0 & dq6, ==, ~status1 & dq6);
    /* Wait for erase to complete. */
    qtest_clock_step_next(c->qtest);
    /* Ensure DQ6 has stopped toggling. */
    g_assert_cmphex(flash_read(c, 0), ==, flash_read(c, 0));
    /* Now the data should be valid. */

    for (int region = 0; region < nb_erase_regions; ++region) {
        for (uint32_t i = 0; i < c->nb_blocs[region]; ++i) {
            byte_addr = (uint64_t)i * c->sector_len[region];
            g_assert_cmphex(flash_read(c, byte_addr), ==, bank_mask(c));
        }
    }

    /* Unlock bypass */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, UNLOCK_BYPASS_CMD);
    bypass_program(c, 0 * c->bank_width, 0x01);
    bypass_program(c, 1 * c->bank_width, 0x23);
    bypass_program(c, 2 * c->bank_width, 0x45);
    /*
     * Test that bypass programming, unlike normal programming can use any
     * address for the PROGRAM_CMD.
     */
    flash_cmd(c, FLASH_ADDR(3 * c->bank_width), PROGRAM_CMD);
    flash_write(c, 3 * c->bank_width, 0x67);
    wait_for_completion(c, 3 * c->bank_width);
    flash_cmd(c, FLASH_ADDR(0), UNLOCK_BYPASS_RESET_CMD);
    bypass_program(c, 4 * c->bank_width, 0x89); /* Should fail. */
    g_assert_cmphex(flash_read(c, 0 * c->bank_width), ==, 0x01);
    g_assert_cmphex(flash_read(c, 1 * c->bank_width), ==, 0x23);
    g_assert_cmphex(flash_read(c, 2 * c->bank_width), ==, 0x45);
    g_assert_cmphex(flash_read(c, 3 * c->bank_width), ==, 0x67);
    g_assert_cmphex(flash_read(c, 4 * c->bank_width), ==, bank_mask(c));

    /* Test ignored high order bits of address. */
    flash_cmd(c, FLASH_ADDR(0x5555), UNLOCK0_CMD);
    flash_cmd(c, FLASH_ADDR(0x2AAA), UNLOCK1_CMD);
    flash_cmd(c, FLASH_ADDR(0x5555), AUTOSELECT_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));
    reset(c);

    /*
     * Program a word on each sector, erase one or two sectors per region, and
     * verify that all of those, and only those, are erased.
     */
    byte_addr = 0;
    for (int region = 0; region < nb_erase_regions; ++region) {
        for (int i = 0; i < config->nb_blocs[region]; ++i) {
            program(c, byte_addr, 0);
            byte_addr += config->sector_len[region];
        }
    }
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, SECOND_UNLOCK_CMD);
    unlock(c);
    byte_addr = 0;
    const uint64_t erase_cmd = replicate(c, SECTOR_ERASE_CMD);
    for (int region = 0; region < nb_erase_regions; ++region) {
        flash_write(c, byte_addr, erase_cmd);
        if (c->nb_blocs[region] > 1) {
            flash_write(c, byte_addr + c->sector_len[region], erase_cmd);
        }
        byte_addr += c->sector_len[region] * c->nb_blocs[region];
    }

    qtest_clock_step_next(c->qtest); /* Step over the 50 us timeout. */
    wait_for_completion(c, 0);
    byte_addr = 0;
    for (int region = 0; region < nb_erase_regions; ++region) {
        for (int i = 0; i < config->nb_blocs[region]; ++i) {
            if (i < 2) {
                g_assert_cmphex(flash_read(c, byte_addr), ==, bank_mask(c));
            } else {
                g_assert_cmphex(flash_read(c, byte_addr), ==, 0);
            }
            byte_addr += config->sector_len[region];
        }
    }

    /* Test erase suspend/resume during erase timeout. */
    sector_erase(c, 0);
    /*
     * Check that DQ 3 is 0 and DQ6 and DQ2 are toggling in the sector being
     * erased as well as in a sector not being erased.
     */
    byte_addr = c->sector_len[0];
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq3, ==, 0);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    status0 = flash_read(c, byte_addr);
    status1 = flash_read(c, byte_addr);
    g_assert_cmpint(status0 & dq3, ==, 0);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);

    /*
     * Check that after suspending, DQ6 does not toggle but DQ2 does toggle in
     * an erase suspended sector but that neither toggle (we should be
     * getting data) in a sector not being erased.
     */
    erase_suspend(c);
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq6, ==, status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    g_assert_cmpint(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));

    /* Check that after resuming, DQ3 is 1 and DQ6 and DQ2 toggle. */
    erase_resume(c);
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    status0 = flash_read(c, byte_addr);
    status1 = flash_read(c, byte_addr);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    wait_for_completion(c, 0);

    /* Repeat this process but this time suspend after the timeout. */
    sector_erase(c, 0);
    qtest_clock_step_next(c->qtest);
    /*
     * Check that DQ 3 is 1 and DQ6 and DQ2 are toggling in the sector being
     * erased as well as in a sector not being erased.
     */
    byte_addr = c->sector_len[0];
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    status0 = flash_read(c, byte_addr);
    status1 = flash_read(c, byte_addr);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);

    /*
     * Check that after suspending, DQ6 does not toggle but DQ2 does toggle in
     * an erase suspended sector but that neither toggle (we should be
     * getting data) in a sector not being erased.
     */
    erase_suspend(c);
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq6, ==, status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    g_assert_cmpint(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));

    /* Check that after resuming, DQ3 is 1 and DQ6 and DQ2 toggle. */
    erase_resume(c);
    status0 = flash_read(c, 0);
    status1 = flash_read(c, 0);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    status0 = flash_read(c, byte_addr);
    status1 = flash_read(c, byte_addr);
    g_assert_cmpint(status0 & dq3, ==, dq3);
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    g_assert_cmpint(status0 & dq2, ==, ~status1 & dq2);
    wait_for_completion(c, 0);

    qtest_quit(qtest);
}

/*
 * Test that
 * 1. enter autoselect mode;
 * 2. enter CFI mode; and then
 * 3. exit CFI mode
 * leaves the flash device in autoselect mode.
 */
static void test_cfi_in_autoselect(const void *opaque)
{
    const FlashConfig *config = opaque;
    QTestState *qtest;
    qtest = qtest_initf("-M r2d"
                        " -drive if=pflash,file=%s,format=raw,copy-on-read=on",
                        image_path);
    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    /* 1. Enter autoselect. */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, AUTOSELECT_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));

    /* 2. Enter CFI. */
    flash_cmd(c, CFI_ADDR, CFI_CMD);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x10)), ==, replicate(c, 'Q'));
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x11)), ==, replicate(c, 'R'));
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0x12)), ==, replicate(c, 'Y'));

    /* 3. Exit CFI. */
    reset(c);
    g_assert_cmphex(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, FLASH_ID0));

    qtest_quit(qtest);
}

/*
 * A guest's 8-byte accesses to the flash. qtest's own accesses are split to
 * the device's maximum access size before they reach it, so this runs a few
 * SH-4 instructions under TCG, whose 8-byte loads and stores (fmov.d with
 * FPSCR.SZ set) reach the device as one access. Each 8-byte load is compared
 * with two 4-byte loads of the same 8 bytes:
 *   R + 0x00: in unlock bypass mode, which turns ROMD off, the 8-byte load of
 *             GUEST_PATTERN; R + 0x08 and R + 0x0c, the two 4-byte loads.
 *   R + 0x10: in CFI query mode, the 8-byte load of "QRY"; R + 0x18 and
 *             R + 0x1c, the two 4-byte loads, made after an 8-byte store of
 *             RESET_CMD, which the device must refuse.
 *   R + 0x20: 1 once the guest is done.
 * Leaving ROMD flushes the TLB asynchronously, at the start of the next
 * translation block. fschg ends the block, so the code doesn't load from the
 * flash between the command that leaves ROMD and the next fschg.
 */
static const uint16_t guest_code[] = {
    0xd317, /* mov.l   @(0x60,pc),r3  ! r3 = R, the results, in P2 */
    0xd518, /* mov.l   @(0x64,pc),r5  ! r5 = UNLOCK0_ADDR in bytes */
    0xd618, /* mov.l   @(0x68,pc),r6  ! r6 = UNLOCK1_ADDR in bytes */
    0xe0aa, /* mov     #0xaa,r0 */
    0x600c, /* extu.b  r0,r0 */
    0x2501, /* mov.w   r0,@r5 */
    0xe055, /* mov     #0x55,r0 */
    0x2601, /* mov.w   r0,@r6 */
    0xe020, /* mov     #0x20,r0 */
    0x2501, /* mov.w   r0,@r5         ! unlock bypass: ROMD is off */
    0xd215, /* mov.l   @(0x6c,pc),r2  ! r2 = the pattern, at byte 0x100 */
    0xf3fd, /* fschg                  ! FPSCR.SZ = 1 */
    0xf028, /* fmov.d  @r2,dr0        ! 8-byte array read */
    0xf30a, /* fmov.d  dr0,@r3        ! R + 0 */
    0xf3fd, /* fschg                  ! FPSCR.SZ = 0 */
    0x6422, /* mov.l   @r2,r4 */
    0x1342, /* mov.l   r4,@(8,r3) */
    0x7204, /* add     #4,r2 */
    0x6422, /* mov.l   @r2,r4 */
    0x1343, /* mov.l   r4,@(12,r3) */
    0xe090, /* mov     #0x90,r0 */
    0x600c, /* extu.b  r0,r0 */
    0x2501, /* mov.w   r0,@r5 */
    0xe000, /* mov     #0,r0 */
    0x2501, /* mov.w   r0,@r5         ! unlock bypass reset */
    0xd10f, /* mov.l   @(0x70,pc),r1  ! r1 = CFI_ADDR in bytes */
    0xe098, /* mov     #0x98,r0 */
    0x600c, /* extu.b  r0,r0 */
    0x2101, /* mov.w   r0,@r1         ! CFI query mode */
    0xd20e, /* mov.l   @(0x74,pc),r2  ! r2 = "QRY", at FLASH_ADDR(0x10) */
    0x7310, /* add     #16,r3 */
    0xf3fd, /* fschg                  ! FPSCR.SZ = 1 */
    0xf028, /* fmov.d  @r2,dr0        ! 8-byte CFI read */
    0xf30a, /* fmov.d  dr0,@r3        ! R + 0x10 */
    0xd40c, /* mov.l   @(0x78,pc),r4  ! r4 = the 8-byte reset command below */
    0xf248, /* fmov.d  @r4,dr2 */
    0xf22a, /* fmov.d  dr2,@r2        ! 8-byte write: refused */
    0xf3fd, /* fschg                  ! FPSCR.SZ = 0 */
    0x6422, /* mov.l   @r2,r4         ! still in CFI mode */
    0x1342, /* mov.l   r4,@(8,r3) */
    0x7204, /* add     #4,r2 */
    0x6422, /* mov.l   @r2,r4 */
    0x1343, /* mov.l   r4,@(12,r3) */
    0xe401, /* mov     #1,r4 */
    0x1344, /* mov.l   r4,@(16,r3)    ! done */
    0xaffe, /* bra     . */
    0x0009, /* nop */
    0x0009, /* nop, to align the literals */
    0x0000, 0xac90, /* 0x60: 0xac900000 */
    0x0aaa, 0xa000, /* 0x64: 0xa0000aaa */
    0x0554, 0xa000, /* 0x68: 0xa0000554 */
    0x0100, 0xa000, /* 0x6c: 0xa0000100 */
    0x00aa, 0xa000, /* 0x70: 0xa00000aa */
    0x0020, 0xa000, /* 0x74: 0xa0000020 */
    0x0080, 0xac80, /* 0x78: 0xac800080 */
    0x0000, /* padding, to align the next literal */
    0x0000, /* padding, to align the next literal */
    0x00f0, 0x0000, 0x0000, 0x0000, /* 0x80: RESET_CMD, 8 bytes */
};

static void test_guest_8byte_access(void)
{
    QTestState *qtest;
    GError *err = NULL;
    uint16_t code[ARRAY_SIZE(guest_code)];
    uint8_t res[0x20];
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is not available");
        return;
    }

    /* A flash image with GUEST_PATTERN at byte GUEST_PATTERN_OFFSET. */
    fd = g_file_open_tmp("qtest-image.XXXXXX", &guest_image_path, &err);
    g_assert_no_error(err);
    g_assert(ftruncate(fd, UNIFORM_FLASH_SIZE) == 0);
    g_assert(pwrite(fd, GUEST_PATTERN, sizeof(GUEST_PATTERN) - 1,
                    GUEST_PATTERN_OFFSET) == sizeof(GUEST_PATTERN) - 1);
    close(fd);

    /* r2d's SH-4 is little-endian. */
    for (size_t i = 0; i < ARRAY_SIZE(guest_code); i++) {
        code[i] = cpu_to_le16(guest_code[i]);
    }
    fd = g_file_open_tmp("qtest-code.XXXXXX", &guest_code_path, &err);
    g_assert_no_error(err);
    close(fd);
    g_file_set_contents(guest_code_path, (const char *)code, sizeof(code),
                        &err);
    g_assert_no_error(err);

    qtest = qtest_initf("-M r2d -accel tcg -kernel %s"
                        " -drive if=pflash,file=%s,format=raw",
                        guest_code_path, guest_image_path);
    /* QEMU has loaded the code by now. */
    unlink(guest_code_path);
    g_clear_pointer(&guest_code_path, g_free);

    for (int i = 0; i < 1000; i++) {
        if (qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20) == 1) {
            break;
        }
        g_usleep(10 * 1000);
    }
    g_assert_cmphex(qtest_readl(qtest, GUEST_RESULT_ADDR + 0x20), ==, 1);
    qtest_memread(qtest, GUEST_RESULT_ADDR, res, sizeof(res));

    /* Unlock bypass: the 4-byte loads read the pattern, and so does ldq. */
    g_assert_cmpmem(&res[0x08], 8, GUEST_PATTERN, 8);
    g_assert_cmphex(ldq_le_p(&res[0x00]), ==, ldq_le_p(&res[0x08]));

    /*
     * CFI query: a 16-bit wide flash returns one CFI byte per 4-byte load.
     * Reading 'Q' after the 8-byte store of RESET_CMD shows it was refused.
     */
    g_assert_cmphex(ldl_le_p(&res[0x18]), ==, 'Q');
    g_assert_cmphex(ldl_le_p(&res[0x1c]), ==, 'Y');
    g_assert_cmphex(ldq_le_p(&res[0x10]), ==, ldq_le_p(&res[0x18]));

    qtest_quit(qtest);
    unlink(guest_image_path);
    g_clear_pointer(&guest_image_path, g_free);
}

static void cleanup(void *opaque)
{
    unlink(image_path);
    g_free(image_path);
    if (guest_code_path) {
        unlink(guest_code_path);
    }
    if (guest_image_path) {
        unlink(guest_image_path);
    }
}

/*
 * XXX: Tests are limited to bank_width = 2 for now because that's what
 * hw/sh4/r2d.c has.
 */
static const FlashConfig configuration[] = {
    /* One x16 device. */
    {
        .bank_width = 2,
    },
    /* Nonuniform sectors (top boot). */
    {
        .bank_width = 2,
        .nb_blocs = { 127, 1, 2, 1 },
        .sector_len = { 0x20000, 0x10000, 0x04000, 0x08000 },
    },
    /* Nonuniform sectors (bottom boot). */
    {
        .bank_width = 2,
        .nb_blocs = { 1, 2, 1, 127 },
        .sector_len = { 0x08000, 0x04000, 0x10000, 0x20000 },
    },
};

int main(int argc, char **argv)
{
    GError *err = NULL;
    int fd = g_file_open_tmp("qtest.XXXXXX", &image_path, &err);
    g_assert_no_error(err);

    if (ftruncate(fd, UNIFORM_FLASH_SIZE) < 0) {
        int error_code = errno;
        close(fd);
        cleanup(NULL);
        g_printerr("Failed to truncate file %s to %u MB: %s\n", image_path,
                   UNIFORM_FLASH_SIZE, strerror(error_code));
        exit(EXIT_FAILURE);
    }
    close(fd);

    qtest_add_abrt_handler(cleanup, NULL);
    g_test_init(&argc, &argv, NULL);

    size_t nb_configurations = sizeof configuration / sizeof configuration[0];
    for (size_t i = 0; i < nb_configurations; ++i) {
        const FlashConfig *config = &configuration[i];
        char *path = g_strdup_printf("pflash-cfi02"
                                     "/geometry/%dx%x-%dx%x-%dx%x-%dx%x"
                                     "/%d",
                                     config->nb_blocs[0],
                                     config->sector_len[0],
                                     config->nb_blocs[1],
                                     config->sector_len[1],
                                     config->nb_blocs[2],
                                     config->sector_len[2],
                                     config->nb_blocs[3],
                                     config->sector_len[3],
                                     config->bank_width);
        qtest_add_data_func(path, config, test_geometry);
        g_free(path);
    }

    qtest_add_data_func("pflash-cfi02/cfi-in-autoselect", &configuration[0],
                        test_cfi_in_autoselect);
    qtest_add_func("pflash-cfi02/guest-8byte-access",
                   test_guest_8byte_access);
    int result = g_test_run();
    cleanup(NULL);
    return result;
}

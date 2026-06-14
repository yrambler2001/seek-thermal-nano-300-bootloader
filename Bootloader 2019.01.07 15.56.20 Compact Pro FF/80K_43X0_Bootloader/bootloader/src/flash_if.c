/* flash_if.c — 80K_43X0_Bootloader (reconstructed)
 *
 * The flash-access layer the boot logic sits on: a memcpy-style read, two
 * blank/equality checks, and program/erase built on lpcspifilib (IMPORTED).
 * Program and erase against the memory-mapped SPI-NOR window drop the
 * controller out of XIP, drive explicit commands, then restore XIP.
 * Names are kept identical to the disassembly.
 */

 #include "bootloader.h"
#include "spifilib_api.h"
#include "chip.h"

extern void *g_spifi_handle;   /* IDA: dword_1001DCAC (defined in spifi_glue.c) */

/* RMW staging-buffer base. The disassembly reads this pointer from a fixed cell
 * at 0x20000000 (AHB SRAM); whatever populates it is outside this image, so we
 * mirror the indirection rather than guess a buffer size. */
#define g_rmw_buf   (*(uint8_t * volatile *)0x20000000u)

/* ---------------------- trivial copy / compare ------------------------- */
int mem_read(void *dst, const void *src, uint32_t len)
{
    memcpy_auto(dst, src, len);
    return FL_OK;
}

int buf_all_eq(const uint8_t *p, uint32_t len, int val)
{
    const uint8_t *end = p + len;
    while (p != end)
        if (*p++ != (uint8_t)val)
            return 0;
    return 1;
}

/* Returns 0 if every dword in [p,p+len) equals val; otherwise the (nonzero)
 * difference of the first mismatch. NOTE: opposite polarity to buf_all_eq. */
int dwords_all_eq(const void *p, uint32_t val, uint32_t len)
{
    const uint32_t *cur = (const uint32_t *)p;
    const uint32_t *end = (const uint32_t *)((const uint8_t *)p + len);
    int diff = 0;
    while (cur != end) {
        diff = (int)(*cur - val);
        if (diff != 0)
            break;
        cur++;
    }
    return diff;
}

/* ------------------------- program / erase ----------------------------- */
/* Program len bytes at a flash address. Bounds-checked to one 64K block; src
 * must be word-aligned. If the target is not already erased, an RMW erase runs
 * first; then the data is programmed. The controller is brought up before the
 * SPIFI lock is taken, and there is no read-back verification. */
int flash_program(uint8_t *dst, const void *src, uint32_t len)
{
    SPIFI_HANDLE_T *h;
    int blank, rc;

    if (((uint32_t)len + ((uint32_t)(uintptr_t)dst & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;
    if (((uint32_t)(uintptr_t)src & 3u) != 0u)
        return FL_BADARG;

    blank = buf_all_eq(dst, len, 0xFF);        /* target already erased? */

    rc = spifi_init();                          /* init BEFORE taking the lock */
    if (rc)
        return rc;

    spifi_lock_acquire();
    h  = (SPIFI_HANDLE_T *)g_spifi_handle;
    rc = spifiDevSetMemMode(h, false);                          /* IDA: spifi_set_memmode (command) */
    if (rc) {
        spifi_lock_release();
        return rc;
    }
    if (blank || (rc = flash_program_rmw((uint32_t)(uintptr_t)dst, NULL, len)) == 0) {
        rc = spifiProgram(h, (uint32_t)(uintptr_t)dst,
                          (const uint32_t *)src, len);          /* IDA: spifi_program */
        if (!rc) {
            rc = spifiDevSetMemMode(h, true);                   /* restore XIP */
            spifi_lock_release();
            if (!rc)
                return spifi_deinit();
            goto deinit;                                        /* restore-XIP failed */
        }
    }
    /* program failed, or the RMW pre-erase failed: restore XIP and release */
    spifiDevSetMemMode(h, true);
    spifi_lock_release();
deinit:
    spifi_deinit();
    return rc;
}

/* Read-modify-write across erase blocks using the staging buffer. src == NULL
 * means "erase-fill" (used by flash_program to clear a target). The lock is
 * taken after init and the erase-block size is queried; there is no final
 * read-back verification. */
int flash_program_rmw(uint32_t a1, const uint8_t *a2, uint32_t a3)
{
    SPIFI_HANDLE_T *h;
    uint32_t remaining = a3;
    uint32_t cur_addr  = a1;
    const uint8_t *cur_src = a2;
    uint32_t info;
    int rc;

    if (((uint32_t)a3 + (a1 & 0xFFFFu)) > 0x10000u)
        return FL_BADARG;

    rc = spifi_init();
    if (rc)
        return rc;
    h    = (SPIFI_HANDLE_T *)g_spifi_handle;
    info = spifiDevGetInfo(h, SPIFI_INFO_ERASE_BLOCKSIZE);      /* IDA: spifi_get_info(h,3) */
    if (info > 0x10000u) {
        spifi_deinit();
        return FL_BADARG;
    }

    spifi_lock_acquire();
    rc = spifiDevSetMemMode(h, false);                          /* command mode */
    if (rc) {
        spifi_lock_release();
        spifi_deinit();
        return rc;
    }
    rc = spifiDevUnlockDevice(h);                               /* IDA: (*h)[0](h,0,0) clear block-protect */
    if (rc) {
        spifiDevSetMemMode(h, true);
        spifi_lock_release();
        spifi_deinit();
        return rc;
    }

    do {
        uint32_t blk   = spifiGetBlockFromAddr(h, cur_addr);    /* IDA: spifi_get_block_from_addr */
        uint32_t base  = spifiGetAddrFromBlock(h, blk);          /* IDA: spifi_get_addr_from_block */
        uint32_t off   = cur_addr - base;
        uint32_t chunk = info - off;
        if (chunk >= remaining)
            chunk = remaining;

        if (off != 0u || info != chunk) {
            /* partial block: read whole block, patch, erase, reprogram */
            rc = spifiRead(h, base, (uint32_t *)g_rmw_buf, info);   /* IDA: spifi_read */
            if (rc) {
                spifiDevSetMemMode(h, true);
                spifi_lock_release();
                spifi_deinit();
                return rc;
            }
            if (cur_src) {
                memcpy_auto(g_rmw_buf + off, cur_src, (int)chunk);
                cur_src += chunk;
            } else {
                memset_bytes(g_rmw_buf + off, 0xFF, (int)chunk);    /* erase-fill */
            }
            rc = spifiErase(h, blk, 1);                             /* IDA: spifi_erase (via spifi_erase_entry) */
            if (rc) {
                spifiDevSetMemMode(h, true);
                spifi_lock_release();
                spifi_deinit();
                return rc;
            }
            rc = spifiProgram(h, base, (const uint32_t *)g_rmw_buf, info);
            if (rc) {
                spifiDevSetMemMode(h, true);
                spifi_lock_release();
                spifi_deinit();
                return rc;
            }
        } else {
            /* full block: erase, then program directly from the source */
            rc = spifiErase(h, blk, 1);
            if (rc) {
                spifiDevSetMemMode(h, true);
                spifi_lock_release();
                spifi_deinit();
                return rc;
            }
            if (((uint32_t)(uintptr_t)cur_src & 3u) != 0u) {
                /* src misaligned: bail. The image releases the lock here but
                 * does NOT restore XIP or deinit on this branch — preserved. */
                spifi_lock_release();
                return rc;                                          /* rc == 0 */
            }
            rc = spifiProgram(h, base, (const uint32_t *)cur_src, info);
            if (rc) {
                spifiDevSetMemMode(h, true);
                spifi_lock_release();
                spifi_deinit();
                return rc;
            }
            cur_src += info;
        }

        if (remaining < chunk) {                                    /* underflow guard */
            spifiDevSetMemMode(h, true);
            spifi_lock_release();
            spifi_deinit();
            return FL_BADARG;
        }
        cur_addr  += chunk;
        remaining -= chunk;
    } while (remaining);

    rc = spifiDevSetMemMode(h, true);                              /* restore XIP */
    spifi_lock_release();
    if (!rc)
        return spifi_deinit();
    return rc;
}

/* Despite the name, erases the SINGLE 64K block containing addr. The 64K-align
 * guard is the entry stub flash_chip_erase_entry (UXTH addr) that falls into
 * this body. This routine takes NO SPIFI lock and performs NO blank-verify. */
int flash_chip_erase(uint8_t *addr)
{
    SPIFI_HANDLE_T *h;
    uint32_t blk;
    int rc, rc_mm;

    if (((uint32_t)(uintptr_t)addr & 0xFFFFu) != 0u)            /* flash_chip_erase_entry */
        return FL_BADARG;

    rc = spifi_init();
    if (rc)
        return rc;
    h = (SPIFI_HANDLE_T *)g_spifi_handle;

    rc_mm = spifiDevSetMemMode(h, false);                       /* command mode */
    if (rc_mm)
        return rc_mm;

    rc = spifiDevUnlockDevice(h);                               /* IDA: (*h)[0](h,0,0) */
    if (rc == 0) {
        blk = spifiGetBlockFromAddr(h, (uint32_t)(uintptr_t)addr);
        rc  = spifiErase(h, blk, 1);                            /* erase the one 64K block */
        if (rc == 0) {
            rc_mm = spifiDevSetMemMode(h, true);                /* restore XIP */
            if (rc_mm == 0)
                return spifi_deinit();
            return rc_mm;
        }
        /* erase failed: fall through to the restore path below */
    }
    /* unlock failed OR erase failed: restore XIP, then — faithfully from the
     * image — call spifi_init() on this branch instead of spifi_deinit(),
     * leaving the init refcount one higher than a clean teardown would. */
    spifiDevSetMemMode(h, true);
    spifi_init();
    return rc;
}
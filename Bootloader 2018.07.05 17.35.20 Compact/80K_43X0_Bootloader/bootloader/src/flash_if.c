/* flash_if.c — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * The flash-access layer the boot logic sits on: a command-mode read, an
 * equality check, and a thin program/erase pair built on lpcspifilib
 * (IMPORTED). Because the controller is never in XIP mode in this build, the
 * reader issues SPIFI read commands directly (no memcpy from a mapped window),
 * and program/erase need no XIP <-> command mode switching, no lock, and no
 * read-modify-write staging buffer. Names are kept identical to the disassembly.
 *
 * These routines are compiled at -O0 in this image; the C below is the
 * idiomatic form and is behaviourally identical.
 */

#include "bootloader.h"
#include "spifilib_api.h"
#include "chip.h"

extern void *g_spifi_handle;   /* IDA: dword_1001DA94 (defined in spifi_glue.c) */

/* ---------------------- read ------------------------------------------- */
/* Read len bytes from flash address 'src' into 'dst' using the SPIFI driver in
 * command mode. The driver's word path wants an aligned destination, so if dst
 * is not word-aligned the leading bytes (up to the next 4-boundary) are read
 * through a small stack buffer first and copied out, then the aligned remainder
 * is read in one go. 'src' is a 0x14xxxxxx flash byte-address. */
int mem_read(void *dst, const void *src, uint32_t len)
{
    SPIFI_HANDLE_T *h        = (SPIFI_HANDLE_T *)g_spifi_handle;
    uint32_t        src_addr = (uint32_t)(uintptr_t)src;
    uint32_t        head     = 4u - ((uint32_t)(uintptr_t)dst & 3u);
    uint8_t         stage[7];
    uint32_t        n;
    int             rc;

    if ((head & 3u) == 0u)                                     /* dst already aligned */
        return spifiRead(h, src_addr, (uint32_t *)dst, len);   /* IDA: spifi_read */

    n = (len >= head) ? head : len;                            /* head = min(len, pad) */
    rc = spifiRead(h, src_addr, (uint32_t *)stage, n);         /* IDA: spifi_read */
    if (rc != 0)
        return rc;

    src_addr += n;
    {
        uint8_t       *d = (uint8_t *)dst;
        const uint8_t *s = stage;
        uint32_t       i = n;
        while (i-- > 0u) {
            *d++ = *s++;
            --len;
        }
    }
    if (len == 0u)
        return rc;

    return spifiRead(h, src_addr, (uint32_t *)((uint8_t *)dst + n), len);  /* IDA: spifi_read */
}

/* ---------------------- compare ---------------------------------------- */
/* Returns 0 if every dword in [p,p+len) equals val; otherwise the (nonzero)
 * difference of the first mismatch. */
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
/* Program len bytes at flash address dst from src. The source must be
 * word-aligned; that is the only check. No bounds check, no lock, no mode
 * switch, no read-back verification — the controller is already in command
 * mode and the caller owns correctness. */
int flash_program(uint32_t dst, const void *src, uint32_t len)
{
    if (((uint32_t)(uintptr_t)src & 3u) != 0u)
        return FL_BADARG;
    return spifiProgram((SPIFI_HANDLE_T *)g_spifi_handle, dst,
                        (const uint32_t *)src, len);           /* IDA: spifi_program */
}

/* Erase the single block that contains 'addr'. Resolves addr -> block via the
 * driver, then erases one block. No lock and no blank-verify. */
int flash_erase_block(uint32_t addr)
{
    SPIFI_HANDLE_T *h   = (SPIFI_HANDLE_T *)g_spifi_handle;
    uint32_t        blk = spifiGetBlockFromAddr(h, addr);      /* IDA: spifi_get_block_from_addr */
    return spifiErase(h, blk, 1);                              /* IDA: spifi_erase (via spifi_erase_tail) */
}
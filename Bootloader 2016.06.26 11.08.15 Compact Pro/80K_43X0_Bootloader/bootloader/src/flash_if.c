// CompactPro_43X0_Bootloader/bootloader/src/flash_if.c
/* flash_if.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * The flash-access layer the boot pipeline sits on. This image drives a bespoke
 * hand-written SPIFI driver (see spifi_glue.c), IMPORTED, not reimplemented.
 * This file only marshals work into the driver's fixed request struct and calls
 * its three relocated entry points (the thunks). Names are kept identical to
 * the disassembly. This layer is byte-identical to the FF sibling build.
 *
 * Faithful to the machine code (NOT the idealised template):
 *   - flash_program is 4-arg (flash_off, src, len, staged); it does NO internal
 *     memcpy and NO bounds check. The src pointer is vestigial — the request ABI
 *     carries none, so program data is pre-staged to the AHB buffer by the
 *     caller. 'staged' selects stage_buf (0x20000000) vs 0; sentinel is 0.
 *   - flash_program_rmw is just erase-then-program (staged), NOT a block RMW.
 *   - flash_chip_erase erases the single 64 KB block at flash_off.
 *   - No read-back verify on program; no blank-verify on erase.
 *
 * Request-struct ABI (typed struct in spifi_glue.c, instance @0x10011ED0):
 *   +0x00 flash_offset : byte offset of the target from 0x14000000
 *   +0x04 length       : byte count
 *   +0x08 stage_buf    : AHB staging buffer (0x20000000) or 0
 *   +0x0C sentinel     : 0
 *   +0x10 opcode       : 0x08 = program, 0x20 = erase
 * The driver context lives immediately after, at 0x10011EE4.
 */
#include "bootloader.h"

/* SCU pin-config registers for P3_x (SPIFI bus): base 0x40086000, SFSP3 +0x180. */
#define SCU_SFSP3(pin)   (*(volatile uint32_t *)(0x40086000u + 0x180u + ((pin) * 4u)))

/* The loader fills the op-request struct by absolute offset from
 * RAM_REQUEST_STRUCT (0x10011ED0), exactly as the machine code does. */
#define REQ_FLASH_OFFSET  (*(volatile uint32_t *)(RAM_REQUEST_STRUCT + 0x00u))
#define REQ_LENGTH        (*(volatile uint32_t *)(RAM_REQUEST_STRUCT + 0x04u))
#define REQ_STAGE_BUF     (*(volatile uint32_t *)(RAM_REQUEST_STRUCT + 0x08u))
#define REQ_SENTINEL      (*(volatile uint32_t *)(RAM_REQUEST_STRUCT + 0x0Cu))
#define REQ_OPCODE        (*(volatile uint32_t *)(RAM_REQUEST_STRUCT + 0x10u))

/* ------------------------------ bring-up ------------------------------- */
/* spifi_init (flash 0x1400063C). Pin-mux the SPIFI bus, prime the request
 * struct (a default — every op overwrites it), and bring the imported driver
 * up through the init thunk, which is passed the driver context (request+0x14).
 *
 * Faithful oddities preserved from the machine code:
 *   - P3_3 is NOT configured.
 *   - P3_4 is written TWICE: 0xF3, then immediately 0xD3.
 *   - P3_5..P3_7 = 0xD3 (function 3); P3_8 = 0x13. */
int spifi_init(void)
{
    SCU_SFSP3(4) = 0xF3u;        /* first write (oddity)    */
    SCU_SFSP3(4) = 0xD3u;        /* immediately overwritten */
    SCU_SFSP3(5) = 0xD3u;
    SCU_SFSP3(6) = 0xD3u;
    SCU_SFSP3(7) = 0xD3u;
    SCU_SFSP3(8) = 0x13u;

    REQ_FLASH_OFFSET = 0u;
    REQ_LENGTH       = 0u;
    REQ_STAGE_BUF    = 0u;
    REQ_SENTINEL     = 0xFFFFFFFFu;       /* default init; flash ops set 0      */
    REQ_OPCODE       = SPIFI_OP_PROGRAM;  /* default init                       */

    return spifi_drv_init_thunk((void *)RAM_DRIVER_CONTEXT);
}

/* ----------------------------- program --------------------------------- */
/* flash_program (flash 0x1400069C). Marshal one page-program op. Data is taken
 * from the staging buffer (the request ABI has no source field); 'staged'
 * selects stage_buf. No read-back verify. */
int flash_program(uint32_t flash_off, const void *src, uint32_t len, int staged)
{
    (void)src;                                  /* vestigial */
    REQ_FLASH_OFFSET = flash_off - SPIFI_XIP_BASE;
    REQ_LENGTH       = len;
    REQ_STAGE_BUF    = staged ? STAGING_BUF_BASE : 0u;   /* 0x20000000 or 0 */
    REQ_SENTINEL     = 0u;
    REQ_OPCODE       = SPIFI_OP_PROGRAM;        /* 8 */
    return spifi_drv_program_thunk((void *)RAM_DRIVER_CONTEXT);
}

/* ------------------------------- erase --------------------------------- */
/* flash_erase_region (flash 0x140006D4). Erase a range; no blank-verify. The
 * request's stage_buf is left at 0x20000000 here (matches the machine code). */
int flash_erase_region(uint32_t flash_off, uint32_t len)
{
    REQ_FLASH_OFFSET = flash_off - SPIFI_XIP_BASE;
    REQ_LENGTH       = len;
    REQ_STAGE_BUF    = STAGING_BUF_BASE;        /* 0x20000000 */
    REQ_SENTINEL     = 0u;
    REQ_OPCODE       = SPIFI_OP_ERASE;          /* 0x20 */
    return spifi_drv_op_thunk((void *)RAM_DRIVER_CONTEXT);
}

/* ----------------------- read-modify-write ----------------------------- */
/* flash_program_rmw (flash 0x14000704). Despite the name, just erase the range
 * then program it (staged). Used by select_boot_slot to flip the 4-byte config
 * selector / stamp a slot header. No final verify. */
int flash_program_rmw(uint32_t flash_off, const void *src, uint32_t len)
{
    if (!flash_erase_region(flash_off, len))
        return 0;
    return flash_program(flash_off, src, len, 1) ? 1 : 0;   /* staged */
}

/* ------------------------------ chip erase ----------------------------- */
/* flash_chip_erase (flash 0x14000728). Despite the name, erases the SINGLE
 * 64 KB block at flash_off (fixed 0x10000 length); used by the migration sweep.
 * stage_buf is 0 here (unlike flash_erase_region). */
int flash_chip_erase(uint32_t flash_off)
{
    REQ_FLASH_OFFSET = flash_off - SPIFI_XIP_BASE;
    REQ_LENGTH       = 0x10000u;                /* single 64 KB block */
    REQ_STAGE_BUF    = 0u;
    REQ_SENTINEL     = 0u;
    REQ_OPCODE       = SPIFI_OP_ERASE;          /* 0x20 */
    return spifi_drv_op_thunk((void *)RAM_DRIVER_CONTEXT);
}
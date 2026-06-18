// SeekProFF_43X0_Bootloader/bootloader/src/flash_if.c
/* flash_if.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * The flash-access layer the boot pipeline sits on. It drives a bespoke
 * hand-written SPIFI driver (see spifi_glue.c), which is IMPORTED, not
 * reimplemented. This file only marshals work into the driver's fixed request
 * struct and calls its three relocated entry points (the thunks). Names are
 * kept identical to the disassembly.
 *
 * Request-struct ABI (struct in spifi_glue.c, instance @0x10011ED0):
 *   +0x00 flash_offset : target byte offset from 0x14000000
 *   +0x04 length       : byte count
 *   +0x08 stage_buf    : AHB staging buffer (0x20000000) for program data, or 0
 *   +0x0C sentinel     : 0
 *   +0x10 opcode       : 0x08 = program, 0x20 = erase
 * The driver context lives immediately after, at 0x10011EE4.
 *
 * The request ABI carries NO source pointer: all program data is taken from the
 * AHB staging buffer at 0x20000000, which callers populate first (the migration
 * sweep and select_boot_slot both pre-stage there). Reads are plain XIP loads.
 * This build does NO read-back verify on program and NO blank-verify on erase.
 */

#include "bootloader.h"

/* SCU pin-config registers for P3_x (SPIFI bus). Base 0x40086000, the SFSP3
 * group at +0x180, one 32-bit reg per pin (so P3_4 == 0x40086190). */
#define SCU_SFSP3(pin)   (*(volatile uint32_t *)(0x40086000u + 0x180u + ((pin) * 4u)))

/* request struct + driver context provided by spifi_glue.c */
extern volatile spifi_request_t *const g_spifi_request;
extern void                     *const g_spifi_driver_ctx;

/* ------------------------------ bring-up ------------------------------- *
 * spifi_init (flash 0x14000588). Order, faithful to the machine code:
 * initialise the request struct FIRST, then pin-mux, then bring the driver up.
 * ----------------------------------------------------------------------- */
int spifi_init(void)
{
    /* 1) prime the request struct (placeholder values; the init thunk ignores
     *    the offset/length but the driver expects the block written). */
    g_spifi_request->flash_offset = 0u;            /* ED0 = 0            */
    g_spifi_request->length       = 0u;            /* ED4 = 0            */
    g_spifi_request->stage_buf    = 0u;            /* ED8 = 0            */
    g_spifi_request->sentinel     = 0xFFFFFFFFu;   /* EDC = 0xFFFFFFFF   */
    g_spifi_request->opcode       = SPIFI_OP_PROGRAM; /* EE0 = 8         */

    /* 2) pin-mux the SPIFI bus. Faithful oddities preserved from the machine
     *    code: P3_3 is NOT configured, and P3_4 is written twice (0xF3 then
     *    immediately 0xD3). P3_5..P3_7 follow the standard SPIFI mux; P3_8 is
     *    the clock pin. */
    SCU_SFSP3(4) = 0xF3u;        /* first write (oddity)   */
    SCU_SFSP3(4) = 0xD3u;        /* immediately overwritten */
    SCU_SFSP3(5) = 0xD3u;
    SCU_SFSP3(6) = 0xD3u;
    SCU_SFSP3(7) = 0xD3u;
    SCU_SFSP3(8) = 0x13u;

    /* 3) bring the imported driver up (probe JEDEC ID, select the vendor
     *    command set, configure read/quad modes, enter XIP). The driver init
     *    additionally takes three constant mode words (3, 0xC0, 0xC) passed in
     *    registers through the passthrough thunk. The "context" handed to init
     *    is the request-struct base. */
    return spifi_drv_init_thunk((void *)g_spifi_request, 3u, 0xC0u, 0xCu);
}

/* ----------------------------- program --------------------------------- *
 * flash_program (flash 0x140005DC). Marshals one page-program op. The data is
 * taken from the AHB staging buffer (the request ABI has no source field), so
 * the 'src' argument is vestigial. 'staged' selects stage_buf (0x20000000) vs 0.
 * No bounds check, no read-back verify, sentinel = 0.
 * ----------------------------------------------------------------------- */
int flash_program(uint32_t flash_off, const void *src, uint32_t len, int staged)
{
    (void)src;   /* not used: program data is pre-staged in the AHB buffer */

    g_spifi_request->stage_buf    = staged ? STAGING_BUF_BASE : 0u;   /* ED8 */
    g_spifi_request->flash_offset = flash_off - SPIFI_XIP_BASE;        /* ED0 */
    g_spifi_request->length       = len;                              /* ED4 */
    g_spifi_request->sentinel     = 0u;                               /* EDC */
    g_spifi_request->opcode       = SPIFI_OP_PROGRAM;                 /* EE0 = 8 */

    return spifi_drv_program_thunk(g_spifi_driver_ctx,
                                   (spifi_request_t *)g_spifi_request);
}

/* ------------------------------- erase --------------------------------- *
 * flash_erase_region (flash 0x14000614). Erase a range; no blank-verify.
 * stage_buf is set to 0x20000000 (vestigial for erase), sentinel = 0.
 * ----------------------------------------------------------------------- */
int flash_erase_region(uint32_t flash_off, uint32_t len)
{
    g_spifi_request->stage_buf    = STAGING_BUF_BASE;   /* ED8 = 0x20000000 */
    g_spifi_request->flash_offset = flash_off - SPIFI_XIP_BASE;   /* ED0 */
    g_spifi_request->length       = len;                /* ED4 */
    g_spifi_request->sentinel     = 0u;                 /* EDC */
    g_spifi_request->opcode       = SPIFI_OP_ERASE;     /* EE0 = 0x20 */

    return spifi_drv_op_thunk(g_spifi_driver_ctx,
                              (spifi_request_t *)g_spifi_request);
}

/* Despite the name, erases the SINGLE 64K block at flash_off (fixed length
 * 0x10000), not the whole device; used by the A->B migration sweep. No
 * blank-verify. stage_buf = 0, sentinel = 0. */
int flash_chip_erase(uint32_t flash_off)
{
    g_spifi_request->length       = 0x10000u;           /* ED4 = 64K */
    g_spifi_request->flash_offset = flash_off - SPIFI_XIP_BASE;   /* ED0 */
    g_spifi_request->stage_buf    = 0u;                 /* ED8 = 0 */
    g_spifi_request->sentinel     = 0u;                 /* EDC */
    g_spifi_request->opcode       = SPIFI_OP_ERASE;     /* EE0 = 0x20 */

    return spifi_drv_op_thunk(g_spifi_driver_ctx,
                              (spifi_request_t *)g_spifi_request);
}

/* ----------------------- erase-then-program ---------------------------- *
 * flash_program_rmw (flash 0x14000704). Despite the name, NOT a block
 * read-modify-write: it erases the range then programs it (staged). The caller
 * pre-stages the data in the AHB buffer; the driver reads it from there.
 * Returns 1 on success, 0 on failure.
 * ----------------------------------------------------------------------- */
int flash_program_rmw(uint32_t flash_off, const void *src, uint32_t len)
{
    if (!flash_erase_region(flash_off, len))
        return 0;
    return flash_program(flash_off, src, len, 1) ? 1 : 0;   /* staged = 1 */
}
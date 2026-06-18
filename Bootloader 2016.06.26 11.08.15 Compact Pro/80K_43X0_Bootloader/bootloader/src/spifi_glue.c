// CompactPro_43X0_Bootloader/bootloader/src/spifi_glue.c
/* spifi_glue.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Interface to the IMPORTED bespoke SPIFI driver. The driver is a single
 * position-relocated code+data blob (NOT NXP lpcspifilib); this file does not
 * reimplement it — it describes its request-struct ABI, reserves its RAM
 * context, and provides the three flash-resident thunk stubs that branch into
 * it. The driver bytes are imported.
 *
 * RELOCATION
 *   The driver blob is copied at boot from flash to SRAM by Reset_Handler's
 *   scatter-load (the .text_ram triplet):
 *       flash 0x14000BE8  ->  RAM 0x10010000 ,  length 0x1D70
 *   so for any RAM address R inside the relocated driver:
 *       flash = R - 0x10010000 + 0x14000BE8
 *   The boot logic stays in flash (XIP) and calls into the RAM copy through the
 *   three thunks below, because driving the controller in command mode (for
 *   program/erase) drops XIP and the driver cannot be fetched from flash then.
 *
 *   One driver helper is the reverse: memcpy_bytes_thunk (flash 0x14002948)
 *   relocates into RAM with the blob but BRANCHES BACK to the flash-resident
 *   memcpy_bytes (0x14000844, util_mem.c) — a RAM->flash cross-boundary call.
 *   It is part of the driver blob and is not reproduced here.
 *
 * The RAM driver entry points (0x100105FF / 0x10010E95 / 0x100110EB) are
 * IDENTICAL to the FF sibling build — only the flash-side addresses (the blob
 * LMA and the three thunk stubs at 0x14000BB8/0xBC8/0xBD8) shift by +0x1C0.
 */
#include "bootloader.h"

/* ABI struct: the driver reads this to perform one program/erase op. The loader
 * fills it by offset (see flash_if.c); this typedef documents the layout and
 * reserves the storage. */
typedef struct {
    uint32_t flash_offset;   /* +0x00 byte offset from 0x14000000             */
    uint32_t length;         /* +0x04 byte count                              */
    uint32_t stage_buf;      /* +0x08 AHB staging buffer (0x20000000), or 0   */
    uint32_t sentinel;       /* +0x0C 0                                       */
    uint32_t opcode;         /* +0x10 SPIFI_OP_PROGRAM (8) / SPIFI_OP_ERASE   */
} spifi_request_t;

/* The loader's BSS marshalling area (zeroed by CRT0). Declaration order places
 * the request struct first (0x10011ED0) and the driver context immediately
 * after (0x10011EE4). */
spifi_request_t g_spifi_request_obj  __attribute__((section(".spifi_driver_bss"), used));
uint8_t         g_spifi_driver_ctx[0x80] __attribute__((section(".spifi_driver_bss"), used));

/* Relocated driver entry points (RAM, Thumb). Offsets from 0x10010000:
 *   init    @ 0x100105FF : controller reset, JEDEC detect, vendor command-set
 *                          selection, read/quad mode config, enter XIP
 *   program @ 0x10010E95 : page-program loop per request
 *   op      @ 0x100110EB : erase per request                                  */
typedef int (*drv_fn)(void *ctx);
#define DRV_INIT_ENTRY     0x100105FFu
#define DRV_PROGRAM_ENTRY  0x10010E95u
#define DRV_OP_ENTRY       0x100110EBu

/* Flash-resident thunks (0x14000BB8 / 0x14000BC8 / 0x14000BD8). Each is a tiny
 * stub that branches into the relocated driver, preserving the single context
 * argument. They stay in flash so the boot logic — itself running from flash —
 * can reach the RAM driver across the XIP boundary. */
int spifi_drv_init_thunk(void *ctx)
{
    return ((drv_fn)DRV_INIT_ENTRY)(ctx);
}
int spifi_drv_program_thunk(void *ctx)
{
    return ((drv_fn)DRV_PROGRAM_ENTRY)(ctx);
}
int spifi_drv_op_thunk(void *ctx)
{
    return ((drv_fn)DRV_OP_ENTRY)(ctx);
}
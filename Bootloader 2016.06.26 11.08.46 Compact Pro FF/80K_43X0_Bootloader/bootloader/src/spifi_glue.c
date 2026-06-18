// SeekProFF_43X0_Bootloader/bootloader/src/spifi_glue.c
/* spifi_glue.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * Interface to the IMPORTED bespoke SPIFI driver. The driver is a single
 * position-relocated code+data blob (it is NOT NXP lpcspifilib); this file does
 * not reimplement it — it only describes the request-struct ABI, reserves the
 * driver's RAM objects, and provides the three flash-resident thunk stubs that
 * branch into it.
 *
 * RELOCATION
 *   The driver blob is copied at boot from flash to SRAM by Reset_Handler's
 *   scatter-load (the .text_ram triplet):
 *       flash 0x14000A28  ->  RAM 0x10010000 ,  length 0x1D70
 *   so for any RAM address R inside the relocated driver:
 *       flash = R - 0x10010000 + 0x14000A28
 *   The boot logic stays in flash (XIP) and calls into the RAM copy through the
 *   three thunks below, because driving the controller in command mode (for
 *   program/erase) drops XIP and the driver cannot be fetched from flash then.
 *
 *   One driver helper goes the other way: memcpy_bytes_thunk (in the blob,
 *   flash ~0x14002788) relocates into RAM with the driver but BRANCHES BACK to
 *   the flash-resident memcpy_bytes (util_mem.c) — a RAM->flash cross-boundary
 *   call. It is part of the blob and is not reproduced here.
 *
 * The thunk signatures below refine the simplified forward declarations in
 * bootloader.h: each thunk is a register-passthrough stub, so its argument list
 * simply mirrors the call site.
 */

#include "bootloader.h"

/* ABI struct (mirrors the field map documented in flash_if.c). The driver reads
 * this to perform one program/erase operation. */
typedef struct {
    uint32_t flash_offset;   /* +0x00 byte offset from 0x14000000             */
    uint32_t length;         /* +0x04 byte count                              */
    uint32_t stage_buf;      /* +0x08 AHB staging buffer (0x20000000), or 0   */
    uint32_t sentinel;       /* +0x0C 0                                       */
    uint32_t opcode;         /* +0x10 SPIFI_OP_PROGRAM (8) / SPIFI_OP_ERASE   */
} spifi_request_t;

/* The boot loader's BSS (0x94 bytes at 0x10011ED0), zeroed by CRT0. The request
 * struct is first (0x10011ED0); the driver context immediately follows
 * (0x10011EE4 == request + 0x14). */
volatile spifi_request_t        g_spifi_request_obj;       /* 0x10011ED0 */
uint8_t                          g_spifi_driver_ctx_obj[0x80]; /* 0x10011EE4 */

volatile spifi_request_t *const  g_spifi_request    = &g_spifi_request_obj;
void                     *const  g_spifi_driver_ctx = (void *)g_spifi_driver_ctx_obj;

/* Relocated driver entry points (RAM, Thumb). Offsets from 0x10010000:
 *   init    @ 0x100105FF : controller reset, JEDEC detect, vendor command-set
 *                          selection, read/quad mode config, enter XIP
 *   program @ 0x10010E95 : page-program loop per request
 *   op      @ 0x100110EB : erase per request                                  */
#define DRV_INIT_ENTRY     0x100105FFu
#define DRV_PROGRAM_ENTRY  0x10010E95u
#define DRV_OP_ENTRY       0x100110EBu

typedef int (*drv_init_fn)   (void *cfg, uint32_t, uint32_t, uint32_t);
typedef int (*drv_program_fn)(void *ctx, spifi_request_t *req);
typedef int (*drv_op_fn)     (void *ctx, spifi_request_t *req);

/* Flash-resident thunks (0x140009F8 / 0x14000A08 / 0x14000A18). Each is a tiny
 * stub that branches into the relocated driver; they stay in flash so the boot
 * logic — itself running from flash — can reach the RAM driver across the XIP
 * boundary. */
int spifi_drv_init_thunk(void *cfg, uint32_t mode0, uint32_t mode1, uint32_t mode2)
{
    return ((drv_init_fn)DRV_INIT_ENTRY)(cfg, mode0, mode1, mode2);
}
int spifi_drv_program_thunk(void *ctx, spifi_request_t *req)
{
    return ((drv_program_fn)DRV_PROGRAM_ENTRY)(ctx, req);
}
int spifi_drv_op_thunk(void *ctx, spifi_request_t *req)
{
    return ((drv_op_fn)DRV_OP_ENTRY)(ctx, req);
}
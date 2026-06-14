/* spifi_glue.c — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * The single bootloader wrapper over NXP lpcspifilib: bring the controller up,
 * register the device family, probe the JEDEC ID, build a device handle, apply
 * the quad option, and return the handle. The lpcspifilib functions are
 * IMPORTED (see lpcspifilib/), not reimplemented; each call is tagged with the
 * disassembly label it matched. The wrapper name is kept identical to the
 * disassembly.
 *
 * This build differs from the Compact Pro FF glue in three ways that matter:
 *   - it is NOT reference-counted (one shot; returns the handle or NULL),
 *   - it does NOT enter memory-mapped (XIP) mode — there is no final
 *     spifiDevSetMemMode(h, true), so the controller stays in command mode,
 *   - there is no deinit and no SPIFI lock.
 */

 #include "bootloader.h"
#include "spifilib_api.h"   /* SPIFI_HANDLE_T, spifiInit, spifiInitDevice, ... */
#include "chip.h"           /* LPC_SPIFI_BASE */

/* Shared device handle (IDA: dword_1001DA94). flash_if.c also reads it.
 * Could equally be declared in bootloader.h; kept here to keep that header free
 * of lpcspifilib types. */
void *g_spifi_handle = NULL;

/* Backing store for the device handle/context (IDA: buffer @0x1001DA40, the
 * first object in .bss). The driver requires a context <=0x53 bytes; 0x54
 * leaves room. */
static uint8_t s_handle_mem[0x54] __attribute__((aligned(4)));

/* ----------------------------------------------------------------------- */
void *spifi_init(void)
{
    SPIFI_HANDLE_T *h;
    uint32_t        ctx_size;

    if (spifiInit(LPC_SPIFI_BASE, true) != 0)                  /* IDA: spifi_reset_controller */
        return NULL;

    /* Register the device-family database. The image issues the registration
     * three times; the registry keeps a one-shot guard, so the second and
     * third calls are no-ops. */
    spifiRegisterFamily(SPIFI_REG_FAMILY_CommonCommandSet);    /* IDA: spifi_register_family(spifi_register_all_families) */
    spifiRegisterFamily(SPIFI_REG_FAMILY_CommonCommandSet);
    spifiRegisterFamily(SPIFI_REG_FAMILY_CommonCommandSet);

    /* spifiGetHandleMemSize (IDA: spifi_get_device_size) returns the RAM context
     * size, NOT a flash size; require 1..0x53. */
    ctx_size = spifiGetHandleMemSize(LPC_SPIFI_BASE);          /* IDA: spifi_get_device_size */
    if (ctx_size == 0u || ctx_size > 0x53u)
        return NULL;

    h = spifiInitDevice(s_handle_mem, sizeof s_handle_mem,
                        LPC_SPIFI_BASE, SPIFI_XIP_BASE);        /* IDA: spifi_init_device */
    if (h == NULL)
        return NULL;

    if (spifiDevSetOpts(h, 0x0Cu, true) != 0)                  /* IDA: spifi_set_options (opts=0xC = quad I/O bits) */
        return NULL;

    /* Deliberately NO spifiDevSetMemMode(h, true) here: the controller is left
     * in command mode and the loader reads flash with explicit commands. */
    g_spifi_handle = h;
    return h;
}
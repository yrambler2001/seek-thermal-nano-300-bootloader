/* keys.c — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * Embedded key material + boot-config base. The boot-config base and the two
 * 128-bit keys live in .data (LMA in flash @0x140019DC; CRT0 copies them to RAM
 * @0x1001D600, so prng_seed_* read the RAM copies). The optional per-unit key
 * slot stays in flash and is read directly at its fixed address (0x14000218).
 *
 * NOTE: these are obfuscation keys baked into the image — recovered constants,
 * not secrets. xorshift128 + a 16-bit checksum is not authenticated encryption.
 */

#include "bootloader.h"

/* .data: boot-config base, later handed to the app
 * (IDA: g_boot_config_base_flash_word @0x140019DC -> RAM @0x1001D600). */
uint32_t g_boot_config_base = 0x14010000u;

/* .data: embedded Key A   (IDA: g_keyA_flash @0x140019E0 -> RAM g_keyA @0x1001D604).
 * xorshift seed [x,y,z,w] = 4D4F4B4B B825505C AE252CD1 0D93AA99. */
uint8_t g_keyA[16] = {
    0x46, 0x31, 0xC4, 0x1E, 0x94, 0xD0, 0x18, 0x5E,
    0x83, 0xCB, 0x72, 0xAB, 0x0E, 0xB7, 0x72, 0xBD
};

/* .data: embedded Key B fallback (IDA: g_keyB_flash @0x140019F0 -> RAM g_keyB @0x1001D614).
 * xorshift seed [x,y,z,w] = 2B93484D AB449F21 6F7EA7FA DAFB753A. */
uint8_t g_keyB[16] = {
    0xE5, 0xEE, 0xAC, 0xC9, 0x92, 0xD3, 0xC4, 0x38,
    0xFE, 0x04, 0x13, 0xB8, 0x25, 0x3C, 0x29, 0x7C
};

/* Flash @0x14000218: optional per-unit key override; all-0xFF (erased) in this
 * image. Pinned by the linker via the .dev_key section; prng_seed_keyB_or_device
 * reads it through DEVICE_KEY_SLOT_ADDR. A slot that is all-00 or all-FF is
 * treated as blank -> Key B is used. */
const uint8_t g_device_key_slot_flash[16]
    __attribute__((section(".dev_key"), used)) = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
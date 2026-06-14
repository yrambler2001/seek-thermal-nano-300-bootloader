/* keys.c — 80K_43X0_Bootloader (reconstructed)
 *
 * Embedded key material + boot-config base. The boot-config base and the two
 * 128-bit keys live in .data (LMA in flash @0x14001C28; CRT0 copies them to RAM
 * @0x1001D84C, so prng_seed_* read the RAM copies). The optional per-unit key
 * slot stays in flash and is read directly at its fixed address (0x14000218).
 *
 * NOTE: these are obfuscation keys baked into the image — recovered constants,
 * not secrets. xorshift128 + a 16-bit checksum is not authenticated encryption.
 */

 #include "bootloader.h"

/* .data: boot-config base, later handed to the app
 * (IDA: g_boot_config_base_flash_word -> RAM g_boot_config_base_ram @0x1001D84C). */
uint32_t g_boot_config_base = 0x14010000u;

/* .data: embedded Key A   (IDA: g_keyA_flash @0x14001C2C -> RAM g_keyA @0x1001D850). */
uint8_t g_keyA[16] = {
    0xF3, 0x2A, 0xD7, 0x71, 0x9D, 0xDD, 0x4A, 0xDA,
    0xBA, 0x1D, 0xED, 0x88, 0x06, 0x52, 0x85, 0xB3
};

/* .data: embedded Key B fallback (IDA: g_keyB_flash @0x14001C3C -> RAM g_keyB @0x1001D860). */
uint8_t g_keyB[16] = {
    0x99, 0x7E, 0xD6, 0xE5, 0xE1, 0xD8, 0x8D, 0x87,
    0x5F, 0x83, 0xA2, 0xA2, 0x3C, 0x93, 0x09, 0x03
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
// SeekProFF_43X0_Bootloader/bootloader/src/keys.c
/* keys.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * Embedded key material, the boot-config base pointer, and the build-info block.
 *
 * This image carries TWO keys plus a dual-purpose mask block:
 *   - g_key_mask : 16 bytes that serve BOTH as Key A (the transport/OTA key,
 *                  seeded directly into the xorshift state) AND as the whitening
 *                  mask XORed into the silicon device-ID when Key B is derived
 *                  from hardware.
 *   - g_keyB     : the fixed embedded Key B (the at-rest device key), used when
 *                  the build marker selects the fixed-key path (it does here).
 * The config base pointer and both keys live in the tail of .data (LMA in flash,
 * copied to RAM by CRT0). The first ~0x13C bytes of .data belong to the imported
 * driver's mutable tables and are part of the driver blob, not this file.
 *
 * NOTE: these are recovered constants, not secrets — they are embedded in clear
 * in the flash image. A 128-bit-keyed xorshift128 plus a 32-bit additive
 * checksum is obfuscation, not authenticated encryption.
 *
 * Build identity: marker {01,02,00,00} (composite 0x01020000), built
 * "Jun 26 2016" "11:08:46". There is NO internal build-name string in this
 * image; the workspace name is descriptive (product code LQ-XXX).
 */
#include "bootloader.h"

/* .data: boot-config base, later handed to the app via the handoff block.
 * (RAM 0x10011EAC <- flash 0x140028F8; value 0x14010000.) */
const uint32_t g_boot_config_ptr
    __attribute__((section(".boot_config_ptr"), used)) = BOOT_CONFIG_BASE;  /* 0x14010000 */

/* .data: Key A == the mask block. Seeded directly into the xorshift state for
 * the transport/OTA form, and XORed with the silicon device-ID for Key-B
 * derivation. (RAM 0x10011EB0 <- flash 0x140028FC, .data+0x140.)
 * Seed dwords (LE): k0=0xE5ABD658, k1=0xE64D4EA9, k2=0x843AAE50, k3=0x81F2F8F9
 *   -> state [k1,k2,k3,k0] = {0xE64D4EA9, 0x843AAE50, 0x81F2F8F9, 0xE5ABD658}. */
const uint8_t g_key_mask[16]
    __attribute__((section(".keys"), used)) = {
    0x58, 0xD6, 0xAB, 0xE5, 0xA9, 0x4E, 0x4D, 0xE6,
    0x50, 0xAE, 0x3A, 0x84, 0xF9, 0xF8, 0xF2, 0x81
};

/* Key A is the same bytes as the mask block (validation key A == g_key_mask). */
extern const uint8_t g_keyA[16] __attribute__((alias("g_key_mask")));

/* .data: fixed Key B (the at-rest device key). Active because the build marker
 * (0x01020000) exceeds the fixed-key threshold (0x01010000).
 * (RAM 0x10011EC0 <- flash 0x1400290C, .data+0x150.)
 * Seed dwords (LE): k0=0x335AA06D, k1=0x001040F5, k2=0x94C8E234, k3=0x8C70057D
 *   -> state [k1,k2,k3,k0] = {0x001040F5, 0x94C8E234, 0x8C70057D, 0x335AA06D}. */
const uint8_t g_keyB[16]
    __attribute__((section(".keys"), used)) = {
    0x6D, 0xA0, 0x5A, 0x33, 0xF5, 0x40, 0x10, 0x00,
    0x34, 0xE2, 0xC8, 0x94, 0x7D, 0x05, 0x70, 0x8C
};

/* Flash-resident build-info block @0x14002798 (NOT copied to RAM). Its address
 * is published to the app at handoff, and its first 4 bytes are the marker that
 * prng_seed_from_key reads to choose the fixed-vs-silicon Key-B path. Layout
 * mirrors the compiler's __DATE__/__TIME__ block: marker, date string, time. */
const uint8_t g_build_info[0x24]
    __attribute__((section(".build_info"), used)) = {
    /* +0x00 */ 0x01, 0x02, 0x00, 0x00,
    /* +0x04 */ 'J','u','n',' ','2','6',' ','2','0','1','6', 0, 0, 0, 0, 0,
    /* +0x14 */ '1','1',':','0','8',':','4','6', 0, 0, 0, 0, 0, 0, 0, 0
};
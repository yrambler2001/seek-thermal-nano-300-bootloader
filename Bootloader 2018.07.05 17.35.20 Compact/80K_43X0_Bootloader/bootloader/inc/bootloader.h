/* bootloader.h — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * Names are kept identical to the disassembly. Where a name does not match
 * its behaviour, the prototype carries a comment.
 *
 * Build identity baked into the image: a 4-byte marker (0x03020002) plus the
 * build date/time "Jul  5 2018 17:35:20". This build embeds NO ASCII build-name
 * string (the build-info block at flash 0x140019B8 is marker + timestamp only).
 * The application this loader boots is internally named "32K_43X0_COMPACT-8HZ".
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===================================================================== */
/* SPIFI / flash memory map                                              */
/* ===================================================================== */
/* The SPI-NOR is mapped into the address space at 0x14000000 and is used
 * here in COMMAND mode (this build never enters memory-mapped/XIP mode);
 * 0x14xxxxxx values are flash byte-addresses passed to the command-mode
 * reader (see mem_read in flash_if.c). */
#define SPIFI_XIP_BASE        0x14000000u   /* external SPI-NOR address window */
#define BOOTLOADER_BASE       0x14000000u   /* this image (64 KB)           */
#define BOOT_CONFIG_BASE      0x14010000u   /* 16-byte boot-config record   */

#define SLOT_A_BASE           0x14030000u   /* firmware slot A              */
#define SLOT_B_BASE           0x14050000u   /* firmware slot B              */
#define SLOT_RECOVERY_BASE    0x14070000u   /* golden / recovery slot       */
#define SLOT_STRIDE           0x00020000u   /* 128 KB between slots          */

/* Optional per-unit key override; all-0xFF (blank) in this dump.          */
#define DEVICE_KEY_SLOT_ADDR  0x14000218u   /* 16 bytes, read from flash     */

/* Build-info block handed to the app (marker + date/time, no name string). */
#define BUILD_INFO_ADDR       0x140019B8u

/* ===================================================================== */
/* RAM layout that Reset_Handler establishes                              */
/* ===================================================================== */
#define RAM_APP_LOAD_BASE     0x10000000u   /* decrypted image lands here    */
#define RAM_HANDOFF_INFO      0x10000200u   /* segmented-path info block     */
#define RAM_CODE_BASE         0x1001C000u   /* RAM copy of boot code         */
#define RAM_DATA_BASE         0x1001D600u   /* RAM copy of keys/config       */
#define MSP_TOP               0x10020000u   /* bootloader initial MSP        */
#define STACK_GUARD_ADDR      0x1001F800u   /* canary                        */
#define STACK_GUARD_VALUE     0xCDCDCDCDu

/* The decryptor fills 0x10000000..0x1001C000 (112 KB) with the application;
 * the bootloader keeps its own runtime in the 16 KB above (RAM_CODE_BASE).  */
#define RAM_APP_REGION_TOP    0x1001C000u

/* ===================================================================== */
/* Image format                                                           */
/* ===================================================================== */
#define IMG_HEADER_OFFSET     0x200u        /* cleartext header @ slot+0x200 */
#define IMG_MAGIC             0xA1B2C3D4u    /* header word[0]                */
#define IMG_FOOTER_TAG        0x45444F43u    /* "CODE" (footer word[0])       */
#define IMG_MONOLITHIC_MAX    0x1C000u       /* monolithic path if len < this */
#define IMG_MAX_LEN           0x10000u       /* image_try_keys() length cap   */
#define SEG_TABLE_BYTES       0x2C0u         /* segment-table size (segmented)*/

/* The cipher copies words 128..143 (bytes 0x200..0x23F) verbatim.         */
#define IMG_HDR_WORD_FIRST    128u           /* 0x200 / 4                     */
#define IMG_HDR_WORD_LAST     143u           /* inclusive                     */
#define SEG_SKIP_WORDS        0x80u          /* skip_words arg for segments   */

/* xorshift128 key-whitening constant                                       */
#define PRNG_WHITEN_MASK      0x13579BDFu

/* image_try_keys() return codes                                            */
#define KEYID_NONE            0
#define KEYID_B_OR_DEVICE     1
#define KEYID_A               2

/* Flash-layer status codes (as returned by the disassembly)                */
#define FL_OK                 0
#define FL_BADARG             11             /* 0xB: misaligned source      */
#define FL_NOTINIT            2

/* ===================================================================== */
/* Prototypes — names verbatim from the disassembly                       */
/* ===================================================================== */

/* --- startup_lpc43xx.c --- */
void      Reset_Handler(void);

/* --- boot_main.c --- */
void      boot_main(void) __attribute__((noreturn));
uint32_t  select_boot_slot(int update_flag);          /* returns slot base addr */
int       image_try_keys(const uint32_t *slot_base);  /* 0 / 1(KeyB) / 2(KeyA)  */
void      memzero_words(void *dst, uint32_t byte_len);

/* --- crypto_stream.c --- */
uint32_t  xorshift128_next(uint32_t *state);
uint32_t *prng_seed_from_key(uint32_t *state, const uint32_t *key);
uint32_t *prng_seed_keyA(uint32_t *state);
uint32_t *prng_seed_keyB_or_device(uint32_t *state);
int       stream_decrypt_skip_header(void *dst, const uint32_t *src,
                                     uint32_t byte_len, uint32_t *state);
int       stream_checksum16(const uint32_t *src, uint32_t byte_len, uint32_t *state);
int       stream_decrypt_segment(void *dst, uint32_t image_base, uint32_t image_offset,
                                 uint32_t byte_len, uint32_t skip_words, uint32_t *state);

/* --- spifi_glue.c (bootloader wrapper around lpcspifilib) --- */
/* spifi_init: reset controller + register family + probe + init device + set
 * options, then return the device handle (or NULL). NOT reference-counted, and
 * deliberately does NOT enter memory-mapped (XIP) mode. There is no deinit and
 * no SPIFI lock in this build. */
void     *spifi_init(void);

/* --- flash_if.c --- */
int       mem_read(void *dst, const void *src, uint32_t len);  /* SPIFI command-mode read */
int       flash_program(uint32_t dst, const void *src, uint32_t len);
int       flash_erase_block(uint32_t addr);   /* erase the block containing addr */
/* dwords_all_eq: returns 0 if every dword equals val, nonzero otherwise.   */
int       dwords_all_eq(const void *p, uint32_t val, uint32_t len);

/* --- board_timer.c --- */
int       board_gpio_init_timer_delay(void);            /* SCU/GPIO bring-up + ~200ms delay */
int       stash_status_config_values(int result, int cfg);
int       timer0_read_tc(void);
int       timer_ms_to_ticks(int ms);

/* --- keys.c (placed at fixed addresses by the linker; see ld script) --- */
extern uint32_t      g_boot_config_base;            /* .data: value 0x14010000           */
extern uint8_t       g_keyA[16];                    /* .data: RAM copy of Key A           */
extern uint8_t       g_keyB[16];                    /* .data: RAM copy of Key B           */
extern const uint8_t g_device_key_slot_flash[16];   /* flash @0x14000218 (read at runtime)*/

#endif /* BOOTLOADER_H */
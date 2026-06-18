// CompactPro_43X0_Bootloader/bootloader/src/boot_main.c
/* boot_main.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Flash-resident boot leaves: slot selection, the two single-key acceptance
 * gates, the checksum-acceptance tails, and the word-zeroing helper. The
 * top-level pipeline that drives these lives in startup_lpc43xx.c's
 * Reset_Handler (this image runs the whole boot from flash; there is no
 * separate RAM stage). Names are kept identical to the disassembly.
 *
 * The ONE structural difference from the sibling FF build: validation here is
 * split into a magic+len gate (image_try_keys[_copy2]) that tail-calls a
 * separate checksum routine (image_checksum_ok[_copy2]); the FF build fuses the
 * checksum test inline. image_try_keys / image_checksum_ok use Key A (the
 * transport/OTA form, used by the boot-time re-encryption migration);
 * image_try_keys_copy2 / image_checksum_ok_copy2 use Key B (the at-rest device
 * form, used to choose a bootable slot). Each gate returns 1 (accept) / 0.
 */
#include "bootloader.h"

/* ===========================================================================
 * memzero_words  (flash 0x140005A8)
 * Word-zero a region; loops while byte_len > bytes-written-so-far.
 * ========================================================================= */
void memzero_words(void *dst, uint32_t byte_len)
{
    uint8_t *p = (uint8_t *)dst;
    while (byte_len > (uint32_t)(p - (uint8_t *)dst)) {
        *(uint32_t *)p = 0u;
        p += 4;
    }
}

/* ===========================================================================
 * image_try_keys  (flash 0x140005C0, Key A)
 * Read the cleartext header at slot+0x200; require magic + length < 0x10000,
 * then tail into the Key-A checksum gate. Used only by the migration sweep.
 * ========================================================================= */
int image_try_keys(const uint32_t *slot_base)
{
    if (slot_base == NULL)
        return 0;
    if (slot_base[128] != IMG_MAGIC)        /* header[0] @ slot+0x200 */
        return 0;
    if (slot_base[129] >= IMG_MAX_LEN)       /* header[1] @ slot+0x204 */
        return 0;
    return image_checksum_ok(slot_base, slot_base[129]);
}

/* ===========================================================================
 * image_checksum_ok  (flash 0x140005DA, Key A)
 * Run the Key-A keystream checksum; accept iff the decrypted word-sum == 0.
 * (The raw-ciphertext sum is computed by stream_checksum16 but ignored here.)
 * ========================================================================= */
int image_checksum_ok(const uint32_t *src, uint32_t byte_len)
{
    uint32_t sum_raw, sum_dec;
    stream_checksum16(src, byte_len, &sum_raw, &sum_dec);     /* Key A */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}

/* ===========================================================================
 * image_try_keys_copy2  (flash 0x140005FC, Key B)
 * Same header check, Key-B checksum. This is the gate slot selection uses.
 * ========================================================================= */
int image_try_keys_copy2(const uint32_t *slot_base)
{
    if (slot_base == NULL)
        return 0;
    if (slot_base[128] != IMG_MAGIC)
        return 0;
    if (slot_base[129] >= IMG_MAX_LEN)
        return 0;
    return image_checksum_ok_copy2(slot_base, slot_base[129]);
}

/* ===========================================================================
 * image_checksum_ok_copy2  (flash 0x14000616, Key B)
 * ========================================================================= */
int image_checksum_ok_copy2(const uint32_t *src, uint32_t byte_len)
{
    uint32_t sum_raw, sum_dec;
    stream_checksum16_copy2(src, byte_len, &sum_raw, &sum_dec); /* Key B */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}

/* ===========================================================================
 * select_boot_slot  (flash 0x14000758)
 *
 * Read the selector dword the config pointer points at (*0x10011EAC ==
 * 0x14010000, so this reads *(0x14010000)) and return the chosen slot base.
 * Each candidate is gated through the Key-B test image_try_keys_copy2.
 *
 *   blank (0 or 0xFFFFFFFF) -> A, then B, then recovery
 *   selector == 1           -> B, then A, then recovery
 *   any other value         -> recovery, then A, then B
 *
 * update_flag != 0 is the warm-boot rollback hook: re-resolve the current slot,
 * and if it is A or B, stamp the RAW flag over that slot's header magic and
 * force the selector to recovery (2) — BOTH writes via flash_program_rmw — then
 * return recovery.
 * ========================================================================= */
uint32_t select_boot_slot(int update_flag)
{
    uint32_t selector;

    if (update_flag) {
        uint32_t cur = select_boot_slot(0);
        if (cur == SLOT_A_BASE || cur == SLOT_B_BASE) {
            uint32_t flag = (uint32_t)update_flag;
            uint32_t sel  = (uint32_t)SLOTID_RECOVERY;         /* 2 */
            /* NOR program only clears bits: this writes the flag over the magic */
            flash_program_rmw(cur + IMG_HEADER_OFFSET, &flag, 4u);
            flash_program_rmw(g_boot_config_ptr, &sel, 4u);    /* selector := 2 */
        }
        return SLOT_RECOVERY_BASE;
    }

    selector = *(volatile uint32_t *)g_boot_config_ptr;        /* *(0x14010000) */

    if (selector == 0u || selector == 0xFFFFFFFFu) {           /* blank */
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_B_BASE))
            return SLOT_B_BASE;
        return SLOT_RECOVERY_BASE;
    } else if (selector == 1u) {                               /* prefer B */
        if (image_try_keys_copy2((const uint32_t *)SLOT_B_BASE))
            return SLOT_B_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        return SLOT_RECOVERY_BASE;
    } else {                                                   /* prefer recovery */
        if (image_try_keys_copy2((const uint32_t *)SLOT_RECOVERY_BASE))
            return SLOT_RECOVERY_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        return SLOT_B_BASE;
    }
}
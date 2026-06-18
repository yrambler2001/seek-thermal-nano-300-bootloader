// SeekProFF_43X0_Bootloader/bootloader/src/boot_main.c
/* boot_main.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * Flash-resident boot leaves: slot selection, the two single-key acceptance
 * gates, and the word-zeroing helper. The top-level pipeline that drives these
 * lives in startup_lpc43xx.c's Reset_Handler (the whole boot runs from flash).
 *
 * Names are kept identical to the disassembly. Note the acceptance gates here
 * are FUSED: each reads + validates the cleartext header AND runs the keyed
 * checksum inline (there is no separate image_checksum_ok routine). Validation
 * is split per key — image_try_keys tests Key A (the transport/OTA form, used
 * by the boot-time re-encryption migration); image_try_keys_copy2 tests Key B
 * (the at-rest device form, used to choose a bootable slot). Each returns
 * 1 (accept) / 0 (reject).
 */

#include "bootloader.h"

/* ----------------------------------------------------------------------- *
 * memzero_words (flash 0x140004FC) — CRT0 .bss / BSS-descriptor zeroing.
 * Word store loop; the zero-length case is guarded up front.
 * ----------------------------------------------------------------------- */
void memzero_words(void *dst, uint32_t byte_len)
{
    uint8_t  *base = (uint8_t *)dst;
    uint32_t *p    = (uint32_t *)dst;

    if (byte_len == 0u)
        return;
    do {
        *p++ = 0u;
    } while (byte_len > (uint32_t)((uint8_t *)p - base));
}

/* ----------------------------------------------------------------------- *
 * image_try_keys (flash 0x1400050E) — acceptance gate, Key A.
 * Reads the cleartext header at slot+0x200 (uint32_t* so +128 words), requires
 * magic + length < 0x10000, then runs the Key-A checksum and accepts iff the
 * decrypted-word sum is exactly 0x00000000. (The "CLZ(sum)>>5" the compiler
 * emits is just that equality test.) Returns 1 if the slot is in transport
 * (Key-A) form; used only by the migration sweep. The checksum's raw-ciphertext
 * out-param is vestigial and ignored.
 * ----------------------------------------------------------------------- */
int image_try_keys(const uint32_t *slot_base)
{
    uint32_t len, sum_raw, sum_dec;

    if (slot_base == NULL)
        return 0;
    if (slot_base[128] != IMG_MAGIC)          /* word @ +0x200 */
        return 0;
    len = slot_base[129];                      /* word @ +0x204 */
    if (len >= IMG_MAX_LEN)
        return 0;

    stream_checksum16(slot_base, len, &sum_raw, &sum_dec);   /* Key A */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}

/* ----------------------------------------------------------------------- *
 * image_try_keys_copy2 (flash 0x14000548) — acceptance gate, Key B.
 * Same header check, Key-B checksum. Returns 1 if the slot is in at-rest
 * device (Key-B) form; this is the gate slot selection uses to decide whether
 * a candidate slot is bootable.
 * ----------------------------------------------------------------------- */
int image_try_keys_copy2(const uint32_t *slot_base)
{
    uint32_t len, sum_raw, sum_dec;

    if (slot_base == NULL)
        return 0;
    if (slot_base[128] != IMG_MAGIC)
        return 0;
    len = slot_base[129];
    if (len >= IMG_MAX_LEN)
        return 0;

    stream_checksum16_copy2(slot_base, len, &sum_raw, &sum_dec);   /* Key B */
    (void)sum_raw;
    return (sum_dec == ACCEPT_SENTINEL) ? 1 : 0;
}

/* ----------------------------------------------------------------------- *
 * select_boot_slot (flash 0x14000740) — pick a slot and return its base.
 *
 * The selector dword is read directly from the boot-config record at
 * 0x14010000; each candidate is gated through the Key-B test image_try_keys_
 * copy2. update_flag == 0 is the normal call:
 *   - blank selector (0x00000000 or 0xFFFFFFFF) -> try A, then B, then recovery
 *   - selector == 1                             -> try B, then A, then recovery
 *   - any other value                           -> try recovery, then A, then B
 *
 * update_flag != 0 is the warm-boot rollback hook: re-resolve the current slot,
 * and if it is A or B, stamp the RAW flag value over that slot's header magic
 * (invalidating it), force the config selector to recovery (2), and return
 * recovery. BOTH flash writes go through the read-modify-write path
 * (flash_program_rmw) so the rest of each block is preserved.
 * ----------------------------------------------------------------------- */
uint32_t select_boot_slot(int update_flag)
{
    uint32_t sel;

    if (update_flag) {
        uint32_t cur = select_boot_slot(0);
        if (cur == SLOT_A_BASE || cur == SLOT_B_BASE) {
            uint32_t flag = (uint32_t)update_flag;     /* raw mailbox value */
            uint32_t two  = 2u;
            /* NOR program only clears bits: this writes the flag over the magic */
            flash_program_rmw(cur + IMG_HEADER_OFFSET, &flag, 4u);
            /* selector := recovery, in place */
            flash_program_rmw(g_boot_config_ptr, &two, 4u);
        }
        return SLOT_RECOVERY_BASE;
    }

    sel = *(volatile uint32_t *)g_boot_config_ptr;     /* *(0x14010000) */

    if (sel == 0u || sel == 0xFFFFFFFFu) {             /* blank: A, B, recovery */
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_B_BASE))
            return SLOT_B_BASE;
        return SLOT_RECOVERY_BASE;
    } else if (sel == 1u) {                            /* prefer B, then A, recovery */
        if (image_try_keys_copy2((const uint32_t *)SLOT_B_BASE))
            return SLOT_B_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        return SLOT_RECOVERY_BASE;
    } else {                                           /* prefer recovery, then A, B */
        if (image_try_keys_copy2((const uint32_t *)SLOT_RECOVERY_BASE))
            return SLOT_RECOVERY_BASE;
        if (image_try_keys_copy2((const uint32_t *)SLOT_A_BASE))
            return SLOT_A_BASE;
        return SLOT_B_BASE;
    }
}
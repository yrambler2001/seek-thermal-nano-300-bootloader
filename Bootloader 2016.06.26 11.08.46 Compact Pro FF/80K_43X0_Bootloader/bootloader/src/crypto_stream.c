// SeekProFF_43X0_Bootloader/bootloader/src/crypto_stream.c
/* crypto_stream.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * Software xorshift128 (Marsaglia) keystream cipher, shift triple 11/19/8.
 * This is NOT authenticated encryption: a 128-bit-keyed PRNG, a positional XOR
 * keystream, and a 32-bit additive checksum. Keys live in keys.c.
 *
 * Structure faithful to the machine code:
 *  - xorshift128_next is a REAL, shared routine (flash 0x140007F4) that every
 *    stream routine below CALLS; it is not inlined.
 *  - prng_seed_from_key (flash 0x14000788) is the shared Key-B / silicon seeder;
 *    the Key-A seed is built inline at its call sites from the mask block.
 *  - The stream routines read XIP flash DIRECTLY, one word at a time
 *    (LDR [Rn],#4). There is no RAM scratch/staging buffer and no chunking.
 *
 * Shared rules (positional cipher):
 *  - keystream word N is consumed for image word N, so every generator is
 *    advanced for EVERY word, even where the word is passed through unchanged.
 *  - image words 128..143 (byte offsets 0x200..0x23F, the 64-byte cleartext
 *    header) are passed through VERBATIM, expressed as: word idx is verbatim
 *    iff (idx - 0x80) <= 0xF.
 *  - NO key whitening: key words seed the state directly.
 *  - acceptance is the decrypted-word sum == 0x00000000.
 */

#include "bootloader.h"

/* keys.c — the mask/Key-A block and the fixed Key-B block; the build-info block
 * (its first 4 bytes are the marker prng_seed_from_key keys off of). */
extern const uint8_t  g_key_mask[16];   /* @0x10011EB0: Key A AND silicon mask */
extern const uint8_t  g_keyB[16];       /* @0x10011EC0: fixed Key B            */
extern const uint8_t  g_build_info[];   /* flash 0x14002798: marker + date/time*/

/* ----------------------------------------------------------------------- *
 * PRNG core (flash 0x140007F4) — standalone, shared.
 * Textbook 32-bit xorshift128, shift triple (11, 19, 8). State = [x,y,z,w].
 * ----------------------------------------------------------------------- */
uint32_t xorshift128_next(uint32_t *state)
{
    uint32_t t = state[0] ^ (state[0] << 11);
    uint32_t w = state[3];

    state[0] = state[1];
    state[1] = state[2];
    state[2] = state[3];
    state[3] = (w ^ (w >> 19)) ^ t ^ (t >> 8);
    return state[3];
}

/* ----------------------------------------------------------------------- *
 * prng_seed_from_key (flash 0x14000788) — Key-B / silicon seeder.
 * Reads the 4-byte build marker (big-endian composite); if it exceeds
 * 0x01010000, use the fixed embedded Key B, else derive Key B from silicon =
 * four device-ID dwords at 0x40045000 XORed with the mask block. One-word
 * rotate, NO whitening: state = [k1, k2, k3, k0].
 *
 * In this image the marker is {01,02,00,00} -> 0x01020000 > 0x01010000, so the
 * FIXED key path is taken.
 * ----------------------------------------------------------------------- */
uint32_t *prng_seed_from_key(uint32_t *state)
{
    const uint8_t *m = g_build_info;
    uint32_t composite = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16)
                       | ((uint32_t)m[2] << 8)  |  (uint32_t)m[3];
    uint32_t k0, k1, k2, k3;

    if (composite > BUILD_MARKER_FIXEDKEY) {            /* fixed Key B */
        const uint32_t *kb = (const uint32_t *)g_keyB;
        k0 = kb[0]; k1 = kb[1]; k2 = kb[2]; k3 = kb[3];
    } else {                                            /* silicon-derived Key B */
        const volatile uint32_t *sid  = (const volatile uint32_t *)SILICON_ID_BASE;
        const uint32_t          *mask = (const uint32_t *)g_key_mask;
        k0 = sid[0] ^ mask[0];
        k1 = sid[1] ^ mask[1];
        k2 = sid[2] ^ mask[2];
        k3 = sid[3] ^ mask[3];
    }
    state[0] = k1;   /* x */
    state[1] = k2;   /* y */
    state[2] = k3;   /* z */
    state[3] = k0;   /* w */
    return state;
}

/* ----------------------------------------------------------------------- *
 * Checksum (per-key acceptance test)
 * Decrypt on the fly (header window verbatim) and accumulate two sums: the raw
 * ciphertext sum (vestigial) and the decrypted-word sum. The caller accepts
 * iff the decrypted sum is exactly 0x00000000. The "16" is a carryover; the
 * accumulators and the test are full 32-bit. stream_checksum16 keys Key A
 * (directly from the mask block); _copy2 keys Key B (via prng_seed_from_key).
 * ----------------------------------------------------------------------- */
void stream_checksum16(const uint32_t *src, uint32_t byte_len,
                       uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t        state[4];
    const uint32_t *mask = (const uint32_t *)g_key_mask;
    uint32_t        total, idx, sum_raw = 0u, sum_dec = 0u;

    /* seed Key A directly from the mask block: state = [k1,k2,k3,k0] */
    state[0] = mask[1];
    state[1] = mask[2];
    state[2] = mask[3];
    state[3] = mask[0];

    if (byte_len == 0u) { *out_sum_raw = 0u; *out_sum_dec = 0u; return; }

    total = byte_len >> 2;
    for (idx = 0u; idx < total; idx++) {
        uint32_t ks    = xorshift128_next(state);                   /* every word */
        uint32_t w     = src[idx];                                  /* XIP direct */
        uint32_t plain = ((idx - SEG_SKIP_WORDS) > 0xFu) ? (w ^ ks) : w;
        sum_raw += w;          /* vestigial */
        sum_dec += plain;      /* checked    */
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}

void stream_checksum16_copy2(const uint32_t *src, uint32_t byte_len,
                             uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t state[4];
    uint32_t total, idx, sum_raw = 0u, sum_dec = 0u;

    prng_seed_from_key(state);                                       /* Key B */

    if (byte_len == 0u) { *out_sum_raw = 0u; *out_sum_dec = 0u; return; }

    total = byte_len >> 2;
    for (idx = 0u; idx < total; idx++) {
        uint32_t ks    = xorshift128_next(state);
        uint32_t w     = src[idx];
        uint32_t plain = ((idx - SEG_SKIP_WORDS) > 0xFu) ? (w ^ ks) : w;
        sum_raw += w;
        sum_dec += plain;
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}

/* ----------------------------------------------------------------------- *
 * stream_reencrypt_keyA_to_keyB (flash 0x1400087C) — dual-key migration.
 * Runs two generators in lockstep (ksA from the mask block, ksB via
 * prng_seed_from_key) and emits out = cipher ^ ksA ^ ksB for every body word,
 * leaving the [128..143] header window verbatim. Converts a Key-A transport
 * image to its Key-B at-rest form. Returns 1 on success, 0 on a bad argument.
 * ----------------------------------------------------------------------- */
int stream_reencrypt_keyA_to_keyB(void *dst, const uint32_t *src, uint32_t byte_len)
{
    uint32_t        ksA[4], ksB[4];
    uint32_t       *out  = (uint32_t *)dst;
    const uint32_t *mask = (const uint32_t *)g_key_mask;
    uint32_t        total, idx;

    if (byte_len == 0u) return 1;
    if (src == NULL)    return 0;
    if (dst == NULL)    return 0;

    /* Key A from the mask block; Key B via prng_seed_from_key */
    ksA[0] = mask[1]; ksA[1] = mask[2]; ksA[2] = mask[3]; ksA[3] = mask[0];
    prng_seed_from_key(ksB);

    total = byte_len >> 2;
    for (idx = 0u; idx < total; idx++) {
        uint32_t a = xorshift128_next(ksA);                         /* advance BOTH */
        uint32_t b = xorshift128_next(ksB);
        uint32_t w = src[idx];
        out[idx] = ((idx - SEG_SKIP_WORDS) > 0xFu) ? (w ^ a ^ b) : w;
    }
    return 1;
}

/* ----------------------------------------------------------------------- *
 * stream_decrypt_skip_header (flash 0x140008F0) — single-key (Key B) decrypt.
 * One routine in this build. Validates args, seeds Key B internally, and XORs
 * every ciphertext word with one keystream word except the verbatim window
 * [128..143]. Used to decrypt the 0x2C0-byte segment table. Returns 1 on
 * success, 0 on a bad argument.
 * ----------------------------------------------------------------------- */
int stream_decrypt_skip_header(void *dst, const uint32_t *src, uint32_t byte_len)
{
    uint32_t  state[4];
    uint32_t *out = (uint32_t *)dst;
    uint32_t  total, idx;

    if (byte_len == 0u) return 1;
    if (src == NULL)    return 0;
    if (dst == NULL)    return 0;
    if (byte_len & 3u)  return 0;        /* length must be word-aligned */

    prng_seed_from_key(state);                                      /* Key B */

    total = byte_len >> 2;
    for (idx = 0u; idx < total; idx++) {
        uint32_t ks = xorshift128_next(state);                      /* every word */
        uint32_t w  = src[idx];                                     /* XIP direct */
        out[idx] = ((idx - SEG_SKIP_WORDS) > 0xFu) ? (w ^ ks) : w;
    }
    return 1;
}

/* ----------------------------------------------------------------------- *
 * stream_decrypt_segment (flash 0x14000938) — positional segment decrypt (B).
 * Reseed Key B, fast-forward the keystream by image_offset/4 words so it lines
 * up with this segment's absolute position in the image, then decrypt
 * byte_len bytes read from image_base+image_offset. The verbatim window is the
 * 16 words at [0x80..0x8F] in absolute image coordinates. Returns 1 on success,
 * 0 on a bad argument.
 * ----------------------------------------------------------------------- */
int stream_decrypt_segment(void *dst, uint32_t image_base,
                           uint32_t image_offset, uint32_t byte_len)
{
    uint32_t        state[4];
    uint32_t       *out = (uint32_t *)dst;
    const uint32_t *seg_src;
    uint32_t        start_word, end_word, idx, i;

    if (byte_len == 0u)                  return 1;
    if (image_base == 0u)                return 0;
    if (dst == NULL)                     return 0;
    if ((byte_len | image_offset) & 3u)  return 0;   /* both word-aligned */

    start_word = image_offset >> 2;
    end_word   = (image_offset + byte_len) >> 2;
    seg_src    = (const uint32_t *)(image_base + image_offset);

    prng_seed_from_key(state);                                      /* Key B */

    /* fast-forward to the segment's absolute word offset */
    for (i = 0u; i < start_word; i++)
        (void)xorshift128_next(state);

    for (idx = start_word; idx < end_word; idx++) {
        uint32_t ks = xorshift128_next(state);
        uint32_t w  = *seg_src++;                                   /* XIP direct */
        *out++ = ((idx - SEG_SKIP_WORDS) > 0xFu) ? (w ^ ks) : w;
    }
    return 1;
}
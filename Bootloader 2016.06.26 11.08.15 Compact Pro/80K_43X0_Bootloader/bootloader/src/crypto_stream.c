// CompactPro_43X0_Bootloader/bootloader/src/crypto_stream.c
/* crypto_stream.c — CompactPro_43X0_Bootloader (reconstructed)
 *
 * Software xorshift128 (Marsaglia) keystream cipher, shift triple 11/19/8. This
 * is NOT authenticated encryption: a 128-bit-keyed PRNG, a positional XOR
 * keystream, and a 32-bit additive checksum. Keys live in keys.c.
 *
 * The ONE structural difference from the sibling FF build: there is NO
 * standalone PRNG routine in this image — the xorshift step is INLINED into
 * every stream routine. It is written once below as a file-local `static`
 * purely for readability; the compiler is free to inline it back, reproducing
 * the machine code. (The FF build factored it into a real exported function;
 * that deduplication is why FF's boot code is 0x1C0 bytes smaller.)
 *
 * Shared rules (identical to the FF build):
 *  - The cipher is POSITIONAL: keystream word N is consumed for image word N, so
 *    every generator is advanced for every word, even pass-through words.
 *  - Image words 128..143 (byte offsets 0x200..0x23F, the cleartext header) are
 *    passed through VERBATIM so the loader can read magic + length first.
 *  - Sources are read DIRECTLY from the memory-mapped SPIFI window (no scratch
 *    staging) — the loader runs in place from flash.
 *  - No whitening: key words seed the PRNG state directly. Checksum acceptance
 *    is the full 32-bit decrypted-word sum == 0x00000000.
 *
 * NOTE on the listing: the routine at 0x1400093C is labelled
 * stream_decrypt_skip_header by the disassembler. It is renamed here to
 * stream_reencrypt_keyA_to_keyB — it runs TWO keystreams in lockstep and emits
 * out = cipher ^ ksA ^ ksB, converting a Key-A transport image to its Key-B
 * at-rest form, with the [128..143] header window preserved.
 */
#include "bootloader.h"

extern const uint8_t g_key_mask[16];   /* mask block @0x10011EB0: Key A and the silicon mask */
extern const uint8_t g_keyB[16];       /* fixed Key B @0x10011EC0                            */
extern const uint8_t g_build_info[];   /* flash 0x14002958: 4-byte marker + build date/time  */

/* ----------------------------------------------------------------------- */
/* PRNG core (inlined in the binary)                                       */
/* ----------------------------------------------------------------------- */
/* Textbook 32-bit xorshift128, shift triple (11, 19, 8). State = [x,y,z,w].
 * Static + file-local: there is no standalone PRNG symbol in this image. */
static uint32_t xorshift128_next(uint32_t *state)
{
    uint32_t t = state[0] ^ (state[0] << 11);
    uint32_t w;
    state[0] = state[1];
    state[1] = state[2];
    state[2] = state[3];
    w        = state[3];
    state[3] = (w ^ (w >> 19)) ^ t ^ (t >> 8);
    return state[3];
}

/* Seed the Key-A state directly from the 16-byte mask block. One-word rotate,
 * NO whitening: state = [k1, k2, k3, k0]. Inlined at the Key-A call sites. */
static void seed_keyA(uint32_t *state)
{
    const uint32_t *k = (const uint32_t *)g_key_mask;
    state[0] = k[1];   /* x */
    state[1] = k[2];   /* y */
    state[2] = k[3];   /* z */
    state[3] = k[0];   /* w */
}

/* ===========================================================================
 * prng_seed_from_key  (flash 0x14000858)
 * Seed the Key-B state. Read the 4-byte build marker (big-endian composite); if
 * it exceeds 0x01010000 use the fixed embedded Key B, else derive Key B from
 * silicon = four device-ID dwords at 0x40045000 XORed with the mask block. Same
 * one-word rotate, NO whitening: state = [kB1, kB2, kB3, kB0].
 * In this dump the marker is {01,02,00,00} -> 0x01020000 -> the FIXED path.
 * ========================================================================= */
uint32_t *prng_seed_from_key(uint32_t *state)
{
    const uint8_t *m = g_build_info;
    uint32_t marker = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16)
                    | ((uint32_t)m[2] << 8)  |  (uint32_t)m[3];
    uint32_t k[4];
    if (marker > BUILD_MARKER_FIXEDKEY) {                    /* fixed Key B */
        const uint32_t *kb = (const uint32_t *)g_keyB;
        k[0] = kb[0]; k[1] = kb[1]; k[2] = kb[2]; k[3] = kb[3];
    } else {                                                 /* silicon-derived */
        const volatile uint32_t *sid  = (const volatile uint32_t *)SILICON_ID_BASE;
        const uint32_t          *mask = (const uint32_t *)g_key_mask;
        k[0] = sid[0] ^ mask[0];
        k[1] = sid[1] ^ mask[1];
        k[2] = sid[2] ^ mask[2];
        k[3] = sid[3] ^ mask[3];
    }
    state[0] = k[1];   /* x */
    state[1] = k[2];   /* y */
    state[2] = k[3];   /* z */
    state[3] = k[0];   /* w */
    return state;
}

/* ----------------------------------------------------------------------- */
/* Checksum (per-key acceptance test)                                      */
/* ----------------------------------------------------------------------- */
/* Decrypt on the fly (header window verbatim) and accumulate two sums: the raw
 * ciphertext sum (vestigial) and the decrypted-word sum. Accepted by the caller
 * iff the decrypted sum is exactly 0x00000000. _copy keys Key A from the mask
 * block; _copy2 keys Key B via prng_seed_from_key. Source is read directly from
 * XIP flash. */
void stream_checksum16(const uint32_t *src, uint32_t byte_len,
                       uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t state[4];
    uint32_t total_words, idx;
    uint32_t sum_raw = 0, sum_dec = 0;
    seed_keyA(state);                                   /* Key A */
    if (byte_len == 0) { *out_sum_raw = 0; *out_sum_dec = 0; return; }
    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; idx++) {
        uint32_t ks    = xorshift128_next(state);
        uint32_t w     = src[idx];
        uint32_t plain = (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                         ? w : (w ^ ks);
        sum_raw += w;        /* vestigial raw-ciphertext sum */
        sum_dec += plain;    /* the one that is checked      */
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}

void stream_checksum16_copy2(const uint32_t *src, uint32_t byte_len,
                             uint32_t *out_sum_raw, uint32_t *out_sum_dec)
{
    uint32_t state[4];
    uint32_t total_words, idx;
    uint32_t sum_raw = 0, sum_dec = 0;
    prng_seed_from_key(state);                          /* Key B */
    if (byte_len == 0) { *out_sum_raw = 0; *out_sum_dec = 0; return; }
    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; idx++) {
        uint32_t ks    = xorshift128_next(state);
        uint32_t w     = src[idx];
        uint32_t plain = (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
                         ? w : (w ^ ks);
        sum_raw += w;
        sum_dec += plain;
    }
    *out_sum_raw = sum_raw;
    *out_sum_dec = sum_dec;
}

/* ----------------------------------------------------------------------- */
/* Re-encrypt (dual key: Key A -> Key B)                                   */
/* ----------------------------------------------------------------------- */
/* The boot-time migration transform (labelled stream_decrypt_skip_header at
 * 0x1400093C; see the rename note at the top). Runs two generators in lockstep
 * (ksA from the mask block, ksB via prng_seed_from_key) and emits
 * out = cipher ^ ksA ^ ksB for every body word, with [128..143] passed through
 * verbatim. Converts a Key-A transport image to its Key-B at-rest form. */
int stream_reencrypt_keyA_to_keyB(void *dst, const uint32_t *src, uint32_t byte_len)
{
    uint32_t  ksA_state[4];
    uint32_t  ksB_state[4];
    uint32_t *out = (uint32_t *)dst;
    uint32_t  total_words, idx;
    if (byte_len == 0) return 1;
    if (src == NULL)   return 0;
    if (dst == NULL)   return 0;
    if (byte_len & 3u) return 0;
    seed_keyA(ksA_state);              /* Key A (transport)  */
    prng_seed_from_key(ksB_state);     /* Key B (at-rest)    */
    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; idx++) {
        uint32_t ksA = xorshift128_next(ksA_state);   /* advance BOTH every word */
        uint32_t ksB = xorshift128_next(ksB_state);
        uint32_t w   = src[idx];
        if (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
            out[idx] = w;                   /* header window: verbatim */
        else
            out[idx] = w ^ ksA ^ ksB;       /* A-form -> B-form        */
    }
    return 1;
}

/* ----------------------------------------------------------------------- */
/* Decrypt (single key, Key B)                                             */
/* ----------------------------------------------------------------------- */
/* Decrypt a whole image into dst with a single internally-seeded Key-B
 * keystream; every ciphertext word is XORed except the 16-word header window
 * [128..143], copied through unchanged. Used to decrypt the 0x2C0-byte segment
 * table. One logical routine: the disassembler labels two entry points
 * (_entry 0x140009F8, _body 0x140009FE) that share a single frame; _body seeds
 * Key B internally. */
int stream_decrypt_skip_header(void *dst, const uint32_t *src, uint32_t byte_len)
{
    uint32_t  state[4];
    uint32_t *out = (uint32_t *)dst;
    uint32_t  total_words, idx;
    if (byte_len == 0)        return 1;
    if (src == NULL)          return 0;
    if (dst == NULL)          return 0;
    if (byte_len & 3u)        return 0;     /* length must be word-aligned */
    prng_seed_from_key(state);              /* Key B, seeded internally    */
    total_words = byte_len >> 2;
    for (idx = 0; idx < total_words; idx++) {
        uint32_t ks = xorshift128_next(state);
        uint32_t w  = src[idx];
        if (idx >= IMG_HDR_WORD_FIRST && idx <= IMG_HDR_WORD_LAST)
            out[idx] = w;                   /* header window: verbatim */
        else
            out[idx] = w ^ ks;              /* body: XOR keystream     */
    }
    return 1;
}

/* ----------------------------------------------------------------------- */
/* Positional segment decrypt (single key, Key B)                          */
/* ----------------------------------------------------------------------- */
/* Segmented boot path. Reseed Key B internally, then fast-forward the keystream
 * by image_offset/4 words so it lines up with this segment's absolute position
 * within the overall image. The verbatim window is the 16 words
 * [SEG_SKIP_WORDS .. SEG_SKIP_WORDS+15] in absolute image coordinates
 * (SEG_SKIP_WORDS == 0x80 -> the same 0x200..0x23F header window). */
int stream_decrypt_segment(void *dst, uint32_t image_base, uint32_t image_offset,
                           uint32_t byte_len)
{
    uint32_t        state[4];
    uint32_t       *out = (uint32_t *)dst;
    const uint32_t *seg_src;
    uint32_t        start_word, end_word, idx;
    if (byte_len == 0)                  return 1;
    if (image_base == 0)                return 0;
    if (dst == NULL)                    return 0;
    if ((byte_len | image_offset) & 3u) return 0;   /* both word-aligned */
    prng_seed_from_key(state);                      /* Key B             */
    start_word = image_offset >> 2;                 /* absolute start word */
    end_word   = (image_offset + byte_len) >> 2;
    seg_src    = (const uint32_t *)(image_base + image_offset);
    /* fast-forward the keystream to the segment's absolute word offset */
    for (idx = 0; idx < start_word; idx++)
        (void)xorshift128_next(state);
    for (idx = start_word; idx < end_word; idx++) {
        uint32_t ks = xorshift128_next(state);
        uint32_t w  = *seg_src++;
        if (idx >= SEG_SKIP_WORDS && (idx - SEG_SKIP_WORDS) <= 0xFu)
            *out++ = w;                 /* verbatim window */
        else
            *out++ = w ^ ks;
    }
    return 1;
}
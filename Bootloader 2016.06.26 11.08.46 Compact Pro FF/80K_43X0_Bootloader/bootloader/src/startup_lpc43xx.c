// SeekProFF_43X0_Bootloader/bootloader/src/startup_lpc43xx.c
/* startup_lpc43xx.c — SeekProFF_43X0_Bootloader (reconstructed)
 *
 * Vector table + Reset_Handler for the LPC43xx SPIFI first-stage bootloader.
 *
 * The CRT0 here is deliberately MINIMAL — it reproduces exactly what the
 * disassembly does and nothing more:
 *   - NO M4MEMMAP remap (the core's reset map is left in place; VTOR alone
 *     is repointed at the application during hand-off)
 *   - NO stack canary / guard word
 *   - NO __set_MSP (the MSP loaded from vector[0] at reset is used as-is)
 *   - VTOR is written ONCE, at hand-off, to 0x10000000
 *
 * Names are kept identical to the disassembly.
 */

#include "bootloader.h"

/* ------- core/peripheral registers touched by CRT0 (literal addresses) ----- */
#define RGU_RESET_CTRL0   (*(volatile uint32_t *)0x40053100u)
#define RGU_RESET_CTRL1   (*(volatile uint32_t *)0x40053104u)
#define NVIC_ICPR         ((volatile uint32_t *)0xE000E280u)   /* ICPR0..ICPR7   */
#define SCB_VTOR          (*(volatile uint32_t *)0xE000ED08u)

/* ------- linker-provided symbols ------------------------------------------- */
extern uint32_t __stack_top;                 /* = 0x10018000 (vector[0])        */
extern const uint32_t __scatter_load_table[];/* 8 words emitted at 0x14000240   */

/* ------- handlers ---------------------------------------------------------- */
void Reset_Handler(void) __attribute__((noreturn));
void Default_Handler(void);

/* system + fault handlers: this image traps everything (spin) */
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

/* the M4 catch-all referenced in the disassembly */
void IRQ52_Handler(void)      __attribute__((weak, alias("Default_Handler")));

/* ------- all-trap vector table (LPC43xx M4: 16 system + 53 IRQ) ------------ */
__attribute__((used, section(".isr_vector")))
void (* const g_pfnVectors[])(void) =
{
    (void (*)(void))(&__stack_top),   /* 0x00  initial MSP = 0x10018000        */
    Reset_Handler,                    /* 0x04  reset -> 0x14000262             */
    NMI_Handler,                      /* 0x08                                  */
    HardFault_Handler,                /* 0x0C                                  */
    MemManage_Handler,                /* 0x10                                  */
    BusFault_Handler,                 /* 0x14                                  */
    UsageFault_Handler,               /* 0x18                                  */
    0, 0, 0, 0,                       /* 0x1C..0x28 reserved                   */
    SVC_Handler,                      /* 0x2C                                  */
    DebugMon_Handler,                 /* 0x30                                  */
    0,                                /* 0x34 reserved                         */
    PendSV_Handler,                   /* 0x38                                  */
    SysTick_Handler,                  /* 0x3C                                  */

    /* IRQ0..IRQ52 — all routed to the trap (image services no interrupts) */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /*  0.. 3 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /*  4.. 7 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /*  8..11 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 12..15 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 16..19 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 20..23 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 24..27 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 28..31 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 32..35 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 36..39 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 40..43 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 44..47 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 48..51 */
    IRQ52_Handler                                                       /* 52     */
};

/* ===========================================================================
 * Reset_Handler  (flash 0x14000262)
 *
 * PUSH {R4-R7,LR}; SUB SP,#0x2C4 — i.e. a normal frame with the 0x2C0-byte
 * segment table held on the stack. Sequence is exactly:
 *   CPSID i -> mailbox check/clear -> RGU reset -> NVIC ICPR clear ->
 *   scatter-load -> spifi_init -> A->B migration (3 slots unrolled) ->
 *   select_boot_slot -> segmented load -> hand-off.
 * ========================================================================= */
__attribute__((noreturn, section(".after_vectors")))
void Reset_Handler(void)
{
    uint32_t        seg[SEG_TABLE_BYTES / 4];   /* 0x2C0 bytes on the stack    */
    uint32_t        update_flag;
    uint32_t        slot_base;
    uint32_t        slot_id;
    const uint32_t *st;
    const uint32_t *d;

    __disable_irq();                            /* CPSID i                     */

    /* ---- warm-boot update mailbox -------------------------------------- *
     * Honour the request only when the flag is one of the two sentinels
     * (flag + 0x55AA00FF <= 1) AND the paired 64-bit gate is <= 0x752F.
     * The RAW flag value is what gets passed on to select_boot_slot — it is
     * NOT remapped to a 1/2 index here.                                     */
    update_flag = *(volatile uint32_t *)RAM_UPDATE_FLAG;
    if ((update_flag + 0x55AA00FFu) > 1u) {
        update_flag = 0u;
    } else {
        uint32_t gate_lo = *(volatile uint32_t *)(RAM_UPDATE_MAGIC + 0u);
        uint32_t gate_hi = *(volatile uint32_t *)(RAM_UPDATE_MAGIC + 4u);
        if (gate_hi != 0u || gate_lo > UPDATE_MAGIC_GATE) {
            update_flag = 0u;
        }
    }

    /* consume the mailbox so the request fires exactly once */
    *(volatile uint32_t *)RAM_UPDATE_FLAG        = 0u;
    *(volatile uint32_t *)(RAM_UPDATE_MAGIC + 0u) = 0u;
    *(volatile uint32_t *)(RAM_UPDATE_MAGIC + 4u) = 0u;

    /* ---- minimal peripheral reset -------------------------------------- */
    RGU_RESET_CTRL0 = 0x10DF1000u;
    RGU_RESET_CTRL1 = 0x01DFF7FFu;

    /* clear all pending NVIC interrupts (ICPR0..ICPR7) */
    NVIC_ICPR[0] = 0xFFFFFFFFu; NVIC_ICPR[1] = 0xFFFFFFFFu;
    NVIC_ICPR[2] = 0xFFFFFFFFu; NVIC_ICPR[3] = 0xFFFFFFFFu;
    NVIC_ICPR[4] = 0xFFFFFFFFu; NVIC_ICPR[5] = 0xFFFFFFFFu;
    NVIC_ICPR[6] = 0xFFFFFFFFu; NVIC_ICPR[7] = 0xFFFFFFFFu;

    /* ---- scatter-load: copy .data, zero .bss, relocate SPIFI driver ---- *
     * Fixed 3-entry table at 0x14000240.                                    */
    st = __scatter_load_table;
    scatterload_copy_words((const void *)st[0], (void *)st[1], st[2]); /* .data  */
    memzero_words((void *)st[3], st[4]);                               /* .bss   */
    scatterload_copy_words((const void *)st[5], (void *)st[6], st[7]); /* driver */

    /* ---- bring up SPI-NOR through the relocated bespoke driver ---------- */
    spifi_init();

    /* ---- A->B migration sweep (three slots, unrolled) ------------------ *
     * If a slot still validates under Key A (transport form), stage it,
     * re-encrypt to Key B (at-rest form), erase the slot and write it back.
     * The length is read directly as the cleartext word at slot+0x204.      */
    if (image_try_keys((const uint32_t *)SLOT_A_BASE)) {
        uint32_t len = *(volatile uint32_t *)(SLOT_A_BASE + IMG_HEADER_OFFSET + 4u);
        memcpy_auto((void *)STAGING_BUF_BASE, (const void *)SLOT_A_BASE, len);
        if (stream_reencrypt_keyA_to_keyB((void *)STAGING_BUF_BASE,
                                          (const uint32_t *)STAGING_BUF_BASE, len)) {
            if (flash_chip_erase(SLOT_A_BASE)) {
                flash_program(SLOT_A_BASE, (const void *)STAGING_BUF_BASE, len, 0);
                image_try_keys_copy2((const uint32_t *)SLOT_A_BASE);
            }
        }
    }
    if (image_try_keys((const uint32_t *)SLOT_B_BASE)) {
        uint32_t len = *(volatile uint32_t *)(SLOT_B_BASE + IMG_HEADER_OFFSET + 4u);
        memcpy_auto((void *)STAGING_BUF_BASE, (const void *)SLOT_B_BASE, len);
        if (stream_reencrypt_keyA_to_keyB((void *)STAGING_BUF_BASE,
                                          (const uint32_t *)STAGING_BUF_BASE, len)) {
            if (flash_chip_erase(SLOT_B_BASE)) {
                flash_program(SLOT_B_BASE, (const void *)STAGING_BUF_BASE, len, 0);
                image_try_keys_copy2((const uint32_t *)SLOT_B_BASE);
            }
        }
    }
    if (image_try_keys((const uint32_t *)SLOT_RECOVERY_BASE)) {
        uint32_t len = *(volatile uint32_t *)(SLOT_RECOVERY_BASE + IMG_HEADER_OFFSET + 4u);
        memcpy_auto((void *)STAGING_BUF_BASE, (const void *)SLOT_RECOVERY_BASE, len);
        if (stream_reencrypt_keyA_to_keyB((void *)STAGING_BUF_BASE,
                                          (const uint32_t *)STAGING_BUF_BASE, len)) {
            if (flash_chip_erase(SLOT_RECOVERY_BASE)) {
                flash_program(SLOT_RECOVERY_BASE, (const void *)STAGING_BUF_BASE, len, 0);
                image_try_keys_copy2((const uint32_t *)SLOT_RECOVERY_BASE);
            }
        }
    }

    /* ---- choose the slot to boot (raw mailbox flag passed through) ------ */
    slot_base = select_boot_slot((int)update_flag);

    if      (slot_base == SLOT_A_BASE) slot_id = SLOTID_A;
    else if (slot_base == SLOT_B_BASE) slot_id = SLOTID_B;
    else                               slot_id = SLOTID_RECOVERY;

    /* ---- decrypt the 0x2C0-byte segment table (Key B) onto the stack --- */
    stream_decrypt_skip_header(seg, (const uint32_t *)slot_base, SEG_TABLE_BYTES);

    /* pass 1: descriptors [144..155] = { srcRef, dstVMA, len } x4 *
     * srcRef is stored as 0x14000000 + image_offset; the ciphertext base is
     * the chosen slot, so image_offset = srcRef - 0x14000000.              */
    for (d = &seg[144]; d < &seg[156]; d += 3) {
        stream_decrypt_segment((void *)d[1], slot_base, d[0] - BOOTLOADER_BASE, d[2]);
    }
    /* zero BSS: descriptors [156..163] = { VMA, len } x4 */
    for (d = &seg[156]; d < &seg[164]; d += 2) {
        memzero_words((void *)d[0], d[1]);
    }
    /* pass 2: descriptors [164..175] = { srcRef, dstVMA, len } x4 */
    for (d = &seg[164]; d < &seg[176]; d += 3) {
        stream_decrypt_segment((void *)d[1], slot_base, d[0] - BOOTLOADER_BASE, d[2]);
    }

    /* ---- install application vector table and hand off ----------------- *
     * The first 0x200 bytes of the decrypted segment table ARE the app
     * vector table; copy them to 0x10000000, publish the handoff cells,
     * repoint VTOR, re-enable IRQs and branch to entry (word 132).
     * NOTE: the MSP is NOT reloaded here — the entry stub owns its stack.   */
    memcpy_auto((void *)RAM_APP_LOAD_BASE, seg, IMG_HEADER_OFFSET);

    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 0u) = (uint32_t)g_build_info; /* 0x14002798 */
    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 4u) = g_boot_config_ptr;      /* 0x14010000 */
    *(volatile uint32_t *)(RAM_HANDOFF_BLOCK + 8u) = slot_id;

    SCB_VTOR = RAM_APP_LOAD_BASE;               /* VTOR = 0x10000000          */

    __enable_irq();                             /* CPSIE i                    */

    ((void (*)(void))seg[132])();               /* jump to application entry  */

    for (;;) { }                                /* not reached                */
}

/* ===========================================================================
 * scatterload_copy_words  (flash 0x140004E6)
 *
 * Word copy used by CRT0. Register order is (R0=src, R1=dst, R2=byte_len);
 * it computes (src - dst) once and indexes the source off the destination
 * cursor. No src!=dst short-circuit — the copy is unconditional.
 * ========================================================================= */
void scatterload_copy_words(const void *src, void *dst, uint32_t byte_len)
{
    uint32_t       *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    uint32_t        n = byte_len >> 2;
    uint32_t        i;

    for (i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

/* ===========================================================================
 * Default_Handler — every system/IRQ vector traps here and spins.
 * ========================================================================= */
void Default_Handler(void)
{
    for (;;) { }
}
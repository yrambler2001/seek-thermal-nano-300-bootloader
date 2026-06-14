/* startup_lpc43xx.c — 80K_43X0_Bootloader (reconstructed)
 *
 * Reset/CRT0 + vector table. FLASH-resident bootstrap: the part executes it
 * in place (XIP) from 0x14000000, and its final act is to relocate the rest
 * of the image into SRAM and branch there.
 *
 * Most peripheral-interrupt vectors point at a single trap, IRQ52_Handler.
 * That is the fingerprint of the stock LPCOpen vector table whose ~49 weak
 * *_IRQHandler aliases all resolve to one default handler; the linker/
 * disassembler then names that shared body after one of the slots (IRQ52).
 * The four 0 entries in the IRQ range, plus the system-reserved slots, are
 * NXP's reserved vector positions. We keep the disassembly name rather than
 * re-introduce the named aliases, so source and listing line up exactly.
 *
 * Three slots are different: external interrupts IRQ12, IRQ13 and IRQ14 carry
 * real handlers (IRQ12_Handler / IRQ13_Handler / IRQ14_Handler). Unlike the
 * flash-resident traps above, these three are RAM-resident — they live in
 * .text_ram and are relocated into SRAM by Reset_Handler before use, and the
 * vector table holds their SRAM addresses. The disassembly carries no names
 * for them; the labels here are assigned by behaviour and the peripheral
 * identities are inferred from the literal base addresses.
 */
#include "bootloader.h"
#include "chip.h"      /* LPCOpen: LPC_CREG, LPC_RGU, SCB, NVIC, __set_MSP, __WFI */

#define BOOT_TEXT __attribute__((section(".text_boot")))   /* stays in flash    */
#define RAM_TEXT  __attribute__((section(".text_ram")))    /* relocated to SRAM */

/* ----------------------------- handlers -------------------------------- */
/* Core faults: "WFI; B ." -> spin executing WFI.                          */
BOOT_TEXT void NMI_Handler(void)        { for (;;) __WFI(); }
BOOT_TEXT void HardFault_Handler(void)  { for (;;) __WFI(); }
BOOT_TEXT void MemManage_Handler(void)  { for (;;) __WFI(); }
BOOT_TEXT void BusFault_Handler(void)   { for (;;) __WFI(); }
BOOT_TEXT void UsageFault_Handler(void) { for (;;) __WFI(); }
BOOT_TEXT void SVC_Handler(void)        { for (;;) __WFI(); }
BOOT_TEXT void DebugMon_Handler(void)   { for (;;) __WFI(); }
BOOT_TEXT void PendSV_Handler(void)     { for (;;) __WFI(); }
BOOT_TEXT void SysTick_Handler(void)    { for (;;) __WFI(); }

/* Catch-all for every unused peripheral IRQ: plain spin, NO wfi ("B ." only). */
BOOT_TEXT __attribute__((noreturn)) void IRQ52_Handler(void) { for (;;) {} }

/* --------------------- live peripheral interrupt handlers --------------- */
/* These three service APB peripherals whose base addresses come straight from
 * the handlers' literal pools. The exact peripheral identity is inferred: each
 * presents a status/control register at +0x00 whose bit0 is the pending flag
 * (write-1-to-clear), and IRQ13/IRQ14 additionally clear bit0 of the registers
 * at +0x04 and +0x14 (an enable/flag pair, timer/SCT-class behaviour). */
#define IRQ12_REG     (*(volatile uint32_t *)0x40084000u)        /* status @+0x00 */
#define IRQ13_REG(o)  (*(volatile uint32_t *)(0x40085000u + (o)))
#define IRQ14_REG(o)  (*(volatile uint32_t *)(0x400C3000u + (o)))

typedef void (*irq_callback_t)(uint32_t);

/* RAM cells (.bss). Addresses observed in the image:
 *   s_irq14_callback @0x1001DC90   s_irq12_count @0x1001DC98 (64-bit)
 *   s_irq13_arg      @0x1001DCA0   s_irq14_arg   @0x1001DCA4
 *   s_irq13_callback @0x1001DCA8
 * Their exact placement is a linker detail; the names are assigned here. */
static volatile uint32_t       s_irq12_count[2];     /* [0]=low, [1]=high */
static volatile irq_callback_t s_irq13_callback;
static volatile uint32_t       s_irq13_arg;
static volatile irq_callback_t s_irq14_callback;
static volatile uint32_t       s_irq14_arg;

/* IRQ12: acknowledge the pending flag and bump a 64-bit event counter. The
 * machine adds 1 to the HIGH word only (adds #0 / adc #1), so each event
 * advances the 64-bit value by 2^32 — reproduced verbatim from the bytes. */
RAM_TEXT void IRQ12_Handler(void)
{
    if (IRQ12_REG & 1u) {
        IRQ12_REG = 1u;                 /* write-1-to-clear */
        s_irq12_count[1] += 1u;         /* low word left untouched, as in the asm */
    }
}

/* IRQ13: if pending, acknowledge bit0 and clear bit0 of the +0x14 and +0x04
 * registers, then dispatch an optional callback (tail-called with its arg). */
RAM_TEXT void IRQ13_Handler(void)
{
    if (IRQ13_REG(0x00) & 1u) {
        IRQ13_REG(0x00)  = 1u;
        IRQ13_REG(0x14) &= ~1u;
        IRQ13_REG(0x04) &= ~1u;
    }
    if (s_irq13_callback)
        s_irq13_callback(s_irq13_arg);
}

/* IRQ14: identical shape to IRQ13 on a different peripheral base. */
RAM_TEXT void IRQ14_Handler(void)
{
    if (IRQ14_REG(0x00) & 1u) {
        IRQ14_REG(0x00)  = 1u;
        IRQ14_REG(0x14) &= ~1u;
        IRQ14_REG(0x04) &= ~1u;
    }
    if (s_irq14_callback)
        s_irq14_callback(s_irq14_arg);
}

/* --------------------------- vector table ------------------------------ */
extern void Reset_Handler(void);

void (* const g_pfnVectors[])(void) __attribute__((section(".vectors"), used)) =
{
    (void (*)(void))MSP_TOP,   /*  0  initial SP  = 0x10020000          */
    Reset_Handler,             /*  1  reset                              */
    NMI_Handler,               /*  2                                     */
    HardFault_Handler,         /*  3                                     */
    MemManage_Handler,         /*  4                                     */
    BusFault_Handler,          /*  5                                     */
    UsageFault_Handler,        /*  6                                     */
    0, 0, 0, 0,                /*  7..10  reserved                       */
    SVC_Handler,               /* 11                                     */
    DebugMon_Handler,          /* 12                                     */
    0,                         /* 13  reserved                           */
    PendSV_Handler,            /* 14                                     */
    SysTick_Handler,           /* 15                                     */
    /* -------- external interrupts IRQ0..IRQ52 (LPC43xx M4) --------     */
    IRQ52_Handler, IRQ52_Handler,                               /* 0,1   */
    0, 0,                                                       /* 2,3 r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 4..7  */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 8..11 */
    IRQ12_Handler, IRQ13_Handler, IRQ14_Handler, IRQ52_Handler, /* 12..15*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 16..19*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 20..23*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 24..27*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 28..31*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 32..35*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 36..39*/
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 40..43*/
    0,                                                          /* 44  r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler,                /* 45..47*/
    0,                                                          /* 48  r */
    IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, IRQ52_Handler, /* 49..52*/
};

/* ----------------- scatter-load tables (from the .ld) ------------------ */
extern uint32_t __data_section_table[];      /* { LMA, VMA, len } triplets */
extern uint32_t __data_section_table_end[];
extern uint32_t __bss_section_table[];        /* { addr, len } pairs        */
extern uint32_t __bss_section_table_end[];

extern void jump_to_ram_stage(int arg);

/* The copy/zero loops are written inline (never as a call into the relocated
 * region, which does not exist yet). Disable loop-idiom recognition so the
 * compiler does not turn them into memcpy/memset calls that would live in
 * .text_ram and crash here. (LPCXpresso's stock startup avoids this the same
 * way, via dedicated data_init/bss_init helpers compiled into flash.)        */
BOOT_TEXT __attribute__((noreturn, optimize("no-tree-loop-distribute-patterns")))
void Reset_Handler(void)
{
    uint32_t *t;

    __disable_irq();                                /* CPSID i              */

    /* Alias the 0x10000000 SRAM bank to address 0 (so the app we will load
     * at 0x10000000 is also reachable via the legacy 0 view), and keep the
     * live vector table in flash for now. boot_main flips VTOR to 0x10000000
     * just before launching the application. */
    LPC_CREG->M4MEMMAP = RAM_APP_LOAD_BASE;         /* 0x10000000           */
    SCB->VTOR          = (uint32_t)g_pfnVectors;    /* 0x14000000           */

    /* Stack canary at SP-0x800, then (re)load MSP. The hardware already
     * loaded MSP from vector[0] at reset; the original re-arms it explicitly
     * to the same value right after planting the canary. */
    *(volatile uint32_t *)STACK_GUARD_ADDR = STACK_GUARD_VALUE;  /* 0x1001F800 */
    __set_MSP(MSP_TOP);                                          /* 0x10020000 */

    /* Pulse-reset peripherals left configured by the boot ROM, then clear
     * every pending interrupt. */
    LPC_RGU->RESET_CTRL[0] = 0x10DF1000u;
    LPC_RGU->RESET_CTRL[1] = 0x01DFF7FFu;
    for (int i = 0; i < 8; i++)
        NVIC->ICPR[i] = 0xFFFFFFFFu;                /* ICPR0..ICPR7          */

    /* CRT0 scatter-load. The listing runs four passes — zero the BSS pair,
     * an empty pass, copy the .data triplet (keys/config), copy the .text_ram
     * triplet (the code executed from SRAM) — in that order. The regions are
     * disjoint, so we fold them into a BSS pass then a DATA pass; the resulting
     * memory image is identical. Both loops are INLINE: at this point the SRAM
     * copy of the program does not exist yet, so we must not call into it. */

    /* zero BSS: table of { addr, len } pairs */
    for (t = __bss_section_table; t < __bss_section_table_end; ) {
        uint32_t *dst = (uint32_t *)*t++;
        uint32_t  len = *t++;
        for (uint32_t off = 0; off < len; off += 4)
            dst[off >> 2] = 0u;
    }

    /* copy DATA + TEXT_RAM: table of { LMA(src), VMA(dst), len } triplets */
    for (t = __data_section_table; t < __data_section_table_end; ) {
        const uint32_t *src = (const uint32_t *)*t++;
        uint32_t       *dst = (uint32_t *)*t++;
        uint32_t        len = *t++;
        if (src != dst)
            for (uint32_t off = 0; off < len; off += 4)
                dst[off >> 2] = src[off >> 2];
    }

    /* Branch into the SRAM copy of boot_main (never returns). */
    jump_to_ram_stage(0);
    for (;;) {}                                     /* not reached           */
}

/* Thin trampoline; stays in flash. boot_main is LINKED at its SRAM VMA
 * (0x1001C124). Reset_Handler has just copied .text_ram there, so this call
 * transfers into SRAM. The listing does the equivalent literal BX 0x1001C125. */
BOOT_TEXT __attribute__((noreturn))
void jump_to_ram_stage(int arg)
{
    (void)arg;
    boot_main();
    for (;;) {}     /* boot_main is noreturn */
}
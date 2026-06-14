/* board_timer.c — 80K_43X0_Bootloader / Compact Android CW (reconstructed)
 *
 * Board GPIO/pin bring-up plus a TIMER0-based busy-wait, run by boot_main right
 * after the SPIFI controller comes up and before any slot is touched. These
 * functions do not exist in the Compact Pro FF image; the labels here are
 * assigned by behaviour and the peripheral identities come from the literal
 * base addresses in the handlers' code.
 *
 * LOAD-BEARING CAVEAT: the ticks-per-millisecond multiplier read by
 * timer_ms_to_ticks lives in .bss (0x1001DB98) and is never written inside this
 * 64 KB image, so as captured it reads 0 and the "200 ms" delay degenerates to
 * a no-op. Whatever calibrates it is board/SDK bring-up outside this image
 * (the same situation as the external staging buffer in the sibling build).
 */
#include "bootloader.h"

/* SCU pin-config registers (base 0x40086000; SFSP[p][n] at +0x80*p + 4*n). */
#define SCU_SFSP1_1   (*(volatile uint32_t *)0x40086084u)   /* P1_1  */
#define SCU_SFSP1_2   (*(volatile uint32_t *)0x40086088u)   /* P1_2  */
#define SCU_SFSP1_4   (*(volatile uint32_t *)0x40086090u)   /* P1_4  */
#define SCU_SFSP2_13  (*(volatile uint32_t *)0x40086134u)   /* P2_13 */
#define SCU_SFSP3_0   (*(volatile uint32_t *)0x40086180u)   /* P3_0  */

/* GPIO: DIR register for port 1, plus two byte-addressable pin cells
 * (GPIO_PORT base 0x400F4000; byte pin [n] = base + n, n = port*32 + pin). */
#define GPIO_DIR1     (*(volatile uint32_t *)0x400F6004u)
#define GPIO_B_P0_14  (*(volatile uint8_t  *)0x400F400Eu)   /* byte pin [14] = P0_14 */
#define GPIO_B_P1_2   (*(volatile uint8_t  *)0x400F4022u)   /* byte pin [34] = P1_2  */

/* TIMER0 timer-counter (free-running tick source). */
#define TIMER0_TC     (*(volatile uint32_t *)0x40084008u)

/* RAM cells (.bss). Observed addresses in the image:
 *   g_boot_cfg            @0x1001DA98   g_boot_status @0x1001DA9C
 *   g_timer_ticks_per_ms  @0x1001DB98   (uninitialised in-image; see caveat)
 * Their exact placement is a linker detail; the names are assigned here. */
static volatile uint32_t g_boot_cfg;            /* 0x1001DA98 */
static volatile uint32_t g_boot_status;         /* 0x1001DA9C */
static volatile uint32_t g_timer_ticks_per_ms;  /* 0x1001DB98 */

/* ----------------------------------------------------------------------- */
int timer0_read_tc(void)
{
    return (int)TIMER0_TC;
}

int timer_ms_to_ticks(int ms)
{
    return ms * (int)g_timer_ticks_per_ms;
}

/* Stash a status word and a config word into the two BSS cells, returning the
 * status. (Inferred names; the routine just records (result, cfg).) */
int stash_status_config_values(int result, int cfg)
{
    g_boot_status = (uint32_t)result;   /* 0x1001DA9C */
    g_boot_cfg    = (uint32_t)cfg;      /* 0x1001DA98 */
    return result;
}

/* Configure a handful of SCU pins and two GPIO outputs, then busy-wait roughly
 * 200 ms on TIMER0, then record (1, 13) via stash_status_config_values. The pin
 * values are reproduced verbatim from the image; the board intent behind them
 * (which lines these enable/drive) is not recoverable from the image alone. */
int board_gpio_init_timer_delay(void)
{
    uint32_t ticks, t0;

    SCU_SFSP1_1  = 0x45u;
    SCU_SFSP1_2  = 0x05u;
    SCU_SFSP3_0  = 0x04u;
    SCU_SFSP2_13 = 0x00u;
    SCU_SFSP1_4  = 0x40u;

    GPIO_DIR1   |= 0x2000u;     /* P1_13 -> output */
    GPIO_B_P1_2  = 0u;          /* byte pin [34] low  */
    GPIO_B_P0_14 = 1u;          /* byte pin [14] high */

    ticks = (uint32_t)timer_ms_to_ticks(200);
    t0    = (uint32_t)timer0_read_tc();
    while (ticks > (uint32_t)timer0_read_tc() - t0)
        ;                       /* ~200 ms settle (no-op if calibration is 0) */

    return stash_status_config_values(1, 13);
}
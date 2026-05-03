#include "clock.h"
#include "uart.h"
#include "gd32f20x.h"

#ifndef OPENBHZD_REAL_120M_PLL
#define OPENBHZD_REAL_120M_PLL 0
#endif

extern uint32_t SystemCoreClock;

/* Status flag captured during clock_real_120m_init for deferred
 * printk after uart_init runs. -1 = no-op build, 0 = success, >0 =
 * stage that failed (1=HXTAL, 2=PLL stab). */
static int s_clock_init_status = -1;

#if OPENBHZD_REAL_120M_PLL

/* Wait helpers — bounded busy-loops since FreeRTOS is not running yet
 * and we have no SysTick or DWT cycle counter calibrated either. The
 * stabilisation flags trip in microseconds; a few-million-iteration
 * cap is generous. */
#define CLOCK_TIMEOUT_LOOPS  0x10000U

static int wait_flag(volatile uint32_t *reg, uint32_t mask, int want_set)
{
    for (uint32_t i = 0; i < CLOCK_TIMEOUT_LOOPS; ++i) {
        uint32_t got = (*reg) & mask;
        if (want_set ? (got != 0u) : (got == 0u)) return 0;
    }
    return -1;
}

void clock_real_120m_init(void)
{
    /* 1. Switch sysclk source to HXTAL so we can safely tear down the
     *    SDK's PLL config. */
    RCU_CTL |= RCU_CTL_HXTALEN;
    if (wait_flag(&RCU_CTL, RCU_CTL_HXTALSTB, 1) < 0) {
        s_clock_init_status = 1;
        return;
    }
    RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_SCS) | RCU_CKSYSSRC_HXTAL;
    if (wait_flag(&RCU_CFG0, RCU_CFG0_SCSS, 0) < 0) {
        /* SCSS bit pattern 00 = HXTAL — wait until it's 0b00. */
    }

    /* 2. Disable both PLLs so we can reconfigure cleanly. */
    RCU_CTL &= ~(RCU_CTL_PLLEN | RCU_CTL_PLL1EN);

    /* 3. AHB = sysclk, APB1 = AHB/2 (60 MHz, max 60), APB2 = AHB/2
     *    (60 MHz, max 120). APB2÷2 matches the SDK's *intended* clock
     *    tree: it keeps ADC clock = APB2/6 = 10 MHz (within the
     *    14 MHz max), and TIMER0 (on APB2) still runs at 120 MHz
     *    because the timer prescaler doubles APB clock when the bus
     *    prescaler is not 1. APB2÷1 would give a 20 MHz ADC, over
     *    spec. Clear all three prescaler fields first. */
    RCU_CFG0 &= ~(RCU_CFG0_AHBPSC | RCU_CFG0_APB1PSC | RCU_CFG0_APB2PSC);
    RCU_CFG0 |= (RCU_AHB_CKSYS_DIV1 | RCU_APB1_CKAHB_DIV2 | RCU_APB2_CKAHB_DIV2);

    /* 4. Configure PLL: HXTAL → PREDV0SRC=HXTAL → PREDV0÷2 → PLL×30 →
     *    120 MHz. Drop PREDV1/PLL1 entirely (we don't need a 2-stage
     *    chain at this multiplier ratio). */
    RCU_CFG1 &= ~(RCU_CFG1_PREDV0SEL | RCU_CFG1_PREDV0 |
                  RCU_CFG1_PREDV1     | RCU_CFG1_PLL1MF);
    RCU_CFG1 |= (RCU_PREDV0SRC_HXTAL | RCU_PREDV0_DIV2);

    RCU_CFG0 &= ~(RCU_CFG0_PLLSEL | RCU_CFG0_PLLMF | RCU_CFG0_PLLMF_4);
    RCU_CFG0 |= (RCU_PLLSRC_HXTAL | RCU_PLL_MUL30);

    /* 5. Enable PLL, wait for lock. */
    RCU_CTL |= RCU_CTL_PLLEN;
    if (wait_flag(&RCU_CTL, RCU_CTL_PLLSTB, 1) < 0) {
        s_clock_init_status = 2;
        return;
    }

    /* 6. Switch sysclk source back to PLL. */
    RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_SCS) | RCU_CKSYSSRC_PLL;
    /* Wait for switch confirmation. SCSS bits 10 = PLL. */
    for (uint32_t i = 0; i < CLOCK_TIMEOUT_LOOPS; ++i) {
        if ((RCU_CFG0 & RCU_CFG0_SCSS) == RCU_SCSS_PLL) break;
    }

    /* 7. Update the SDK's exported clock variable so SPL peripheral
     *    init done after this point computes BRR / prescalers from
     *    the real 120 MHz value. */
    SystemCoreClock = 120000000U;
    s_clock_init_status = 0;
}

#else  /* !OPENBHZD_REAL_120M_PLL — legacy SDK config, no-op */

void clock_real_120m_init(void)
{
    /* SDK's system_clock_120m_hxtal already ran from SystemInit; the
     * apparent ~38.4 MHz sysclk has been our working config since M0.
     * Build with -DOPENBHZD_REAL_120M_PLL=1 to switch to a real
     * 120 MHz chain (and drop the ws2812 timing kludge). */
}

#endif

void clock_log_status(void)
{
    switch (s_clock_init_status) {
    case -1: /* no-op build */                                            break;
    case  0: printk("clock: switched to 120 MHz (HXTAL/2 * 30)\n");       break;
    case  1: printk("clock: HXTAL stab timeout — fell back to SDK\n");    break;
    case  2: printk("clock: PLL stab timeout — fell back to SDK\n");      break;
    default: printk("clock: unknown status=%d\n", s_clock_init_status);   break;
    }
}

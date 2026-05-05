#include "rtc.h"
#include "uart.h"
#include "gd32f20x.h"
#include "gd32f20x_rtc.h"
#include "gd32f20x_bkp.h"
#include "gd32f20x_pmu.h"

/* Magic split across two 16-bit BKP data registers. Pick a value
 * that's vanishingly unlikely to appear after a power-on (BKP RAM
 * is undefined cold but typically all-zero or all-one). */
#define RTC_MAGIC_LO 0xF00DU
#define RTC_MAGIC_HI 0xCAFEU

/* LSI nominal 40 kHz; PSC = 40000-1 yields a 1 Hz tick. Real LSI
 * drifts ±5% which is fine for our 30-min HA resync window. */
#define RTC_PRESCALER (40000U - 1U)

static int magic_valid(void)
{
    return (BKP_DATA0 == RTC_MAGIC_LO) && (BKP_DATA1 == RTC_MAGIC_HI);
}

static void magic_set(int valid)
{
    BKP_DATA0 = valid ? RTC_MAGIC_LO : 0u;
    BKP_DATA1 = valid ? RTC_MAGIC_HI : 0u;
}

/* Bounded waits — keep the RTC bring-up from wedging the boot if the
 * peripheral clock or LSI isn't behaving. ~50 ms at 120 MHz core. */
static int rtc_lwoff_wait_bounded(void)
{
    for (uint32_t i = 0; i < 800000u; ++i) {
        if (RTC_CTL & RTC_CTL_LWOFF) return 1;
    }
    return 0;
}

/* RSYNF sets within microseconds on a healthy RTC, but allow up to
 * ~2 s here so that even a worst-case 1 Hz shadow-sync window can't
 * leave us reading a stale counter on warm boot. */
static int rtc_rsyn_wait_bounded(void)
{
    RTC_CTL &= ~RTC_CTL_RSYNF;
    for (uint32_t i = 0; i < 30000000u; ++i) {
        if (RTC_CTL & RTC_CTL_RSYNF) return 1;
    }
    return 0;
}

void rtc_init(void)
{
    /* APB1 clocks for PMU + BKP register interface. */
    rcu_periph_clock_enable(RCU_PMU);
    rcu_periph_clock_enable(RCU_BKPI);

    /* Allow writes to backup domain (BKP_DATAx + RTC + RCU_BDCTL). */
    pmu_backup_write_enable();

    /* Diagnostic: surface raw BKP/RTC/BDCTL state at boot. Sample BKP_DATA
     * into locals so the print and the magic check can't disagree about
     * what we just read. Also peek at the RTC divider twice with a small
     * busy-wait between, which tells us whether RTCCLK is actually
     * ticking — a healthy RTC will have DIV decrement between the two
     * reads, a frozen RTC will hold the same value. */
    uint16_t b0 = BKP_DATA0;
    uint16_t b1 = BKP_DATA1;
    int      mv = (b0 == RTC_MAGIC_LO) && (b1 == RTC_MAGIC_HI);
    uint32_t div_a = (RTC_DIVH << 16) | (RTC_DIVL & 0xFFFFu);
    for (volatile int i = 0; i < 200000; ++i) { __asm__ volatile (""); }
    uint32_t div_b = (RTC_DIVH << 16) | (RTC_DIVL & 0xFFFFu);
    printk("rtc: bkp0=0x%04x bkp1=0x%04x cnt=0x%08x bdctl=0x%08x "
           "div_a=0x%08x div_b=0x%08x mv=%d\n",
           (unsigned)b0, (unsigned)b1,
           (unsigned)rtc_counter_get(), (unsigned)RCU_BDCTL,
           (unsigned)div_a, (unsigned)div_b, mv);

    if (mv) {
        /* Backup domain already configured by a previous boot in this
         * VDD cycle. Don't reset it (that would wipe BKP_DATA + RTC).
         *
         * Make sure BDCTL.RTCEN is set — survives non-VDD resets but
         * cheap to re-assert defensively.
         *
         * Skip rtc_register_sync_wait() here: it polls RSYNF after
         * clearing it, which can hang forever if the RTC peripheral
         * clock or LSI source isn't running for any reason. The
         * counter reads in rtc_load_unix() are robust to slightly
         * stale shadow registers (they re-sync within a few LSI
         * ticks after the warm reset). */
        rcu_periph_clock_enable(RCU_RTC);
        return;
    }

    /* Fresh power-on (or first boot ever): bring up the backup domain.
     * Reset clears any stale BKP/RTC config. */
    rcu_bkp_reset_enable();
    rcu_bkp_reset_disable();

    /* Start LSI (IRC40K) and wait for it to stabilise. LSI lives in
     * the always-on domain and isn't gated by VBAT; we use it as the
     * RTC reference because LSE crystal isn't characterised on this
     * PCB. */
    rcu_osci_on(RCU_IRC40K);
    rcu_osci_stab_wait(RCU_IRC40K);

    rcu_rtc_clock_config(RCU_RTCSRC_IRC40K);
    rcu_periph_clock_enable(RCU_RTC);

    /* Wait for RTC APB shadow regs to mirror peripheral state, then
     * push a 1 Hz prescaler. Bounded — if RSYNF/LWOFF never set we
     * leave magic invalid and fall back to "no RTC" rather than hang. */
    if (!rtc_rsyn_wait_bounded()) { magic_set(0); return; }
    if (!rtc_lwoff_wait_bounded()) { magic_set(0); return; }
    rtc_configuration_mode_enter();
    rtc_prescaler_set(RTC_PRESCALER);
    rtc_configuration_mode_exit();
    (void)rtc_lwoff_wait_bounded();

    magic_set(0);
}

int rtc_load_unix(uint32_t *out)
{
    if (!magic_valid()) return 0;
    /* The RTC counter shadow regs reset to 0 on every system reset
     * and must re-sync from the always-on side before the read is
     * meaningful. RSYNF gets set every 1 Hz tick (PSC=39999 @ LSI),
     * so we wait up to ~2 s; if it never sets the RTC really is dead
     * and we report cold. */
    (void)rtc_rsyn_wait_bounded();
    uint32_t cnt = rtc_counter_get();
    if (cnt == 0u) {
        printk("rtc: load found cnt=0 (shadow not synced) — cold\n");
        return 0;
    }
    if (out) *out = cnt;
    return 1;
}

void rtc_store_unix(uint32_t unix_seconds)
{
    if (!rtc_lwoff_wait_bounded()) {
        printk("rtc: store unix=%u TIMEOUT (LWOFF#1 stuck)\n",
               (unsigned)unix_seconds);
        return;
    }
    rtc_counter_set(unix_seconds);
    if (!rtc_lwoff_wait_bounded()) {
        printk("rtc: store unix=%u TIMEOUT (LWOFF#2 stuck)\n",
               (unsigned)unix_seconds);
        return;
    }
    magic_set(1);
    /* Force a shadow re-sync so the read-back proves the write took. */
    (void)rtc_rsyn_wait_bounded();
    uint32_t verify = rtc_counter_get();
    printk("rtc: stored unix=%u verify=0x%08x (bkp0=0x%04x bkp1=0x%04x)\n",
           (unsigned)unix_seconds, (unsigned)verify,
           (unsigned)BKP_DATA0, (unsigned)BKP_DATA1);
}

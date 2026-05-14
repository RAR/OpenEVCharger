#include "hal/rtc.h"
#include "hal/uart.h"
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
 * peripheral clock or LSI isn't behaving. */
static int rtc_lwoff_wait_bounded(void)
{
    for (uint32_t i = 0; i < 800000u; ++i) {
        if (RTC_CTL & RTC_CTL_LWOFF) return 1;
    }
    return 0;
}

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

    /* LSI (IRC40K) lives in the system-domain RCU_RSTSCK register and
     * gets disabled by every system reset / SYSRESETREQ. Even when
     * BDCTL.RTCEN + BDCTL.RTCSRC=LSI survive (backup domain), the clock
     * source itself is off after a warm boot, the counter freezes, and
     * shadow regs can't sync. So always re-assert LSI here, regardless
     * of whether we're cold or warm. */
    rcu_osci_on(RCU_IRC40K);
    rcu_osci_stab_wait(RCU_IRC40K);

    if (magic_valid()) {
        /* Backup domain (BKP_DATA, RTC counter, BDCTL) is intact. */
        rcu_periph_clock_enable(RCU_RTC);
        return;
    }

    /* Fresh power-on (or first boot ever): bring up the backup domain.
     * Reset clears any stale BKP/RTC config. */
    rcu_bkp_reset_enable();
    rcu_bkp_reset_disable();

    rcu_rtc_clock_config(RCU_RTCSRC_IRC40K);
    rcu_periph_clock_enable(RCU_RTC);

    /* Bounded RSYNF + LWOFF — if either expires we bail with magic
     * invalid rather than hang the boot. */
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
    /* RTC counter shadow regs reset to 0 on every system reset and
     * must re-sync from the always-on side before the read is
     * meaningful. */
    (void)rtc_rsyn_wait_bounded();
    uint32_t cnt = rtc_counter_get();
    if (cnt == 0u) return 0;
    if (out) *out = cnt;
    return 1;
}

void rtc_store_unix(uint32_t unix_seconds)
{
    if (!rtc_lwoff_wait_bounded()) return;
    rtc_counter_set(unix_seconds);
    if (!rtc_lwoff_wait_bounded()) return;
    magic_set(1);
}

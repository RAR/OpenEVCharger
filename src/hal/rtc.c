#include "rtc.h"
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

void rtc_init(void)
{
    /* APB1 clocks for PMU + BKP register interface. */
    rcu_periph_clock_enable(RCU_PMU);
    rcu_periph_clock_enable(RCU_BKPI);

    /* Allow writes to backup domain (BKP_DATAx + RTC + RCU_BDCTL). */
    pmu_backup_write_enable();

    if (magic_valid()) {
        /* Backup domain already configured by a previous boot in this
         * VDD cycle. Don't reset it (that would wipe BKP_DATA + RTC).
         * Just sync the shadow registers in case the prescaler/counter
         * read paths need it. */
        rtc_register_sync_wait();
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
     * push a 1 Hz prescaler. */
    rtc_register_sync_wait();
    rtc_lwoff_wait();
    rtc_configuration_mode_enter();
    rtc_prescaler_set(RTC_PRESCALER);
    rtc_configuration_mode_exit();
    rtc_lwoff_wait();

    magic_set(0);
}

int rtc_load_unix(uint32_t *out)
{
    if (!magic_valid()) return 0;
    rtc_register_sync_wait();
    if (out) *out = rtc_counter_get();
    return 1;
}

void rtc_store_unix(uint32_t unix_seconds)
{
    rtc_lwoff_wait();
    rtc_counter_set(unix_seconds);
    rtc_lwoff_wait();
    magic_set(1);
}

/* boards/nexcyber/main.c — M1 bring-up entry point.
 *
 * Brings the clock + log UART up, then starts a FreeRTOS scheduler with
 * one heartbeat task. Proves the ARM_CM4F port + SysTick wiring +
 * vector-table aliasing + heap_4 all land coherently on the Nations
 * silicon. No safety core, no comms, no persistence — those land in
 * M2+ as the shared HAL ports.
 *
 * The previous M0 was a bare-metal busy-loop heartbeat in main()
 * (commit ab0a478). Swapping that for a scheduler-driven heartbeat
 * is the smallest possible "FreeRTOS works on this chip" trace —
 * if the printk timing changes from "tight nop loop" to "1 Hz exact
 * via vTaskDelay(pdMS_TO_TICKS(1000))" then the scheduler is alive.
 *
 * See boards/nexcyber/README.md for the porting milestone roadmap.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hal/clock.h"
#include "hal/uart.h"
#include "hal/gpio.h"
#include "hal/adc_scan.h"
#include "hal/cp_pwm.h"
#include "hal/spi2.h"
#include "hal/bl0939.h"
#include "hal/nextion.h"
#include "hal/relay.h"
#include "hal/gfci.h"
#include "pin_map.h"
#include "core/j1772.h"
#include "n32g45x.h"

/* Newlib's __libc_init_array references _init/_fini; we have no C++
 * static ctors so empty stubs are fine. Same idiom as src/main.c on
 * the rippleon target. */
void _init(void) {}
void _fini(void) {}

/* FreeRTOS hooks. configCHECK_FOR_STACK_OVERFLOW=2 + configUSE_MALLOC_FAILED_HOOK=1
 * (see src/FreeRTOSConfig.h) make these mandatory link-time symbols. Trap
 * into an infinite loop with interrupts disabled so a debugger can break
 * in and inspect — same pattern as rippleon's src/main.c. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task; (void)task_name;
    __asm volatile("cpsid i");
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    __asm volatile("cpsid i");
    for (;;) {}
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;
    uint16_t adc[ADC_RANKS];
    for (;;) {
        /* Every 5 beats, dump ADC1 readings alongside the heartbeat.
         * Format: "adc pa6=%u pc0=%u pc1=%u vref=%u" — raw 12-bit
         * counts. VrefInt should sit near 1490-1540 (≈1.20 V at 3.3 V
         * Vref → 12-bit raw). If vref reads outside that band, the
         * Vref calibration or HXTAL is off. */
        if ((beat % 5) == 0) {
            adc_scan_latest(adc);
            printk("adc pa6=%u pc0=%u pc1=%u vref=%u\n",
                   (unsigned)adc[ADC_RANK_PA6],
                   (unsigned)adc[ADC_RANK_PC0],
                   (unsigned)adc[ADC_RANK_PC1],
                   (unsigned)adc[ADC_RANK_VREFINT]);
        }
        printk("beat %u\n", (unsigned)beat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* FreeRTOS-aware delay shim for relay/gfci drivers. */
static void task_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* SWD-triggered bench test command mailbox. The bench operator can
 * poke a command number from openocd:
 *   pin_probe.py poke <addr_of_g_bench_cmd> N
 * where N is one of:
 *   1 = relay_close (50 ms close pulse, then hold PA0 HIGH)
 *   2 = relay_open
 *   3 = gfci_cal_pulse (500 ms)
 *   4 = bl0939_smoke_test (one-shot register dump via printk)
 * Monitor task polls this every 100 ms and ACKs by zeroing it.
 *
 * `volatile` + the global symbol survives in the .map so the address
 * is easy to find with `nm openevcharger.elf | grep g_bench_cmd`. */
volatile uint32_t g_bench_cmd = 0;

static void bench_run_cmd(uint32_t cmd)
{
    switch (cmd) {
    case 1:
        printk("bench: relay_close (50 ms pulse, then hold PA0 HIGH)\n");
        relay_close(50, task_delay_ms);
        break;
    case 2:
        printk("bench: relay_open\n");
        relay_open();
        break;
    case 3:
        printk("bench: gfci_cal_pulse (500 ms)\n");
        gfci_cal_pulse(500, task_delay_ms);
        break;
    case 4:
        printk("bench: bl0939_smoke_test\n");
        bl0939_smoke_test();
        break;
    default:
        printk("bench: unknown cmd %u\n", (unsigned)cmd);
        break;
    }
}

/* Read PC13 (STOP loop) + PC3/PC7 (mains L1/L2 detect). All active-HIGH
 * (PC13 = NC switch closed when HIGH; PC3/PC7 = leg present when HIGH). */
static void read_safety_inputs(int *stop_ok, int *l1, int *l2)
{
    *stop_ok = GPIO_ReadInputDataBit(PIN_STOP_SENSE_PORT,
                                     PIN_STOP_SENSE_PIN) ? 1 : 0;
    *l1 = GPIO_ReadInputDataBit(PIN_MAINS_DETECT_L1_PORT,
                                PIN_MAINS_DETECT_L1_PIN) ? 1 : 0;
    *l2 = GPIO_ReadInputDataBit(PIN_MAINS_DETECT_L2_PORT,
                                PIN_MAINS_DETECT_L2_PIN) ? 1 : 0;
}

/* Map J1772 state → stock-fw Nextion page name. Not exhaustive —
 * these are the four pages identified in the stock firmware string
 * table; mapping here is a best guess. */
static const char *page_for_state(j1772_state_t s)
{
    switch (s) {
    case J1772_STATE_A: return "page nogun";
    case J1772_STATE_B: return "page waittime";
    case J1772_STATE_C: return "page chargeing";
    case J1772_STATE_D: return "page chargeing";
    case J1772_STATE_E: return "page setting";
    case J1772_STATE_F: return "page setting";
    default:            return NULL;
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    j1772_ctx_t ctx;
    j1772_init(&ctx);

    uint32_t prev_stop = 0xFF, prev_l1 = 0xFF, prev_l2 = 0xFF;
    j1772_state_t prev_committed = J1772_STATE_INVALID;
    uint32_t tick = 0;

    for (;;) {
        /* Refresh ADC2 scan (the only place that drives the
         * diagnostic + CP read-back buffer). 100 ms = 10 Hz. */
        adc2_diag_scan();
        int32_t cp_mv = adc2_cp_mv();

        /* J1772 debounce: 5 consecutive samples (≈ 500 ms) before a
         * state transition commits. */
        j1772_state_t committed = j1772_step(&ctx, cp_mv, 5);

        /* Periodic status log every 1 s (every 10 ticks). */
        if ((tick % 10) == 0) {
            int stop_ok, l1, l2;
            read_safety_inputs(&stop_ok, &l1, &l2);
            printk("monitor: cp=%d mV (raw=%u) j1772=%s stop=%d L1=%d L2=%d hold=%d\n",
                   (int)cp_mv, (unsigned)adc2_cp_raw(),
                   j1772_state_name(committed),
                   stop_ok, l1, l2, relay_hold_asserted() ? 1 : 0);

            if (stop_ok != (int)prev_stop && prev_stop != 0xFF) {
                printk("monitor: STOP changed %u -> %d\n",
                       (unsigned)prev_stop, stop_ok);
            }
            if (l1 != (int)prev_l1 && prev_l1 != 0xFF) {
                printk("monitor: L1 changed %u -> %d\n",
                       (unsigned)prev_l1, l1);
            }
            if (l2 != (int)prev_l2 && prev_l2 != 0xFF) {
                printk("monitor: L2 changed %u -> %d\n",
                       (unsigned)prev_l2, l2);
            }
            prev_stop = (uint32_t)stop_ok;
            prev_l1   = (uint32_t)l1;
            prev_l2   = (uint32_t)l2;
        }

        /* On committed J1772 state transition, log + flip Nextion page. */
        if (committed != prev_committed && committed != J1772_STATE_INVALID) {
            printk("J1772: state %s -> %s (cp=%d mV)\n",
                   j1772_state_name(prev_committed),
                   j1772_state_name(committed),
                   (int)cp_mv);
            const char *page = page_for_state(committed);
            if (page) {
                nextion_send_cmd(page);
            }
            prev_committed = committed;
        }

        /* PC11 safety-supervisor heartbeat — toggle once per tick
         * (100 ms = 5 Hz pulse train, 10 Hz edge rate). Stock fw's
         * rate is bench-blocked; pick something visible to a scope
         * until we measure it. M5+ replaces this with a rate match. */
        if ((tick & 1u) == 0) {
            GPIO_SetBits(PIN_SAFETY_LOOP_EN_PORT,
                         PIN_SAFETY_LOOP_EN_PIN);
        } else {
            GPIO_ResetBits(PIN_SAFETY_LOOP_EN_PORT,
                           PIN_SAFETY_LOOP_EN_PIN);
        }

        /* SWD-triggered bench command mailbox. */
        uint32_t cmd = g_bench_cmd;
        if (cmd != 0) {
            g_bench_cmd = 0;
            bench_run_cmd(cmd);
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int main(void)
{
    clock_real_120m_init();
    uart_init();
    clock_log_status();
    printk("openevcharger nexcyber: M2 bring-up (FreeRTOS + GPIO HAL)\n");

    gpio_init_all();
    gpio_log_straps();

    /* M3 ADC HAL — fires DMA1 ch1 streaming ADC1 samples into the
     * private s_adc_buf. Must run AFTER gpio_init_all() so the AIN
     * pads (PA4-PA7, PB0-2, PC0/1/4/5) are already in analog mode. */
    adc_scan_init();
    printk("adc1 scan up (4 ranks: PA6/PC0/PC1/VrefInt)\n");

    /* M3 CP PWM — TIM1_CH1 → PA8, 1 kHz, idle = +12 V advertise.
     * PA8 pad already in AF_PP per gpio_init_all(). */
    cp_pwm_init();
    cp_pwm_set_idle_high();
    printk("cp pwm up (TIM1_CH1, 1 kHz, idle +12 V)\n");

    /* M3 SPI2 + BL0939 — hardware SPI on PB12-15, ~562 kHz. Pads
     * configured to AF_PP / AF input / OUT_PP by gpio_init_all() in
     * M2. Run a one-shot smoke test that reads a few defaulted /
     * runtime registers to verify the link is alive. */
    spi2_init();
    bl0939_smoke_test();

    /* M3 Nextion HMI link — USART2 / 9600 8N1 / PA2-PA3 / DMA1 ch6 RX.
     * Bench-blocked: cannot validate without the display attached.
     * Sends a "page setting" probe so an operator can see if the
     * display reaches its first screen — confirms TX wire + correct
     * baud rate. RX is silent unless the user touches the screen. */
    nextion_init();
    nextion_send_cmd("page setting");
    printk("nextion: USART2 up @ 9600, sent 'page setting'\n");

    /* M4 relay + GFCI drivers — pads already OUT_PP via gpio_init_all.
     * Just zeroes the ODR bits and sets internal state. */
    relay_init();
    gfci_init();
    printk("relay/gfci drivers initialised (contactors open)\n");

    /* 256 words = 1 KB stack — plenty for printk + an itoa scratch. */
    BaseType_t ok = xTaskCreate(heartbeat_task, "heartbeat",
                                256, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        printk("xTaskCreate failed: %d\n", (int)ok);
        for (;;) {}
    }

    /* M4 monitor task — runs at 10 Hz, reads CP, steps J1772 state
     * machine, logs state changes, updates Nextion page, pulses PC11
     * safety-supervisor heartbeat, polls STOP + mains-detect inputs. */
    BaseType_t mon_ok = xTaskCreate(monitor_task, "monitor",
                                    320, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (mon_ok != pdPASS) {
        printk("monitor xTaskCreate failed: %d\n", (int)mon_ok);
        for (;;) {}
    }

    vTaskStartScheduler();

    /* Should never return. If it does, the scheduler couldn't start
     * — typically out of heap for the idle task. */
    printk("scheduler returned — halt\n");
    for (;;) {}
}

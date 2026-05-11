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

        /* ADC2 diagnostic scan — every beat (1 Hz). Updates the
         * adc2_diag_buf so an SWD peek during a J1772 state walk
         * reveals which physical AIN pin carries CP_RAW. */
        adc2_diag_scan();
        printk("beat %u\n", (unsigned)beat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    /* 256 words = 1 KB stack — plenty for printk + an itoa scratch. */
    BaseType_t ok = xTaskCreate(heartbeat_task, "heartbeat",
                                256, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        printk("xTaskCreate failed: %d\n", (int)ok);
        for (;;) {}
    }

    vTaskStartScheduler();

    /* Should never return. If it does, the scheduler couldn't start
     * — typically out of heap for the idle task. */
    printk("scheduler returned — halt\n");
    for (;;) {}
}

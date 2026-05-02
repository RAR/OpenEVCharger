/* OpenBHZD M0 — heartbeat LED on PD4.
 *
 * Sets up the system clock at 120 MHz (handled by the vendor's SystemInit
 * called from startup_gd32f20x_cl.S) and toggles PD4 once per second using
 * SysTick as the time base.
 *
 * Once M1 introduces FreeRTOS, this file becomes the kernel-bringup
 * entry point and the blink moves into io_task.
 */

#include "gd32f20x.h"

/* Newlib's __libc_init_array (called from startup before main) walks
 * the .init_array section and also references _init/_fini. We have no
 * C++ static constructors, so these can be empty stubs.
 */
void _init(void) {}
void _fini(void) {}

#define HEARTBEAT_PORT  GPIOD
#define HEARTBEAT_PIN   GPIO_PIN_4
#define HEARTBEAT_RCU   RCU_GPIOD

static volatile uint32_t systick_ms;

void SysTick_Handler(void)
{
    systick_ms++;
}

static void systick_init(void)
{
    /* SystemCoreClock is set by SystemInit() in system_gd32f20x.c */
    SysTick_Config(SystemCoreClock / 1000U);   /* 1 ms tick */
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = systick_ms;
    while ((systick_ms - start) < ms) {
        __WFI();
    }
}

static void heartbeat_init(void)
{
    rcu_periph_clock_enable(HEARTBEAT_RCU);
    gpio_init(HEARTBEAT_PORT,
              GPIO_MODE_OUT_PP,
              GPIO_OSPEED_2MHZ,
              HEARTBEAT_PIN);
    gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
}

int main(void)
{
    systick_init();
    heartbeat_init();

    for (;;) {
        gpio_bit_set(HEARTBEAT_PORT, HEARTBEAT_PIN);
        delay_ms(500);
        gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
        delay_ms(500);
    }
}

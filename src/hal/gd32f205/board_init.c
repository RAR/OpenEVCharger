/* src/hal/gd32f205/board_init.c — GD32F205 (Rippleon ROC001) board hooks.
 *
 * The raw vendor-SPL bring-up that used to be inlined in src/main.c lives
 * here so main() stays board-agnostic. Calls + ordering are lifted
 * verbatim from the pre-Task-10 main() — each hook keeps the original
 * call site, so the bl-call stream in main() is unchanged (codegen
 * neutral; see boards' Task-10 regression gate). */

#include "hal/board_init.h"
#include "hal/clock.h"
#include "gd32f20x.h"
#include "pin_map.h"

void board_early_init(void)
{
    /* If built with OPENEVCHARGER_REAL_120M_PLL=1, swap the SDK's broken
     * 120m_hxtal config for a clean direct chain. No-op otherwise. */
    clock_real_120m_init();
}

void board_debug_pins_init(void)
{
    /* Release JTAG pins (PA15, PB3, PB4) so SPI3 (PB3/PB4/PB5) and
     * TIMER1_CH0 (PA15) can use them. SWDPENABLE keeps SWD alive
     * (PA13/PA14) for the OpenOCD probe. Must run before any AF init
     * touches those pins — including spi3_init() / w25q_init(). */
    rcu_periph_clock_enable(RCU_AF);
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
}

void board_fc41d_release(void)
{
    /* Normal mode: power up the FC41D and release reset so its
     * firmware runs and starts answering us on UART4. Stock
     * firmware's Thd_Wifi did the same VEN-then-CEN release with
     * a delay between. Without this the module sits dead. */
    gpio_bit_set(PIN_FC41D_VEN_PORT, PIN_FC41D_VEN_PIN);
    for (volatile int i = 0; i < 600000; ++i) { __asm__ volatile (""); }
    gpio_bit_set(PIN_FC41D_CEN_PORT, PIN_FC41D_CEN_PIN);
}

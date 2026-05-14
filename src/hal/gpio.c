#include "gpio.h"
#include "uart.h"
#include "pin_map.h"
#include "gd32f20x.h"

static void clock_enable_all(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_AF);
}

static void init_outputs_safe_low(void)
{
    /* Drive pin LOW *before* configuring as output (BC clears the bit
     * regardless of current MODE). Then configure as output PP. This
     * guarantees zero glitch to a momentary HIGH on any safety output.
     *
     * Polarity (re-confirmed 2026-05-03): PE12 HIGH = closed, LOW =
     * open. Idle LOW = open at boot, what we want. */
    gpio_bit_reset(PIN_RELAY_MAIN_PORT,       PIN_RELAY_MAIN_PIN);
    gpio_bit_reset(PIN_RELAY_AUX_PORT,        PIN_RELAY_AUX_PIN);
    gpio_bit_reset(PIN_RELAY_FORCE_OPEN_PORT, PIN_RELAY_FORCE_OPEN_PIN);
    gpio_bit_reset(PIN_GFCI_CAL_PORT,         PIN_GFCI_CAL_PIN);
    gpio_bit_reset(PIN_BUZZER_PORT,           PIN_BUZZER_PIN);
    gpio_bit_reset(PIN_U11_G0_PORT,           PIN_U11_G0_PIN);
    gpio_bit_reset(PIN_U11_G1_PORT,           PIN_U11_G1_PIN);
    gpio_bit_reset(PIN_FC41D_VEN_PORT,        PIN_FC41D_VEN_PIN);
    gpio_bit_reset(PIN_FC41D_CEN_PORT,        PIN_FC41D_CEN_PIN);
    gpio_bit_reset(PIN_FC41D_WAKE_PORT,       PIN_FC41D_WAKE_PIN);
    gpio_bit_reset(PIN_HEARTBEAT_PORT,        PIN_HEARTBEAT_PIN);

    gpio_init(PIN_RELAY_MAIN_PORT,       GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_RELAY_MAIN_PIN);
    gpio_init(PIN_RELAY_AUX_PORT,        GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_RELAY_AUX_PIN);
    gpio_init(PIN_RELAY_FORCE_OPEN_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_RELAY_FORCE_OPEN_PIN);
    gpio_init(PIN_GFCI_CAL_PORT,   GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_GFCI_CAL_PIN);
    gpio_init(PIN_BUZZER_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_BUZZER_PIN);
    gpio_init(PIN_U11_G0_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_U11_G0_PIN);
    gpio_init(PIN_U11_G1_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_U11_G1_PIN);
    gpio_init(PIN_FC41D_VEN_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_VEN_PIN);
    gpio_init(PIN_FC41D_CEN_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_CEN_PIN);
    gpio_init(PIN_FC41D_WAKE_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_WAKE_PIN);
    gpio_init(PIN_HEARTBEAT_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_HEARTBEAT_PIN);
}

static void init_w25q_pads(void)
{
    /* CS deasserted (HIGH) BEFORE configuring as output. */
    gpio_bit_set(PIN_W25Q_CS_PORT, PIN_W25Q_CS_PIN);
    gpio_init(PIN_W25Q_CS_PORT,   GPIO_MODE_OUT_PP,      GPIO_OSPEED_50MHZ, PIN_W25Q_CS_PIN);
    gpio_init(PIN_W25Q_SCK_PORT,  GPIO_MODE_AF_PP,       GPIO_OSPEED_50MHZ, PIN_W25Q_SCK_PIN);
    gpio_init(PIN_W25Q_MOSI_PORT, GPIO_MODE_AF_PP,       GPIO_OSPEED_50MHZ, PIN_W25Q_MOSI_PIN);
    gpio_init(PIN_W25Q_MISO_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_W25Q_MISO_PIN);
    /* SPI3 peripheral itself stays disabled until M4. */
}

static void init_cp_pwm_pad(void)
{
    /* TIM1_CH3 full-remap → PE13. AFIO remap is set when TIM1 driver
     * runs in M3; the pad just needs AF_PP here. With TIM1 disabled the
     * pin stays high-impedance on AF in F1-style register model. */
    gpio_init(PIN_CP_PWM_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, PIN_CP_PWM_PIN);
}

static void init_inputs_floating(void)
{
    gpio_init(PIN_WS2812_PORT,      GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_WS2812_PIN);
    gpio_init(PIN_STRAP_PB7_PORT,   GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_STRAP_PB7_PIN);
    gpio_init(PIN_STRAP_PB14_PORT,  GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_STRAP_PB14_PIN);
}

static void init_inputs_pullup(void)
{
    gpio_bit_set(PIN_BTN_PC9_PORT,   PIN_BTN_PC9_PIN);
    gpio_bit_set(PIN_DIP1_PORT,      PIN_DIP1_PIN);
    gpio_bit_set(PIN_DIP2_PORT,      PIN_DIP2_PIN);
    gpio_bit_set(PIN_DIP3_PORT,      PIN_DIP3_PIN);
    gpio_bit_set(PIN_DIP4_PORT,      PIN_DIP4_PIN);
    gpio_bit_set(PIN_STRAP_PB8_PORT, PIN_STRAP_PB8_PIN);
    gpio_bit_set(PIN_STRAP_PE2_PORT, PIN_STRAP_PE2_PIN);

    gpio_init(PIN_BTN_PC9_PORT,   GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_BTN_PC9_PIN);
    gpio_init(PIN_DIP1_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP1_PIN);
    gpio_init(PIN_DIP2_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP2_PIN);
    gpio_init(PIN_DIP3_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP3_PIN);
    gpio_init(PIN_DIP4_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP4_PIN);
    gpio_init(PIN_STRAP_PB8_PORT, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_STRAP_PB8_PIN);
    gpio_init(PIN_STRAP_PE2_PORT, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_STRAP_PE2_PIN);
}

static void init_analog_inputs(void)
{
    gpio_init(PIN_ADC_AC_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_AC_PIN);
    gpio_init(PIN_ADC_NTC1_PORT,   GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NTC1_PIN);
    gpio_init(PIN_ADC_CT_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CT_PIN);
    gpio_init(PIN_ADC_LCT_PORT,    GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_LCT_PIN);
    gpio_init(PIN_ADC_CP_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CP_PIN);
    gpio_init(PIN_ADC_CC_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CC_PIN);
    gpio_init(PIN_ADC_PE_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_PE_PIN);
    gpio_init(PIN_ADC_NTC2_PORT,   GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NTC2_PIN);
    gpio_init(PIN_ADC_UNUSED_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_UNUSED_PIN);
    gpio_init(PIN_ADC_BTN_PORT,    GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_BTN_PIN);

    gpio_init(PIN_ADC_NC0_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC0_PIN);
    gpio_init(PIN_ADC_NC1_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC1_PIN);
    gpio_init(PIN_ADC_NC2_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC2_PIN);
}

void gpio_init_all(void)
{
    clock_enable_all();
    init_outputs_safe_low();
    init_w25q_pads();
    init_cp_pwm_pad();
    init_inputs_floating();
    init_inputs_pullup();
    init_analog_inputs();
}

void gpio_log_straps(void)
{
    int dip1 = (gpio_input_bit_get(PIN_DIP1_PORT, PIN_DIP1_PIN) == SET) ? 1 : 0;
    int dip2 = (gpio_input_bit_get(PIN_DIP2_PORT, PIN_DIP2_PIN) == SET) ? 1 : 0;
    int dip3 = (gpio_input_bit_get(PIN_DIP3_PORT, PIN_DIP3_PIN) == SET) ? 1 : 0;
    int dip4 = (gpio_input_bit_get(PIN_DIP4_PORT, PIN_DIP4_PIN) == SET) ? 1 : 0;
    int pb7  = (gpio_input_bit_get(PIN_STRAP_PB7_PORT,  PIN_STRAP_PB7_PIN)  == SET) ? 1 : 0;
    int pb8  = (gpio_input_bit_get(PIN_STRAP_PB8_PORT,  PIN_STRAP_PB8_PIN)  == SET) ? 1 : 0;
    int pe2  = (gpio_input_bit_get(PIN_STRAP_PE2_PORT,  PIN_STRAP_PE2_PIN)  == SET) ? 1 : 0;
    int pb14 = (gpio_input_bit_get(PIN_STRAP_PB14_PORT, PIN_STRAP_PB14_PIN) == SET) ? 1 : 0;
    printk("straps: dip=%d%d%d%d pb7=%d pb8=%d pe2=%d pb14=%d\n",
           dip1, dip2, dip3, dip4, pb7, pb8, pe2, pb14);
}

int gpio_dip4_held(void)
{
    /* Active-low: 0 = held to GND (slid to "ON"), 1 = open (pull-up). */
    return (gpio_input_bit_get(PIN_DIP4_PORT, PIN_DIP4_PIN) == SET) ? 0 : 1;
}

/* boards/nexcyber/hal/gpio.c — M2 GPIO bring-up for the N32G45x.
 *
 * Idempotent one-shot config for every confirmed pin in
 * boards/nexcyber/pin_map.h. Same surface as rippleon's src/hal/gpio.c
 * (`gpio_init_all` + `gpio_log_straps`) so board-shared main paths can
 * call them without #ifdefs.
 *
 * Note: USART1 pads (PA9 TX / PA10 RX) are owned by uart_init() — this
 * file doesn't touch them. The USART pad init in uart.c runs before
 * gpio_init_all() so the log channel is up before this code can
 * printk anything.
 *
 * Pins NOT touched here, deliberately:
 *   USART2 (PA2/PA3), USART3 (PB10/PB11), SPI2 (PB12-15) — owned by
 *     their peripheral drivers (Nextion / debug-or-BL0939 / BL0939),
 *     all bench-blocked.
 *   PA0, PA1, PA15, PB8, PB9, PC8, PC10 — TBD OUT_PP. Don't drive
 *     unknown loads. Reset default (input floating) is safe.
 *   PC2, PC12 — AF_PP with unidentified remap target. Skip until
 *     AFIO_MAPR is decoded on a bench trace.
 *   Ports D/E/F/G — entirely unused in stock firmware.
 */

#include "hal/gpio.h"
#include "hal/uart.h"
#include "pin_map.h"
#include "n32g45x.h"

static void clock_enable_all(void)
{
    /* GPIOA already enabled by uart_init(); GPIOB+C+AFIO need clocks
     * before this function configures anything on those ports. Calling
     * RCC_EnableAPB2PeriphClk on an already-enabled bit is a no-op. */
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO
                            | RCC_APB2_PERIPH_GPIOA
                            | RCC_APB2_PERIPH_GPIOB
                            | RCC_APB2_PERIPH_GPIOC,
                            ENABLE);
}

static void cfg_pin(GPIO_Module *port, uint16_t pin,
                    GPIO_ModeType mode, GPIO_SpeedType speed)
{
    GPIO_InitType io = {0};
    io.Pin        = pin;
    io.GPIO_Mode  = mode;
    io.GPIO_Speed = speed;
    GPIO_InitPeripheral(port, &io);
}

static void init_outputs_safe_state(void)
{
    /* Drive each output to its bench-safe state BEFORE configuring as
     * OUT_PP, so the pin can't glitch through the wrong level during
     * the F1-style mode change. Mirrors rippleon's src/hal/gpio.c
     * init_outputs_safe_low() pattern.
     *
     * - PC6 buzzer:        LOW (no tone)
     * - PB0 GFCI CAL:      LOW (no test pulse — relay coil de-energised)
     * - PC11 safety-loop-en: LOW at boot. The pin is "safety loop is
     *   wired through" gating from the stock fw — driving it HIGH
     *   before we've validated the loop's continuity would assert a
     *   downstream enable (candidate consumers: GFCI module, gun-lock
     *   relay, peripheral 12V rail) on potentially-broken hardware.
     *   Safety task will raise it once safety_state == OK in M5+. */
    GPIO_ResetBits(PIN_BUZZER_PORT,           PIN_BUZZER_PIN);
    GPIO_ResetBits(PIN_GFCI_CAL_PORT,         PIN_GFCI_CAL_PIN);
    GPIO_ResetBits(PIN_SAFETY_LOOP_EN_PORT,   PIN_SAFETY_LOOP_EN_PIN);

    cfg_pin(PIN_BUZZER_PORT,         PIN_BUZZER_PIN,         GPIO_Mode_Out_PP, GPIO_Speed_2MHz);
    cfg_pin(PIN_GFCI_CAL_PORT,       PIN_GFCI_CAL_PIN,       GPIO_Mode_Out_PP, GPIO_Speed_2MHz);
    cfg_pin(PIN_SAFETY_LOOP_EN_PORT, PIN_SAFETY_LOOP_EN_PIN, GPIO_Mode_Out_PP, GPIO_Speed_2MHz);
}

static void init_cp_pwm_pad(void)
{
    /* PA8 = TIM1_CH1 default AF. Configure pad as AF_PP @ 50 MHz so
     * it's ready for TIM1 to drive when M3 lands cp_pwm_init(). With
     * TIM1 disabled the pad sits high-impedance on AF in F1-style
     * register model. */
    cfg_pin(PIN_CP_PWM_PORT, PIN_CP_PWM_PIN, GPIO_Mode_AF_PP, GPIO_Speed_50MHz);
}

static void init_inputs_pullup(void)
{
    /* PA11/PA12 capacitive touch buttons — TTP223-style, idle HIGH,
     * pulled LOW briefly on a tap. IN_PU keeps a defined level if the
     * touch IC's pull becomes weak.
     *
     * PC13 STOP loop sense — NC E-stop switch in series, active-LOW.
     * HIGH = switch closed (button not pressed) = safe to run.
     * LOW  = switch open (E-stop pressed OR wire pulled) = halt.
     * IPU so a disconnected wire reads LOW (= halt) rather than
     * floating into an indeterminate "OK" state — fail-safe direction. */
    cfg_pin(PIN_BTN_TOUCH1_PORT,  PIN_BTN_TOUCH1_PIN,  GPIO_Mode_IPU, GPIO_Speed_50MHz);
    cfg_pin(PIN_BTN_TOUCH2_PORT,  PIN_BTN_TOUCH2_PIN,  GPIO_Mode_IPU, GPIO_Speed_50MHz);
    cfg_pin(PIN_STOP_SENSE_PORT,  PIN_STOP_SENSE_PIN,  GPIO_Mode_IPU, GPIO_Speed_50MHz);
}

static void init_inputs_floating(void)
{
    /* PC3 — mains-detect candidate A (TLP293-2 photocoupler output?).
     * Static decode showed IN_FLOATING in stock fw; replicate. */
    cfg_pin(PIN_MAINS_DETECT_A_PORT, PIN_MAINS_DETECT_A_PIN,
            GPIO_Mode_IN_FLOATING, GPIO_Speed_50MHz);
}

static void init_inputs_pulldown(void)
{
    /* PC7 / PC9 — mains-detect candidates B/C. IN_PD per static decode.
     * 60 Hz pulse stream expected when mains live. */
    cfg_pin(PIN_MAINS_DETECT_B_PORT, PIN_MAINS_DETECT_B_PIN,
            GPIO_Mode_IPD, GPIO_Speed_50MHz);
    cfg_pin(PIN_MAINS_DETECT_C_PORT, PIN_MAINS_DETECT_C_PIN,
            GPIO_Mode_IPD, GPIO_Speed_50MHz);
}

static void init_analog_inputs(void)
{
    /* Five ADC channels confirmed configured AIN in stock fw. Channel-
     * to-role mapping is bench-blocked (J1772 state walk + ZMPT107
     * scope correlation); configuring as AIN here just opens the pad
     * so the ADC HAL can sample them in M3. */
    cfg_pin(PIN_ADC_VSENSE_L1_PORT, PIN_ADC_VSENSE_L1_PIN, GPIO_Mode_AIN, GPIO_Speed_2MHz);
    cfg_pin(PIN_ADC_VSENSE_L2_PORT, PIN_ADC_VSENSE_L2_PIN, GPIO_Mode_AIN, GPIO_Speed_2MHz);
    cfg_pin(PIN_ADC_CP_PORT,        PIN_ADC_CP_PIN,        GPIO_Mode_AIN, GPIO_Speed_2MHz);
    cfg_pin(PIN_ADC_CC_PORT,        PIN_ADC_CC_PIN,        GPIO_Mode_AIN, GPIO_Speed_2MHz);
    cfg_pin(PIN_ADC_NTC_PORT,       PIN_ADC_NTC_PIN,       GPIO_Mode_AIN, GPIO_Speed_2MHz);
}

void gpio_init_all(void)
{
    clock_enable_all();
    init_outputs_safe_state();
    init_cp_pwm_pad();
    init_inputs_pullup();
    init_inputs_floating();
    init_inputs_pulldown();
    init_analog_inputs();
}

void gpio_log_straps(void)
{
    /* One-line boot snapshot of the digital inputs we can interpret.
     * Bench scope correlation key (mains on, no plug, STOP not pressed):
     *   - btn1/btn2 = 1 (touch panels untouched, IPU)
     *   - stop = 1 (NC switch closed; LOW = E-stop pressed / loop broken)
     *   - mains_a/b/c TBD — likely 60 Hz toggle on at least one of them
     *     when AC is up
     */
    int btn1 = GPIO_ReadInputDataBit(PIN_BTN_TOUCH1_PORT,     PIN_BTN_TOUCH1_PIN)     ? 1 : 0;
    int btn2 = GPIO_ReadInputDataBit(PIN_BTN_TOUCH2_PORT,     PIN_BTN_TOUCH2_PIN)     ? 1 : 0;
    int stop = GPIO_ReadInputDataBit(PIN_STOP_SENSE_PORT,     PIN_STOP_SENSE_PIN)     ? 1 : 0;
    int ma   = GPIO_ReadInputDataBit(PIN_MAINS_DETECT_A_PORT, PIN_MAINS_DETECT_A_PIN) ? 1 : 0;
    int mb   = GPIO_ReadInputDataBit(PIN_MAINS_DETECT_B_PORT, PIN_MAINS_DETECT_B_PIN) ? 1 : 0;
    int mc   = GPIO_ReadInputDataBit(PIN_MAINS_DETECT_C_PORT, PIN_MAINS_DETECT_C_PIN) ? 1 : 0;
    printk("straps: btn=%d%d stop=%d mains=%d%d%d\n",
           btn1, btn2, stop, ma, mb, mc);
}

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
#include "adc_scan_nx.h"
#include "hal/cp_pwm.h"
#include "spi2.h"
#include "hal/bl0939.h"
#include "nextion.h"
#include "relay_nx.h"
#include "gfci_nx.h"
#include "led_ring.h"
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
 *   5 = nextion dim=10 (dim backlight — unambiguous USART2 link test)
 *   6 = nextion dim=100 (restore backlight)
 *   7 = nextion `page 0`
 *   8 = nextion `page 1`
 *   9 = nextion `page 2`
 *   10 = nextion `page 3`
 *   11 = probe USART3/9600 on PB10 — send "dim=10" then "dim=100"
 *   12 = probe UART4/9600 on PC10 — send "dim=10" then "dim=100"
 *        (PC10 doubles as blue LED; this temporarily steals the pad
 *         then restores it as OUT_PP so blue LED works again)
 *   13 = probe USART2/115200 on PA2 — re-init at 115200 + dim test
 * Monitor task polls this every 100 ms and ACKs by zeroing it.
 *
 * `volatile` + the global symbol survives in the .map so the address
 * is easy to find with `nm openevcharger.elf | grep g_bench_cmd`. */
volatile uint32_t g_bench_cmd = 0;
/* Side-channel argument for bench cmds that need a parameter.
 * Currently used by cmd 18 (DGUS test with arbitrary BRR). */
volatile uint32_t g_bench_arg = 0;

/* Tear down USART2 + DMA1_CH6 so re-probing a different UART won't
 * clash with the nextion HAL we already brought up at boot. We don't
 * fully de-init — just drop ENABLE bits so the peripherals go quiet. */
static void nextion_park_usart2(void)
{
    USART_EnableDMA(USART2, USART_DMAREQ_RX, DISABLE);
    USART_Enable(USART2, DISABLE);
    DMA_EnableChannel(DMA1_CH6, DISABLE);
}

/* Send "dim=10\xFF\xFF\xFF", delay, then "dim=100\xFF\xFF\xFF" out a
 * caller-configured UART. Generic enough that the per-cmd helpers
 * below just wire up clocks + GPIO AF and call this. */
static void uart_send_blocking(USART_Module *uart, const char *s)
{
    while (*s) {
        while (USART_GetFlagStatus(uart, USART_FLAG_TXDE) == RESET) { }
        USART_SendData(uart, (uint16_t)(uint8_t)*s++);
    }
    for (int i = 0; i < 3; ++i) {
        while (USART_GetFlagStatus(uart, USART_FLAG_TXDE) == RESET) { }
        USART_SendData(uart, 0xFF);
    }
    /* Wait for the final byte to drain off the wire before returning,
     * so a subsequent peripheral reconfiguration doesn't truncate it. */
    while (USART_GetFlagStatus(uart, USART_FLAG_TXC) == RESET) { }
}

static void probe_send_dim_cycle(USART_Module *uart,
                                 void (*delay_ms)(uint32_t))
{
    uart_send_blocking(uart, "dim=10");
    if (delay_ms) delay_ms(1500);
    uart_send_blocking(uart, "dim=100");
}

static void probe_usart3_pb10_9600(void)
{
    nextion_park_usart2();
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART3, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOB | RCC_APB2_PERIPH_AFIO, ENABLE);
    GPIO_InitType io = {0};
    io.Pin = GPIO_PIN_10;
    io.GPIO_Mode = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOB, &io);
    USART_InitType u = {0};
    u.BaudRate = 9600;
    u.WordLength = USART_WL_8B;
    u.StopBits = USART_STPB_1;
    u.Parity = USART_PE_NO;
    u.Mode = USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(USART3, &u);
    USART_Enable(USART3, ENABLE);
    probe_send_dim_cycle(USART3, task_delay_ms);
    USART_Enable(USART3, DISABLE);
}

static void probe_uart4_pc10_9600(void)
{
    nextion_park_usart2();
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_UART4, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOC | RCC_APB2_PERIPH_AFIO, ENABLE);
    /* PC10 is currently OUT_PP for blue LED. Re-cfg as AF_PP so UART4
     * controls it. Saves prior LED state so we can restore after. */
    bool prev_blue = (GPIO_ReadOutputDataBit(PIN_LED_BLUE_PORT,
                                             PIN_LED_BLUE_PIN) != 0);
    GPIO_InitType io = {0};
    io.Pin = GPIO_PIN_10;
    io.GPIO_Mode = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOC, &io);
    USART_InitType u = {0};
    u.BaudRate = 9600;
    u.WordLength = USART_WL_8B;
    u.StopBits = USART_STPB_1;
    u.Parity = USART_PE_NO;
    u.Mode = USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(UART4, &u);
    USART_Enable(UART4, ENABLE);
    probe_send_dim_cycle(UART4, task_delay_ms);
    USART_Enable(UART4, DISABLE);
    /* Restore PC10 as OUT_PP for blue LED. */
    io.Pin = GPIO_PIN_10;
    io.GPIO_Mode = GPIO_Mode_Out_PP;
    io.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitPeripheral(GPIOC, &io);
    if (prev_blue) {
        GPIO_SetBits(PIN_LED_BLUE_PORT, PIN_LED_BLUE_PIN);
    } else {
        GPIO_ResetBits(PIN_LED_BLUE_PORT, PIN_LED_BLUE_PIN);
    }
}

/* USART3 with partial remap to PC10/PC11 — stock-fw most-referenced
 * UART (8 base-addr hits) and PC10 doubles as our blue LED pad. If the
 * LCD lives here, this probe should dim its backlight. */
static void probe_usart3_pc10_remap_9600(void)
{
    nextion_park_usart2();
    /* Save blue LED ODR so we can restore. */
    bool prev_blue = (GPIO_ReadOutputDataBit(PIN_LED_BLUE_PORT,
                                             PIN_LED_BLUE_PIN) != 0);

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART3, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOC | RCC_APB2_PERIPH_AFIO, ENABLE);

    /* AFIO_MAPR: set USART3_REMAP = 01 (partial remap → PC10/PC11). */
    GPIO_ConfigPinRemap(GPIO_PART_RMP_USART3, ENABLE);

    GPIO_InitType io = {0};
    io.Pin = GPIO_PIN_10;
    io.GPIO_Mode = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOC, &io);

    USART_InitType u = {0};
    u.BaudRate = 9600;
    u.WordLength = USART_WL_8B;
    u.StopBits = USART_STPB_1;
    u.Parity = USART_PE_NO;
    u.Mode = USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(USART3, &u);
    USART_Enable(USART3, ENABLE);
    probe_send_dim_cycle(USART3, task_delay_ms);
    USART_Enable(USART3, DISABLE);

    /* Clear the remap so PC10 is plain GPIO again. */
    GPIO_ConfigPinRemap(GPIO_PART_RMP_USART3, DISABLE);

    /* Restore PC10 as OUT_PP for blue LED. */
    io.Pin = GPIO_PIN_10;
    io.GPIO_Mode = GPIO_Mode_Out_PP;
    io.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitPeripheral(GPIOC, &io);
    if (prev_blue) {
        GPIO_SetBits(PIN_LED_BLUE_PORT, PIN_LED_BLUE_PIN);
    } else {
        GPIO_ResetBits(PIN_LED_BLUE_PORT, PIN_LED_BLUE_PIN);
    }
}

static void probe_usart2_115200(void)
{
    nextion_park_usart2();
    USART_InitType u = {0};
    u.BaudRate = 115200;
    u.WordLength = USART_WL_8B;
    u.StopBits = USART_STPB_1;
    u.Parity = USART_PE_NO;
    u.Mode = USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(USART2, &u);
    USART_Enable(USART2, ENABLE);
    probe_send_dim_cycle(USART2, task_delay_ms);
    USART_Enable(USART2, DISABLE);
}

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
    case 5:
        printk("bench: nextion dims=10 (backlight dim — Tuya variant)\n");
        nextion_send_cmd("dims=10");
        break;
    case 6:
        printk("bench: nextion dims=100 (backlight restore)\n");
        nextion_send_cmd("dims=100");
        break;
    case 7:
        printk("bench: nextion page 0\n");
        nextion_send_cmd("page 0");
        break;
    case 8:
        printk("bench: nextion page 1\n");
        nextion_send_cmd("page 1");
        break;
    case 9:
        printk("bench: nextion page 2\n");
        nextion_send_cmd("page 2");
        break;
    case 10:
        printk("bench: nextion page 3\n");
        nextion_send_cmd("page 3");
        break;
    case 11:
        printk("bench: probe USART3/9600 on PB10 (dim=10 / dim=100)\n");
        probe_usart3_pb10_9600();
        break;
    case 12:
        printk("bench: probe UART4/9600 on PC10 (dim=10 / dim=100)\n");
        probe_uart4_pc10_9600();
        break;
    case 13:
        printk("bench: probe USART2/115200 (dim=10 / dim=100)\n");
        probe_usart2_115200();
        break;
    case 14:
        printk("bench: probe USART3 partial-remap (PC10)/9600 (dims=10/100)\n");
        probe_usart3_pc10_remap_9600();
        break;
    case 16: {
        /* Send DGUS backlight=0 frame via USART2 at 115200 baud,
         * bytes back-to-back. Stock fw uses USART2 = 115200 = LCD. */
        printk("bench: DGUS dim=0 frame on USART2 @ 115200 (BRR=0x0138)\n");
        /* Re-init USART2 at 115200 baud (BRR for 36 MHz PCLK1). */
        USART_Enable(USART2, DISABLE);
        USART_InitType u = {0};
        u.BaudRate = 115200;
        u.WordLength = USART_WL_8B;
        u.StopBits = USART_STPB_1;
        u.Parity = USART_PE_NO;
        u.Mode = USART_MODE_TX;
        u.HardwareFlowControl = USART_HFCTRL_NONE;
        USART_Init(USART2, &u);
        USART_Enable(USART2, ENABLE);
        static const uint8_t frame_off[] = {0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x00};
        uart_send_blocking(USART2, "");  /* prime TXDE */
        for (unsigned i = 0; i < sizeof(frame_off); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, frame_off[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        task_delay_ms(1500);
        static const uint8_t frame_on[] = {0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x64};
        for (unsigned i = 0; i < sizeof(frame_on); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, frame_on[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        printk("bench: DGUS frames sent\n");
        break;
    }
    case 17: {
        /* Same DGUS dim cycle but at PAGE CHANGE — more visible. */
        printk("bench: DGUS page 0 → page 1 on USART2 @ 115200\n");
        USART_Enable(USART2, DISABLE);
        USART_InitType u = {0};
        u.BaudRate = 115200;
        u.WordLength = USART_WL_8B;
        u.StopBits = USART_STPB_1;
        u.Parity = USART_PE_NO;
        u.Mode = USART_MODE_TX;
        u.HardwareFlowControl = USART_HFCTRL_NONE;
        USART_Init(USART2, &u);
        USART_Enable(USART2, ENABLE);
        static const uint8_t page0[] = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x00};
        static const uint8_t page1[] = {0x5A,0xA5,0x07,0x82,0x00,0x84,0x5A,0x01,0x00,0x01};
        for (unsigned i = 0; i < sizeof(page0); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, page0[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        task_delay_ms(2000);
        for (unsigned i = 0; i < sizeof(page1); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, page1[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        printk("bench: DGUS page frames sent\n");
        break;
    }
    case 18: {
        /* Re-init USART2 with arbitrary BRR from g_bench_arg, then send
         * DGUS backlight=0 + 1.5s + backlight=100. Use this to sweep BRR
         * values fast via SWD pokes. */
        uint32_t brr = g_bench_arg;
        printk("bench: DGUS test with BRR=%#x\n", (unsigned)brr);
        USART_Enable(USART2, DISABLE);
        /* Manual register pokes — bypass USART_Init's BRR computation. */
        USART2->CTRL1 = 0;                  /* clear, then reconfigure */
        USART2->BRCF = brr & 0xFFFF;
        USART2->CTRL2 = 0;                  /* 1 stop bit */
        USART2->CTRL3 = 0;
        USART2->CTRL1 = (1u << 13) | (1u << 3);  /* UE | TE */
        static const uint8_t fr_off[] = {0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x00};
        static const uint8_t fr_on [] = {0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x64};
        for (unsigned i = 0; i < sizeof(fr_off); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, fr_off[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        task_delay_ms(1500);
        for (unsigned i = 0; i < sizeof(fr_on); ++i) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, fr_on[i]);
        }
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) { }
        break;
    }
    case 20: {
        /* Built from DWIN T5L_DGUSII Application Development Guide v2.9:
         *   - System_Reset at VP=0x0004: write magic 55AA 5AA5 → LCD CPU
         *     hard reset (works regardless of LCD CFG's CRC setting,
         *     since CRC-mode LCDs see the trailing CRC as extra data
         *     beyond the first 4 reset bytes — first 4 still trigger).
         *   - Backlight OFF/ON at VP=0x0082 → visible LCD-alive proof.
         *   - Page 0 at VP=0x0084 → display content change.
         *
         * CRC mode of the LCD is set by SD-card CFG byte 0x05 bit 7;
         * we can't know it from outside. Tests both modes:
         *   1) System_Reset (CRC variant — universal)
         *   2) Backlight blink (no-CRC, for stock-mode LCDs)
         *   3) Backlight blink (CRC, for CRC-mode LCDs)
         *   4) Page 0 (no-CRC, then CRC)
         *
         * If LCD backlight flickers AT ALL during this sequence, the
         * LCD is alive — the step that produced it reveals the mode.
         * CRC = MODBUS-16 (poly 0xA001, init 0xFFFF) over CMD+VP+DATA,
         * trailer LSB-first. Verified against doc examples 2026-05-12. */
        uint32_t brr = g_bench_arg ? g_bench_arg : 0x138;
        printk("bench: DGUS universal test (docs-driven), BRR=%#x\n", (unsigned)brr);
        USART_Enable(USART2, DISABLE);
        USART2->CTRL1 = 0;
        USART2->BRCF = brr & 0xFFFF;
        USART2->CTRL2 = 0;
        USART2->CTRL3 = 0;
        USART2->CTRL1 = (1u << 13) | (1u << 3);  /* UE | TE */

        /* Pre-computed frames (CRC trailers verified against doc) */
        static const uint8_t fr_reset_crc[]   = {0x5A,0xA5,0x09,0x82,0x00,0x04,
                                                  0x55,0xAA,0x5A,0xA5,0x83,0xFF};
        static const uint8_t fr_bl_off[]      = {0x5A,0xA5,0x04,0x82,0x00,0x82,0x00};
        static const uint8_t fr_bl_on[]       = {0x5A,0xA5,0x04,0x82,0x00,0x82,0x64};
        static const uint8_t fr_bl_off_crc[]  = {0x5A,0xA5,0x06,0x82,0x00,0x82,
                                                  0x00,0x48,0xFC};
        static const uint8_t fr_bl_on_crc[]   = {0x5A,0xA5,0x06,0x82,0x00,0x82,
                                                  0x64,0x49,0x17};
        static const uint8_t fr_page0[]       = {0x5A,0xA5,0x07,0x82,0x00,0x84,
                                                  0x5A,0x01,0x00,0x00};
        static const uint8_t fr_page0_crc[]   = {0x5A,0xA5,0x09,0x82,0x00,0x84,
                                                  0x5A,0x01,0x00,0x00,0x0A,0x0E};

        #define SEND_FRAME(buf) do { \
            for (unsigned _i = 0; _i < sizeof(buf); ++_i) { \
                while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) {} \
                USART_SendData(USART2, (buf)[_i]); \
            } \
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) {} \
        } while (0)

        printk("  step 1: System_Reset (CRC-variant — universal)\n");
        SEND_FRAME(fr_reset_crc);
        task_delay_ms(3000);    /* LCD CPU reboot window */

        printk("  step 2: Backlight blink (no-CRC mode)\n");
        SEND_FRAME(fr_bl_off);
        task_delay_ms(600);
        SEND_FRAME(fr_bl_on);
        task_delay_ms(800);

        printk("  step 3: Backlight blink (CRC mode)\n");
        SEND_FRAME(fr_bl_off_crc);
        task_delay_ms(600);
        SEND_FRAME(fr_bl_on_crc);
        task_delay_ms(800);

        printk("  step 4: Page 0 (no-CRC then CRC)\n");
        SEND_FRAME(fr_page0);
        task_delay_ms(500);
        SEND_FRAME(fr_page0_crc);

        printk("bench: DGUS universal test done\n");
        #undef SEND_FRAME
        break;
    }
    case 19: {
        /* Earlier attempt — VP=0x0000 zero-writes per stock-fw analysis.
         * Superseded by cmd 20 (docs-driven, universal CRC handling).
         * Kept for reference. */
        uint32_t brr = g_bench_arg ? g_bench_arg : 0x138;
        printk("bench: DGUS full init sequence, BRR=%#x\n", (unsigned)brr);
        USART_Enable(USART2, DISABLE);
        USART2->CTRL1 = 0;
        USART2->BRCF = brr & 0xFFFF;
        USART2->CTRL2 = 0;
        USART2->CTRL3 = 0;
        USART2->CTRL1 = (1u << 13) | (1u << 3);  /* UE | TE */
        static const uint8_t fr_init2[]  = {0x5A,0xA5,0x05,0x82,0x00,0x00,0x00,0x00};
        static const uint8_t fr_init12[] = {0x5A,0xA5,0x0F,0x82,
                                            0x00,0x00,0x00,0x00,0x00,0x00,
                                            0x00,0x00,0x00,0x00,0x00,0x00,
                                            0x00,0x00};
        static const uint8_t fr_bl_off[] = {0x5A,0xA5,0x04,0x82,0x00,0x82,0x00};
        static const uint8_t fr_bl_on[]  = {0x5A,0xA5,0x04,0x82,0x00,0x82,0x64};
        static const uint8_t fr_page0[]  = {0x5A,0xA5,0x07,0x82,0x00,0x84,
                                            0x5A,0x01,0x00,0x00};
        #define SEND_FRAME(buf) do { \
            for (unsigned _i = 0; _i < sizeof(buf); ++_i) { \
                while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) {} \
                USART_SendData(USART2, (buf)[_i]); \
            } \
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) {} \
        } while (0)

        SEND_FRAME(fr_init2);
        task_delay_ms(100);
        SEND_FRAME(fr_init12);
        task_delay_ms(200);
        SEND_FRAME(fr_bl_off);
        task_delay_ms(500);
        SEND_FRAME(fr_bl_on);
        task_delay_ms(100);
        SEND_FRAME(fr_page0);
        printk("bench: DGUS init sequence done\n");
        #undef SEND_FRAME
        break;
    }
    case 15: {
        /* Continuous spam 0x55 to USART2 for ~10 seconds. Measures
         * with a multimeter: PA2 should average ~1.65 V (50% duty).
         * If LCD-RX pad also averages ~1.65 V, the trace is good.
         * If LCD-RX stays at 3.3 V or 0 V, the wiggle test pointed
         * at the wrong pad. */
        printk("bench: spam 0x55 on USART2 for 10s @ current BRR\n");
        uint32_t start = xTaskGetTickCount();
        uint32_t hz = configTICK_RATE_HZ;
        while ((xTaskGetTickCount() - start) < (10u * hz)) {
            while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
            USART_SendData(USART2, 0x55);
        }
        printk("bench: spam done\n");
        break;
    }
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

        /* On committed J1772 state transition, log + flip Nextion page +
         * update the LED ring (blue = standby/waiting, green = charging,
         * both off in fault states E/F since red is hardware-damaged on
         * this bench unit so we can't communicate fault visually). */
        if (committed != prev_committed && committed != J1772_STATE_INVALID) {
            printk("J1772: state %s -> %s (cp=%d mV)\n",
                   j1772_state_name(prev_committed),
                   j1772_state_name(committed),
                   (int)cp_mv);
            const char *page = page_for_state(committed);
            if (page) {
                nextion_send_cmd(page);
            }
            switch (committed) {
            case J1772_STATE_A:   /* no gun */
            case J1772_STATE_B:   /* plugged, not charging */
                led_blue_set(true);
                led_green_set(false);
                break;
            case J1772_STATE_C:   /* charging */
            case J1772_STATE_D:   /* charging, vent required */
                led_blue_set(false);
                led_green_set(true);
                break;
            case J1772_STATE_E:   /* CP shorted */
            case J1772_STATE_F:   /* CP -12V (fault) */
            default:
                led_blue_set(false);
                led_green_set(false);
                break;
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
    led_ring_init();
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

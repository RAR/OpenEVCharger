#include "uart5.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

/* Bench debug 2026-05-03: xStreamBufferSendFromISR was triggering a
 * boot loop on the very first byte received. Replaced with a tiny
 * lockless ring buffer (single-producer ISR / single-consumer
 * comms_task) until the FreeRTOS-side issue is understood. */
#define UART5_RX_RING_LEN  256u   /* power of 2 */
#define UART5_RX_RING_MASK (UART5_RX_RING_LEN - 1u)
static volatile uint8_t  s_rx_ring[UART5_RX_RING_LEN];
static volatile uint16_t s_rx_head;   /* ISR writes here */
static volatile uint16_t s_rx_tail;   /* task reads here */

size_t uart5_rx_pop(uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (n < cap) {
        uint16_t h = s_rx_head;
        uint16_t t = s_rx_tail;
        if (h == t) break;            /* empty */
        out[n++] = s_rx_ring[t & UART5_RX_RING_MASK];
        s_rx_tail = (t + 1u) & UART5_RX_RING_MASK;
    }
    return n;
}

void uart5_init(void)
{

    rcu_periph_clock_enable(PIN_UART4_TX_RCU);
    rcu_periph_clock_enable(PIN_UART4_RX_RCU);
    rcu_periph_clock_enable(RCU_UART4);

    /* PC12 = UART4_TX (AF PP, 50 MHz). PD2 = UART4_RX (input float). */
    gpio_init(PIN_UART4_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
              PIN_UART4_TX_PIN);
    gpio_init(PIN_UART4_RX_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ,
              PIN_UART4_RX_PIN);

    usart_deinit(UART4);
    usart_baudrate_set(UART4, 115200U);
    usart_word_length_set(UART4, USART_WL_8BIT);
    usart_stop_bit_set(UART4, USART_STB_1BIT);
    usart_parity_config(UART4, USART_PM_NONE);
    usart_hardware_flow_rts_config(UART4, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(UART4, USART_CTS_DISABLE);
    usart_receive_config(UART4, USART_RECEIVE_ENABLE);
    usart_transmit_config(UART4, USART_TRANSMIT_ENABLE);
    usart_enable(UART4);

    /* RX-not-empty interrupt. ISR pushes into a lockless ring buffer
     * (no FreeRTOS API called from ISR) so priority is unconstrained
     * by configMAX_SYSCALL_INTERRUPT_PRIORITY. We pick 6 — same urgency
     * tier as adc_inject. */
    usart_interrupt_enable(UART4, USART_INT_RBNE);
    nvic_irq_enable(UART4_IRQn, 6U, 0U);
}

size_t uart5_send(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; ++i) {
        while (RESET == usart_flag_get(UART4, USART_FLAG_TBE)) { }
        usart_data_transmit(UART4, p[i]);
    }
    /* Wait for the last byte to fully shift out so a back-to-back
     * send doesn't interleave. */
    while (RESET == usart_flag_get(UART4, USART_FLAG_TC)) { }
    return len;
}

void UART4_IRQHandler(void)
{
    /* GD32 USART error flags (ORERR/FERR/NERR/PERR) are cleared by
     * reading STAT0 followed by DATA. If we only read DATA (as the
     * vendor `usart_data_receive` macro does), ORERR stays set and
     * the IRQ keeps re-firing forever — the line is "always RX" from
     * the controller's perspective. Bench 2026-05-03: with the FC41D
     * streaming continuous bytes, this IRQ storm starved every task
     * including safety, wdg_kick never ran, IWDG fired at 1 s, and
     * the chip reset → repeat → visible "very short flashes" on the
     * heartbeat LED.
     *
     * Fix: latch STAT0 first (read-clears ORERR/FERR/NERR/PERR when
     * paired with the DATA read), always drain DATA when RBNE or any
     * error bit is set, and only push the byte to the stream buffer
     * if it arrived cleanly. */
    uint32_t stat = USART_STAT0(UART4);
    const uint32_t errs = USART_STAT0_ORERR | USART_STAT0_FERR |
                          USART_STAT0_NERR  | USART_STAT0_PERR;

    if (stat & (USART_STAT0_RBNE | errs)) {
        uint8_t c = (uint8_t)USART_DATA(UART4);   /* clears RBNE + errs */
        if ((stat & USART_STAT0_RBNE) && !(stat & errs)) {
            uint16_t h = s_rx_head;
            uint16_t next = (h + 1u) & UART5_RX_RING_MASK;
            if (next != s_rx_tail) {  /* drop on full */
                s_rx_ring[h & UART5_RX_RING_MASK] = c;
                s_rx_head = next;
            }
        }
        (void)c;
    }
}

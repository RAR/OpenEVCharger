#include "rfid.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

/* Lockless RX ring — same pattern as uart5.c. ISR is single-producer
 * (USART1_IRQHandler), the safety-task-side rfid step is single
 * consumer. The 11-byte longest frame plus a few-frame backlog fits
 * comfortably in 64 B. */
#define RFID_RX_RING_LEN  64u
#define RFID_RX_RING_MASK (RFID_RX_RING_LEN - 1u)
static volatile uint8_t  s_rx_ring[RFID_RX_RING_LEN];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

/* Keepalive frame literal — matches stock fw V1.0.066 buffer at
 * flash 0x08024604. */
static const uint8_t s_keepalive[7] = {
    0xAA, 0xD0, 0xD1, 0x01, 0x00, 0x00, 0x4C
};

void rfid_init(void)
{
    rcu_periph_clock_enable(PIN_RFID_RCU);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_USART1);

    /* USART1 remap: route TX/RX from default PA2/PA3 to PD5/PD6. */
    gpio_pin_remap_config(GPIO_USART1_REMAP, ENABLE);

    gpio_init(PIN_RFID_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
              PIN_RFID_TX_PIN);
    gpio_init(PIN_RFID_RX_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ,
              PIN_RFID_RX_PIN);

    usart_deinit(USART1);
    usart_baudrate_set(USART1, 115200U);
    usart_word_length_set(USART1, USART_WL_8BIT);
    usart_stop_bit_set(USART1, USART_STB_1BIT);
    usart_parity_config(USART1, USART_PM_NONE);
    usart_hardware_flow_rts_config(USART1, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(USART1, USART_CTS_DISABLE);
    usart_receive_config(USART1, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART1, USART_TRANSMIT_ENABLE);
    usart_enable(USART1);

    /* RBNE IRQ; same priority tier as uart5 (= 6). */
    usart_interrupt_enable(USART1, USART_INT_RBNE);
    nvic_irq_enable(USART1_IRQn, 6U, 0U);
}

size_t rfid_rx_pop(uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (n < cap) {
        uint16_t h = s_rx_head;
        uint16_t t = s_rx_tail;
        if (h == t) break;
        out[n++] = s_rx_ring[t & RFID_RX_RING_MASK];
        s_rx_tail = (t + 1u) & RFID_RX_RING_MASK;
    }
    return n;
}

void rfid_send_keepalive(void)
{
    for (size_t i = 0; i < sizeof(s_keepalive); ++i) {
        while (RESET == usart_flag_get(USART1, USART_FLAG_TBE)) { }
        usart_data_transmit(USART1, s_keepalive[i]);
    }
    while (RESET == usart_flag_get(USART1, USART_FLAG_TC)) { }
}

void USART1_IRQHandler(void)
{
    /* Mirror uart5.c: latch STAT0 + DATA together so ORERR/FERR/NERR/
     * PERR are cleared (vendor `usart_data_receive` only clears RBNE
     * → ORE-stuck IRQ storm under any noise). */
    uint32_t stat = USART_STAT0(USART1);
    const uint32_t errs = USART_STAT0_ORERR | USART_STAT0_FERR |
                          USART_STAT0_NERR  | USART_STAT0_PERR;

    if (stat & (USART_STAT0_RBNE | errs)) {
        uint8_t c = (uint8_t)USART_DATA(USART1);
        if ((stat & USART_STAT0_RBNE) && !(stat & errs)) {
            uint16_t h = s_rx_head;
            uint16_t next = (h + 1u) & RFID_RX_RING_MASK;
            if (next != s_rx_tail) {
                s_rx_ring[h & RFID_RX_RING_MASK] = c;
                s_rx_head = next;
            }
        }
        (void)c;
    }
}

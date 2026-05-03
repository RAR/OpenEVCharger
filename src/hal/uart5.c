#include "uart5.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

static StreamBufferHandle_t s_rx_stream = NULL;

void uart5_init(StreamBufferHandle_t rx)
{
    s_rx_stream = rx;

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

    /* RX-not-empty interrupt. Priority 5 = configMAX_SYSCALL_INTERRUPT
     * (matches adc_inject ISR). Allows xStreamBufferSendFromISR. */
    usart_interrupt_enable(UART4, USART_INT_RBNE);
    nvic_irq_enable(UART4_IRQn, 5U, 0U);
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
    if (RESET != usart_interrupt_flag_get(UART4, USART_INT_FLAG_RBNE)) {
        uint8_t c = (uint8_t)usart_data_receive(UART4);
        if (s_rx_stream != NULL) {
            BaseType_t hpw = pdFALSE;
            (void)xStreamBufferSendFromISR(s_rx_stream, &c, 1, &hpw);
            portYIELD_FROM_ISR(hpw);
        }
    }
    /* If the line idles for a long time and noise drives ORE/FE, the
     * read of DATA above clears the flags. No additional handling. */
}

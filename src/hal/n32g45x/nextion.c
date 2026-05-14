/* boards/nexcyber/hal/nextion.c — M3 Nextion HMI raw transport.
 *
 * USART2 init at 9600 8N1 + DMA1 channel 6 RX into a 128-byte
 * circular buffer. Surface in nextion.h is the bare-wire layer;
 * the protocol parser and page-state model land in M5+.
 *
 * Matches the stock-fw layout: USART2 on PA2/PA3, DMA1 ch6 RX,
 * 128-byte ring (the stock fw used a 115 B CNDTR per DMA dump
 * 2026-05-11, but 128 B is the natural power-of-two — minor
 * waste in unused tail). The MCU is the master; touch events
 * arrive asynchronously, hence the always-listening RX path.
 */

#include "nextion.h"
#include "hal/uart.h"
#include "n32g45x.h"
#include <string.h>

#define NEXTION_RX_RING_SIZE 128u

static volatile uint8_t s_rx_ring[NEXTION_RX_RING_SIZE];

/* Tail pointer — incremented as the caller drains bytes. The DMA
 * controller doesn't write to a known "head" we can read directly;
 * instead we compute current head from (RING_SIZE - DMA_CNDTR(CH6))
 * each drain call. */
static volatile uint16_t s_rx_tail;

void nextion_init(void)
{
    /* USART2 sits on APB1. PA2/PA3 + AFIO live on APB2. DMA1 on AHB.
     * All the bus clocks except SPI2 are already on by gpio_init_all;
     * enabling here keeps the function self-contained. */
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART2, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA | RCC_APB2_PERIPH_AFIO, ENABLE);
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_DMA1, ENABLE);

    /* USART2 PA2/PA3 pad modes were left untouched by M2 gpio_init_all
     * (we explicitly skipped USART2 pads — see boards/nexcyber/hal/gpio.c
     * file comment). Configure them as AF_PP / IN_FLOATING here so we
     * own the pins fully at this layer. */
    GPIO_InitType io = {0};
    io.Pin        = GPIO_PIN_2;
    io.GPIO_Mode  = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &io);

    io.Pin        = GPIO_PIN_3;
    io.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &io);

    USART_InitType u = {0};
    u.BaudRate            = 9600;
    u.WordLength          = USART_WL_8B;
    u.StopBits            = USART_STPB_1;
    u.Parity              = USART_PE_NO;
    u.Mode                = USART_MODE_RX | USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(USART2, &u);

    /* DMA1 channel 6 → s_rx_ring, circular, peripheral-to-memory,
     * 8-bit transfers. Matches the stock-fw DMA config (CCR=0x30A1
     * decoded in pin_map.h SRAM cache notes). */
    DMA_DeInit(DMA1_CH6);
    DMA_InitType d;
    DMA_StructInit(&d);
    d.PeriphAddr     = (uint32_t)&USART2->DAT;
    d.MemAddr        = (uint32_t)s_rx_ring;
    d.Direction      = DMA_DIR_PERIPH_SRC;
    d.BufSize        = NEXTION_RX_RING_SIZE;
    d.PeriphInc      = DMA_PERIPH_INC_DISABLE;
    d.DMA_MemoryInc  = DMA_MEM_INC_ENABLE;
    d.PeriphDataSize = DMA_PERIPH_DATA_SIZE_BYTE;
    d.MemDataSize    = DMA_MemoryDataSize_Byte;
    d.CircularMode   = DMA_MODE_CIRCULAR;
    d.Priority       = DMA_PRIORITY_HIGH;
    d.Mem2Mem        = DMA_M2M_DISABLE;
    DMA_Init(DMA1_CH6, &d);
    DMA_EnableChannel(DMA1_CH6, ENABLE);

    /* Route USART2 RX through DMA. The SPL spelling for the DMAReq
     * mask is USART_DMAREQ_RX. */
    USART_EnableDMA(USART2, USART_DMAREQ_RX, ENABLE);

    USART_Enable(USART2, ENABLE);

    s_rx_tail = 0;
}

static void nextion_putc_blocking(uint8_t b)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET) { }
    USART_SendData(USART2, b);
}

void nextion_send_cmd(const char *cmd)
{
    if (!cmd) return;
    while (*cmd) {
        nextion_putc_blocking((uint8_t)*cmd++);
    }
    /* Nextion-mandated 3-byte terminator. */
    nextion_putc_blocking(0xFF);
    nextion_putc_blocking(0xFF);
    nextion_putc_blocking(0xFF);
}

size_t nextion_rx_drain(uint8_t *out, size_t max)
{
    if (!out || max == 0) return 0;

    /* Current head = ring size minus the DMA controller's residual
     * transfer count. CNDTR counts DOWN from the initial BufSize,
     * so head_index = SIZE - CNDTR (mod SIZE for circular). */
    uint32_t cndtr = DMA_GetCurrDataCounter(DMA1_CH6);
    uint16_t head = (uint16_t)((NEXTION_RX_RING_SIZE - cndtr) % NEXTION_RX_RING_SIZE);

    size_t copied = 0;
    while (s_rx_tail != head && copied < max) {
        out[copied++] = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) % NEXTION_RX_RING_SIZE);
    }
    return copied;
}

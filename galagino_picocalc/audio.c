/*
 * audio.c - PWM audio on the PicoCalc speakers (GP26 left, GP27 right)
 *
 * Two DMA channels chained into each other feed the PWM compare register
 * at AUDIO_RATE, paced by a DMA timer. Whenever one finishes, its IRQ
 * refills that buffer while the other one plays.
 */

#include "audio.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"

#define PIN_LEFT   26
#define PIN_RIGHT  27
#define AUDIO_DMA_IRQ  1   // DMA_IRQ_1; the LCD doesn't use DMA IRQs

static audio_fill_fn fill_fn;
static uint slice;
static int timer;
static int dma_ch[2];
// one 32 bit word per sample: channel A duty in the low half, B in the high
static uint32_t buffer[2][AUDIO_SAMPLES];

static void refill(int i) {
  uint16_t tmp[AUDIO_SAMPLES];
  fill_fn(tmp, AUDIO_SAMPLES);
  for(int s=0;s<AUDIO_SAMPLES;s++)
    buffer[i][s] = tmp[s] | ((uint32_t)tmp[s] << 16);
}

static void __isr audio_dma_irq(void) {
  for(int i=0;i<2;i++) {
    if(dma_irqn_get_channel_status(AUDIO_DMA_IRQ, dma_ch[i])) {
      dma_irqn_acknowledge_channel(AUDIO_DMA_IRQ, dma_ch[i]);
      refill(i);
      dma_channel_set_read_addr(dma_ch[i], buffer[i], false);
    }
  }
}

void audio_init(audio_fill_fn fill) {
  fill_fn = fill;

  gpio_set_function(PIN_LEFT, GPIO_FUNC_PWM);
  gpio_set_function(PIN_RIGHT, GPIO_FUNC_PWM);
  slice = pwm_gpio_to_slice_num(PIN_LEFT);   // GP26/27 share slice 5

  pwm_config pc = pwm_get_default_config();
  pwm_config_set_clkdiv(&pc, 1.0f);
  pwm_config_set_wrap(&pc, AUDIO_RANGE - 1); // 300 MHz / 1024 = 293 kHz carrier
  pwm_init(slice, &pc, true);
  pwm_set_both_levels(slice, AUDIO_RANGE/2, AUDIO_RANGE/2);

  // DMA timer: 300 MHz / 12500 = 24 kHz exactly
  timer = dma_claim_unused_timer(true);
  audio_set_rate(AUDIO_RATE);

  refill(0);
  refill(1);

  dma_ch[0] = dma_claim_unused_channel(true);
  dma_ch[1] = dma_claim_unused_channel(true);

  for(int i=1;i>=0;i--) {   // configure the second first; the first starts it all
    dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dma_get_timer_dreq(timer));
    channel_config_set_chain_to(&c, dma_ch[i^1]);
    dma_channel_configure(dma_ch[i], &c, &pwm_hw->slice[slice].cc,
                          buffer[i], AUDIO_SAMPLES, false);
    dma_irqn_set_channel_enabled(AUDIO_DMA_IRQ, dma_ch[i], true);
  }

  irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq);
  irq_set_enabled(DMA_IRQ_1, true);

  dma_channel_start(dma_ch[0]);
}

void audio_set_rate(unsigned rate) {
  // 300 MHz / 25500 = 11764.7 Hz, which is exactly Donkey Kong's rate
  dma_timer_set_fraction(timer, 1, (clock_get_hz(clk_sys) + rate/2) / rate);
}

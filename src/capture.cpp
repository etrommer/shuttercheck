// Capture path: TIM3 (TRGO) + ADC1 + DMA1_Channel1, through the STM32 HAL.
// The whole file is firmware-only: the native unit-test build compiles it as
// an empty translation unit and tests src/scan.cpp directly.
#if defined(ARDUINO)
#include "capture.h"

#include <Arduino.h>
#include <string.h>

#include <stm32f1xx_hal.h>

namespace capture {
namespace {

constexpr uint32_t kTotalSamples = 2 * kHalfSamples;

// DMA double buffer. Only the finished (quiescent) half is read, and only
// after `g_readyOrdinal` (the volatile handshake) reports it as complete, so
// the samples themselves need no volatile qualifier.
uint16_t buffer[kTotalSamples];

// Ordinal of the newest finished buffer half: 1, 2, 3, ...; 0 means none.
// The ordinal gives both the slot ((ordinal - 1) & 1) and the true position
// of the half on the sampling grid ((ordinal - 1) x kHalfSamples), so no
// second handshake value can fall out of step with the first. Volatile
// because an ISR writes it and loop() reads it.
volatile uint32_t g_readyOrdinal = 0;

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
TIM_HandleTypeDef htim3;

// Crossing-scan state and result FIFO (issue 5). The scan runs in
// processNextHalf() from loop() at thread priority.
scan::State g_state;
scan::ResultFifo g_fifo;

#if defined(SHUTTERCHECK_DEBUG)
// The halves that the DMA finished and that the scan never saw, for the
// debug report (issue 12). The count is exact (issue 16).
uint32_t g_lostHalves = 0;
// Ordinal of the half of the last scan; 0 means no scan yet. The difference
// between two scanned ordinals is the number of halves that the DMA finished
// and that the scan missed.
uint32_t g_lastOrdinal = 0;
// The recorded raw halves (issue 16). The ring is a window on the sampling
// grid: one slot holds one scanned half and the absolute index of its first
// sample. `head` is the next slot to write and `count` the number of
// recorded slots, so the oldest recorded slot is (head - count +
// kHistoryHalves) modulo kHistoryHalves.
uint16_t g_history[kHistorySamples];
uint64_t g_historyIndex[kHistoryHalves];
uint32_t g_historyHead = 0;
uint32_t g_historyCount = 0;
// A frozen ring records nothing: a dump prints it, and the print must not
// race the DMA. The scan keeps running while the ring is frozen.
bool g_historyFrozen = false;

// Copies one scanned half into the ring. The half is quiescent: the DMA is
// filling the other one, so the copy cannot race it. A frozen ring takes
// nothing, because a dump prints it (issue 16).
void recordHalf(const uint16_t* half, uint64_t startIndex) {
  if (g_historyFrozen) return;
  memcpy(&g_history[g_historyHead * kHalfSamples], half,
         kHalfSamples * sizeof(uint16_t));
  g_historyIndex[g_historyHead] = startIndex;
  g_historyHead = (g_historyHead + 1) % kHistoryHalves;
  if (g_historyCount < kHistoryHalves) ++g_historyCount;
}
#endif

// How many halves the DMA has finished: 1, 2, 3, ... The callbacks share this
// counter and they alternate, so the count is also the slot of the newest
// half ((count - 1) & 1). `g_readyOrdinal` cannot hold the count: the loop
// clears it on every scan.
uint32_t g_publishedHalves = 0;

// The DMA finished one buffer half: publish its ordinal. The callbacks share
// this counter, and they alternate, so the ordinal is also the slot:
// (ordinal - 1) & 1.
void onHalfFinished() { g_readyOrdinal = ++g_publishedHalves; }

}  // namespace

// DMA1_Channel1 transfer/half-complete vector. The callbacks only publish the
// newest finished half; the crossing scan runs in processNextHalf() from
// loop() so the USB interrupt can preempt it (an ISR-long crosser starves the
// USB stack and breaks enumeration).
extern "C" void DMA1_Channel1_IRQHandler(void) {
  HAL_DMA_IRQHandler(&hdma_adc1);
}

extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc) {
  if (hadc == &hadc1) {
    onHalfFinished();
  }
}

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
  if (hadc == &hadc1) {
    onHalfFinished();
  }
}

void begin() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_ADC1_CLK_ENABLE();

  // PA1 = ADC1_IN1, analog in.
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_ANALOG;
  HAL_GPIO_Init(GPIOA, &gpio);

  // TIM3 counts at 72 MHz (APB1 x2). PSC = 71, ARR = 1 -> update every
  // (71+1)x(1+1) = 144 ticks = exactly 2.000 us = 500 kS/s, no software
  // jitter.
  //
  // Do not use ARR = 0. With ARR = 0 this TIM3 does not generate a periodic
  // update event, so TRGO gives no trigger and the ADC never converts.
  // Measured on hardware: ARR = 0 gives one transfer per start, ARR = 1
  // gives the full rate.
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 71;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 1;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.RepetitionCounter = 0;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  HAL_TIM_Base_Init(&htim3);
  TIM_MasterConfigTypeDef master = {};
  master.MasterOutputTrigger = TIM_TRGO_UPDATE;  // CR2 MMS = 010.
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  HAL_TIMEx_MasterConfigSynchronization(&htim3, &master);
  HAL_TIM_Base_Start(&htim3);

  // DMA1_Channel1 lands the conversions in the circular buffer. Configured
  // here, not in HAL_ADC_MspInit(): the Arduino core already defines that
  // symbol (it only enables clocks).
  __HAL_RCC_DMA1_CLK_ENABLE();
  hdma_adc1.Instance = DMA1_Channel1;
  hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
  hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
  hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_adc1.Init.Mode = DMA_CIRCULAR;
  hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
  HAL_DMA_Init(&hdma_adc1);
  __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

  // ADC1: one fixed range, 12 MHz ADCCLK, SMP = 7.5 cycles, triggered by
  // TIM3 TRGO (design invariant 3).
  hadc1.Instance = ADC1;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
  HAL_ADC_Init(&hadc1);

  ADC_ChannelConfTypeDef ch = {};
  ch.Channel = ADC_CHANNEL_1;
  ch.Rank = ADC_REGULAR_RANK_1;
  ch.SamplingTime = ADC_SAMPLETIME_7CYCLES_5;
  HAL_ADC_ConfigChannel(&hadc1, &ch);

  // Power-on calibration: reset calibration, then calibrate, wait for the
  // end (design invariant 3). No conversion ran before this.
  HAL_ADCEx_Calibration_Start(&hadc1);

  HAL_ADC_Start_DMA(&hadc1, reinterpret_cast<uint32_t*>(buffer),
                    kTotalSamples);
}

bool processNextHalf() {
  // Volatile read of the ordinal of the newest finished half; 0 means none.
  const uint32_t ordinal = g_readyOrdinal;
  if (ordinal == 0) {
    return false;
  }
  g_readyOrdinal = 0;  // Consume the handshake so this half is scanned once.
#if defined(SHUTTERCHECK_DEBUG)
  // The DMA finishes one half per half period, so the ordinal of this half
  // minus the ordinal of the last scanned one is the number of halves that
  // the scan missed, plus the one in hand. The scan missed them because the
  // DMA overwrote them (issue 16).
  if (g_lastOrdinal != 0 && ordinal > g_lastOrdinal + 1) {
    g_lostHalves += ordinal - g_lastOrdinal - 1;
  }
  g_lastOrdinal = ordinal;
#endif
  // The half is quiescent here: the circular DMA is filling the other half,
  // so the scan reads it at thread priority as a plain array.
  const uint32_t slot = (ordinal - 1) & 1;
  const uint16_t* half = &buffer[slot * kHalfSamples];
  // The polarity is a template parameter (issue 10): this build drives the
  // cascode front end, which is inverting, so dark is the high plateau.
  scan::scan<scan::Polarity::kDarkHigh>(half, kHalfSamples, &g_state, &g_fifo);
#if defined(SHUTTERCHECK_DEBUG)
  // Record the half with the index of its first sample on the absolute
  // sampling grid: (ordinal - 1) halves of kHalfSamples samples came before
  // it, also the halves the scan missed (issue 16).
  recordHalf(half, static_cast<uint64_t>(ordinal - 1) * kHalfSamples);
#endif
  return true;
}

#if defined(SHUTTERCHECK_DEBUG)
const scan::State& state() { return g_state; }
uint32_t lostHalves() { return g_lostHalves; }

uint32_t historyCount() { return g_historyCount; }

void historyChunk(uint32_t i, HistoryChunk* out) {
  // The oldest recorded slot first: `head` is the next slot to write, so the
  // oldest one is `count` slots behind it.
  const uint32_t oldest =
      (g_historyHead + kHistoryHalves - g_historyCount) % kHistoryHalves;
  const uint32_t slot = (oldest + i) % kHistoryHalves;
  out->samples = &g_history[slot * kHalfSamples];
  out->startIndex = g_historyIndex[slot];
}

void historyFreeze() { g_historyFrozen = true; }

void historyResume() {
  // The window after a print is not contiguous with the window before it, so
  // the ring starts empty again (issue 16).
  g_historyFrozen = false;
  g_historyHead = 0;
  g_historyCount = 0;
}
#endif

bool isRunning() {
  // HAL view of the live path: the ADC is in regular conversion and the
  // circular DMA channel is busy. Both handles are configured in begin().
  const bool adc = (HAL_ADC_GetState(&hadc1) & HAL_ADC_STATE_REG_BUSY) ==
                   HAL_ADC_STATE_REG_BUSY;
  const bool dma = HAL_DMA_GetState(&hdma_adc1) == HAL_DMA_STATE_BUSY;
  return adc && dma;
}

bool nextResult(scan::Result* out) { return scan::fifoPop(&g_fifo, out); }

}  // namespace capture
#endif  // defined(ARDUINO)

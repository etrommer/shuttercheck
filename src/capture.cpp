// Capture path: TIM3 (TRGO) + ADC1 + DMA1_Channel1, through the STM32 HAL.
// The whole file is firmware-only: the native unit-test build compiles it as
// an empty translation unit and tests src/scan.cpp directly.
#if defined(ARDUINO)
#include "capture.h"

#include <Arduino.h>
#include <stm32f1xx_hal.h>

namespace capture {
namespace {

constexpr uint32_t kTotalSamples = 2 * kHalfSamples;

volatile uint16_t buffer[kTotalSamples];

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
TIM_HandleTypeDef htim3;

// Crossing-scan state and result FIFO (issue 5). The scan runs in the DMA
// callbacks on the newest finished half; loop() drains the FIFO.
scan::State g_state;
scan::ResultFifo g_fifo;

void scanHalf(uint32_t idx) {
  // idx 0 = first half finished (half-complete), 1 = second half (complete).
  scan::scan(&buffer[idx * kHalfSamples], kHalfSamples, &g_state, &g_fifo);
}

}  // namespace

// DMA1_Channel1 transfer/half-complete vector. The crossing scan runs here,
// on the just-finished half, inside the interrupt (design invariant 7).
extern "C" void DMA1_Channel1_IRQHandler(void) {
  HAL_DMA_IRQHandler(&hdma_adc1);
}

extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc) {
  if (hadc == &hadc1) {
    scanHalf(0);
  }
}

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
  if (hadc == &hadc1) {
    scanHalf(1);
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

  HAL_ADC_Start_DMA(&hadc1,
                    reinterpret_cast<uint32_t*>(const_cast<uint16_t*>(buffer)),
                    kTotalSamples);
}

bool nextResult(scan::Result* out) { return scan::fifoPop(&g_fifo, out); }

}  // namespace capture
#endif  // defined(ARDUINO)

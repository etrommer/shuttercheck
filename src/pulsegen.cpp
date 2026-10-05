// Test pulse generator (issue 18). Compiled into ON_DEVICE_TEST builds only.
//
// One arm call = 1 ms of dark lead-in, then the light pulse. The lead-in is
// the time from the counter start to the compare match, the pulse is the
// time from the match to the update event at the auto-reload. PWM mode 2
// drives the pin dark (high: the output polarity is inverted) outside that
// window and light (low) inside it. One-pulse mode stops the counter at the
// update event, so the pin returns to dark and the next arm call starts from
// zero. This file is firmware-only: the native unit-test build compiles it
// as an empty translation unit.
#if defined(ON_DEVICE_TEST) && defined(ARDUINO)
#include "pulsegen.h"

#include <Arduino.h>
#include <stm32f1xx_hal.h>

namespace pulsegen {
namespace {

TIM_HandleTypeDef htim2;

// TIM2 counts at 72 MHz (APB1 x2). The counter is 16-bit and the test wants
// widths up to 1 s, so there are two tick regimes:
//   1 us per tick (PSC = 71): 1 ms lead-in = 1000 ticks, widths to 64 ms.
//   20 us per tick (PSC = 1439): 1 ms lead-in = 50 ticks, widths to 1.31 s.
constexpr uint32_t kPrescalerFast = 71;
constexpr uint32_t kPrescalerSlow = 1439;
constexpr uint32_t kLeadTicksFast = 1000;
constexpr uint32_t kLeadTicksSlow = 50;
constexpr uint32_t kTicksPerMsFast = 1000;
constexpr uint32_t kTicksPerMsSlow = 50;
// The fast regime must hold the 1000 lead-in ticks and the width in one
// 16-bit period: (65536 - 1000) / 1000 = 64 ms.
constexpr uint32_t kMaxWidthFastMs = 64;

}  // namespace

void begin() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_TIM2_CLK_ENABLE();

  // PA0 = TIM2_CH1 in the default alternate-function map, next to PA1 on
  // the header. TIM2 is free (the sample clock is TIM3).
  GPIO_InitTypeDef gpio = {};
  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &gpio);

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = kPrescalerFast;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = kLeadTicksFast + kTicksPerMsFast - 1;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.RepetitionCounter = 0;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  HAL_TIM_OnePulse_Init(&htim2, TIM_OPMODE_SINGLE);

  // PWM mode 2, inverted polarity: outside the match-to-update window the
  // pin is high (dark), inside it the pin is low (light). No preload: arm()
  // writes CCR and ARR and both take effect at the write.
  TIM_OC_InitTypeDef oc = {};
  oc.OCMode = TIM_OCMODE_PWM2;
  oc.Pulse = kLeadTicksFast;
  oc.OCPolarity = TIM_OCPOLARITY_LOW;
  oc.OCFastMode = TIM_OCFAST_DISABLE;
  HAL_TIM_PWM_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1);

  // Enable the channel output with the counter stopped, so the pin holds the
  // dark level from the first sample on. TIM2 on the F1 has no break
  // circuit: CCxE is the only output enable. HAL_TIM_OnePulse_Start is not
  // the tool here: in this HAL it arms the timer for a trigger start and it
  // marks the channels busy, so the next call fails. arm() starts the
  // counter in software instead.
  TIM_CCxChannelCmd(TIM2, TIM_CHANNEL_1, TIM_CCx_ENABLE);
}

bool isRunning() {
  // The HAL handle does not track this direct counter start; read TIM2 CEN.
  return (TIM2->CR1 & TIM_CR1_CEN) != 0;
}


void arm(uint32_t widthMs) {
  const bool fast = widthMs <= kMaxWidthFastMs;
  const uint32_t lead = fast ? kLeadTicksFast : kLeadTicksSlow;
  const uint32_t widthTicks =
      widthMs * (fast ? kTicksPerMsFast : kTicksPerMsSlow);

  // The counter period is ARR + 1 ticks, so the match-to-update window (the
  // pulse) is ARR + 1 - CCR ticks wide. PWM mode 2 opens the window at the
  // compare match (the light edge) and closes it at the update event (the
  // dark edge).
  __HAL_TIM_SET_PRESCALER(&htim2, fast ? kPrescalerFast : kPrescalerSlow);
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, lead);
  __HAL_TIM_SET_AUTORELOAD(&htim2, lead + widthTicks - 1);
  // The prescaler is shadow-loaded: the update event takes it into account.
  // The same event clears the counter, so the lead-in is exact.
  HAL_TIM_GenerateEvent(&htim2, TIM_EVENTSOURCE_UPDATE);
  // One-pulse mode: the hardware clears CEN at the update event of the
  // pulse, and the pin returns to dark.
  __HAL_TIM_ENABLE(&htim2);
}


}  // namespace pulsegen
#endif  // defined(ON_DEVICE_TEST) && defined(ARDUINO)

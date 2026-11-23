#include "azimuth_service.h"
#include "pwm_service.h"
#include "settings_service.h"

#define AZ_FULL_CIRCLE_X10 3600L

/* Integer division rounded to nearest (halves away from zero). denominator > 0. */
static int32_t Az_DivRound(int32_t numerator, int32_t denominator)
{
  return (numerator >= 0) ? ((numerator + (denominator / 2)) / denominator)
                          : -(((-numerator) + (denominator / 2)) / denominator);
}

static int32_t Az_Wrap(int32_t azimuth_x10)
{
  azimuth_x10 %= AZ_FULL_CIRCLE_X10;
  return (azimuth_x10 < 0) ? (azimuth_x10 + AZ_FULL_CIRCLE_X10) : azimuth_x10;
}

/* Allowed pulse window: pmin..pmax narrowed by the rotation limits. Returns 1 if a limit narrows it. */
static uint8_t Az_Window(const Settings_t *settings, uint16_t *low, uint16_t *high)
{
  uint8_t limited = 0U;

  *low = settings->az_pulse_min;
  *high = settings->az_pulse_max;
  if ((settings->az_limit_pulse_min != 0U) && (settings->az_limit_pulse_min > *low))
  {
    *low = settings->az_limit_pulse_min;
    limited = 1U;
  }
  if ((settings->az_limit_pulse_max != 0U) && (settings->az_limit_pulse_max < *high))
  {
    *high = settings->az_limit_pulse_max;
    limited = 1U;
  }
  return limited;
}

static uint16_t Az_ServoPosition(const Settings_t *settings)
{
  return (settings->az_servo == 2U) ? PWM_ServiceGetServo2Position() : PWM_ServiceGetServo1Position();
}

static uint16_t Az_ServoTarget(const Settings_t *settings)
{
  return (settings->az_servo == 2U) ? PWM_ServiceGetServo2() : PWM_ServiceGetServo1();
}

int16_t Az_FromPulse(uint16_t pulse)
{
  const Settings_t *settings = Settings_Get();
  int32_t span = (int32_t)settings->az_pulse_max - (int32_t)settings->az_pulse_min;
  int32_t turned;

  if ((settings->az_cal_pulse == 0U) || (pulse == 0U) || (span <= 0))
  {
    return AZ_UNKNOWN;
  }

  /* |pulse - cal| <= 3000 and range <= 3600: the product stays far below INT32_MAX. */
  turned = Az_DivRound(((int32_t)pulse - (int32_t)settings->az_cal_pulse) * (int32_t)settings->az_range_x10, span);
  if (settings->az_invert != 0U)
  {
    turned = -turned;
  }
  return (int16_t)Az_Wrap((int32_t)settings->az_cal_azimuth_x10 + turned);
}

Az_Error_t Az_PulseFor(uint16_t azimuth_x10, uint16_t *pulse)
{
  const Settings_t *settings = Settings_Get();
  int32_t span = (int32_t)settings->az_pulse_max - (int32_t)settings->az_pulse_min;
  int32_t turn;
  uint8_t attempt;
  uint8_t limited = 0U;
  uint16_t low;
  uint16_t high;

  if (azimuth_x10 >= AZ_FULL_CIRCLE_X10)
  {
    return AZ_ERR_BAD_VALUE;
  }
  if ((settings->az_cal_pulse == 0U) || (span <= 0) || (settings->az_range_x10 == 0U))
  {
    return AZ_ERR_NOT_CALIBRATED;
  }

  /* The mechanism does not wrap, so the target is reached either by turning clockwise by `turn`
   * or counter-clockwise by 360 - turn from the bound point. Only one of the two (both at a
   * full 360-degree range) lands inside pmin..pmax. */
  (void)Az_Window(settings, &low, &high);
  turn = Az_Wrap((int32_t)azimuth_x10 - (int32_t)settings->az_cal_azimuth_x10);
  for (attempt = 0U; attempt < 2U; attempt++)
  {
    int32_t offset = Az_DivRound(turn * span, (int32_t)settings->az_range_x10);
    int32_t candidate = (int32_t)settings->az_cal_pulse + ((settings->az_invert != 0U) ? -offset : offset);

    if ((candidate >= (int32_t)settings->az_pulse_min) && (candidate <= (int32_t)settings->az_pulse_max))
    {
      if ((candidate >= (int32_t)low) && (candidate <= (int32_t)high))
      {
        *pulse = (uint16_t)candidate;
        return AZ_OK;
      }
      limited = 1U;
    }
    turn -= AZ_FULL_CIRCLE_X10;
  }
  return (limited != 0U) ? AZ_ERR_LIMIT : AZ_ERR_UNREACHABLE;
}

static void Az_SetServo(uint16_t pulse)
{
  if (Settings_Get()->az_servo == 2U)
  {
    PWM_ServiceSetServo2(pulse);
  }
  else
  {
    PWM_ServiceSetServo1(pulse);
  }
}

Az_Error_t Az_Goto(uint16_t azimuth_x10)
{
  uint16_t pulse;
  Az_Error_t error = Az_PulseFor(azimuth_x10, &pulse);

  if (error == AZ_OK)
  {
    Az_SetServo(pulse);
  }
  return error;
}

Az_Error_t Az_GotoZero(void)
{
  const Settings_t *settings = Settings_Get();
  uint16_t low;
  uint16_t high;

  if ((AZ_ZERO_PULSE < settings->az_pulse_min) || (AZ_ZERO_PULSE > settings->az_pulse_max))
  {
    return AZ_ERR_UNREACHABLE;
  }
  (void)Az_Window(settings, &low, &high);
  if ((AZ_ZERO_PULSE < low) || (AZ_ZERO_PULSE > high))
  {
    return AZ_ERR_LIMIT;
  }
  Az_SetServo(AZ_ZERO_PULSE);
  return AZ_OK;
}

/* Pulse that is turn_x10 (signed, clockwise > 0) away from the binding, clamped to pmin..pmax. */
static uint16_t Az_PulseAtTurn(const Settings_t *settings, int32_t turn_x10)
{
  int32_t span = (int32_t)settings->az_pulse_max - (int32_t)settings->az_pulse_min;
  int32_t offset = Az_DivRound(turn_x10 * span, (int32_t)settings->az_range_x10);
  int32_t pulse = (int32_t)settings->az_cal_pulse + ((settings->az_invert != 0U) ? -offset : offset);

  if (pulse < (int32_t)settings->az_pulse_min)
  {
    pulse = (int32_t)settings->az_pulse_min;
  }
  if (pulse > (int32_t)settings->az_pulse_max)
  {
    pulse = (int32_t)settings->az_pulse_max;
  }
  return (uint16_t)pulse;
}

/* Signed clockwise turn (0.1 deg) from the binding to a pulse. */
static int32_t Az_TurnAtPulse(const Settings_t *settings, uint16_t pulse)
{
  int32_t span = (int32_t)settings->az_pulse_max - (int32_t)settings->az_pulse_min;
  int32_t turned = Az_DivRound(((int32_t)pulse - (int32_t)settings->az_cal_pulse) * (int32_t)settings->az_range_x10, span);

  return (settings->az_invert != 0U) ? -turned : turned;
}

static Az_Error_t Az_SaveLimits(uint16_t limit_min, uint16_t limit_max, int32_t plus_x10, int32_t minus_x10)
{
  Settings_t settings = *Settings_Get();

  settings.az_limit_pulse_min = limit_min;
  settings.az_limit_pulse_max = limit_max;
  settings.az_limit_plus_x10 = (plus_x10 == AZ_LIMIT_NONE) ? SETTINGS_AZ_EMPTY : (uint16_t)plus_x10;
  settings.az_limit_minus_x10 = (minus_x10 == AZ_LIMIT_NONE) ? SETTINGS_AZ_EMPTY : (uint16_t)minus_x10;
  settings.az_limit_ref_pulse = settings.az_cal_pulse;
  Settings_Set(&settings);
  Az_ApplyLimits();     /* enforced at once, even if the flash write fails */
  return (Settings_Save() != 0U) ? AZ_OK : AZ_ERR_SAVE;
}

Az_Error_t Az_SetLimits(int32_t plus_x10, int32_t minus_x10)
{
  const Settings_t *settings = Settings_Get();
  uint16_t clockwise = 0U;
  uint16_t counter_clockwise = 0U;

  if ((plus_x10 < AZ_LIMIT_NONE) || (plus_x10 > AZ_FULL_CIRCLE_X10) ||
      (minus_x10 < AZ_LIMIT_NONE) || (minus_x10 > AZ_FULL_CIRCLE_X10) ||
      ((plus_x10 == AZ_LIMIT_NONE) && (minus_x10 == AZ_LIMIT_NONE)) ||
      ((plus_x10 == 0L) && (minus_x10 == 0L)))
  {
    return AZ_ERR_BAD_VALUE;
  }
  if ((settings->az_cal_pulse == 0U) || (settings->az_pulse_max <= settings->az_pulse_min) ||
      (settings->az_range_x10 == 0U))
  {
    return AZ_ERR_NOT_CALIBRATED;
  }

  if (plus_x10 != AZ_LIMIT_NONE)
  {
    clockwise = Az_PulseAtTurn(settings, plus_x10);
  }
  if (minus_x10 != AZ_LIMIT_NONE)
  {
    counter_clockwise = Az_PulseAtTurn(settings, -minus_x10);
  }
  /* Clockwise is the larger pulse unless the direction is inverted. */
  return (settings->az_invert != 0U) ? Az_SaveLimits(clockwise, counter_clockwise, plus_x10, minus_x10)
                                     : Az_SaveLimits(counter_clockwise, clockwise, plus_x10, minus_x10);
}

Az_Error_t Az_ClearLimits(void)
{
  return Az_SaveLimits(0U, 0U, AZ_LIMIT_NONE, AZ_LIMIT_NONE);
}

uint8_t Az_GetLimits(int32_t *plus_x10, int32_t *minus_x10)
{
  const Settings_t *settings = Settings_Get();
  uint16_t clockwise = (settings->az_invert != 0U) ? settings->az_limit_pulse_min : settings->az_limit_pulse_max;
  uint16_t counter_clockwise = (settings->az_invert != 0U) ? settings->az_limit_pulse_max : settings->az_limit_pulse_min;

  uint8_t same_binding;

  *plus_x10 = 0L;
  *minus_x10 = 0L;
  if ((settings->az_cal_pulse == 0U) || (settings->az_pulse_max <= settings->az_pulse_min))
  {
    return 0U;
  }
  /* Entered values while the binding pulse is unchanged, otherwise recomputed from the pulses. */
  same_binding = (settings->az_limit_ref_pulse == settings->az_cal_pulse) ? 1U : 0U;
  if (clockwise != 0U)
  {
    *plus_x10 = ((same_binding != 0U) && (settings->az_limit_plus_x10 != SETTINGS_AZ_EMPTY))
                ? (int32_t)settings->az_limit_plus_x10 : Az_TurnAtPulse(settings, clockwise);
  }
  if (counter_clockwise != 0U)
  {
    *minus_x10 = ((same_binding != 0U) && (settings->az_limit_minus_x10 != SETTINGS_AZ_EMPTY))
                 ? (int32_t)settings->az_limit_minus_x10 : -Az_TurnAtPulse(settings, counter_clockwise);
  }
  return 1U;
}

void Az_ApplyLimits(void)
{
  const Settings_t *settings = Settings_Get();
  uint8_t index = (settings->az_servo == 2U) ? 1U : 0U;
  uint16_t low;
  uint16_t high;

  PWM_ServiceSetLimits((uint8_t)(1U - index), 0U, 0U);
  if (Az_Window(settings, &low, &high) != 0U)
  {
    PWM_ServiceSetLimits(index, low, high);   /* with a limit set, raw pulses stay inside pmin..pmax too */
  }
  else
  {
    PWM_ServiceSetLimits(index, 0U, 0U);
  }
}

Az_Error_t Az_GotoPreset(uint8_t slot)
{
  if (slot >= SETTINGS_AZ_PRESET_COUNT)
  {
    return AZ_ERR_BAD_VALUE;
  }
  if (Settings_Get()->az_presets[slot].azimuth_x10 == SETTINGS_AZ_EMPTY)
  {
    return AZ_ERR_NO_PRESET;
  }
  return Az_Goto(Settings_Get()->az_presets[slot].azimuth_x10);
}

void Az_GetState(int16_t *position_x10, int16_t *target_x10)
{
  const Settings_t *settings = Settings_Get();

  *position_x10 = Az_FromPulse(Az_ServoPosition(settings));
  *target_x10 = Az_FromPulse(Az_ServoTarget(settings));
}

const char *Az_ErrorName(Az_Error_t error)
{
  switch (error)
  {
    case AZ_OK: return "";
    case AZ_ERR_BAD_VALUE: return "bad_value";
    case AZ_ERR_NOT_CALIBRATED: return "not_calibrated";
    case AZ_ERR_UNREACHABLE: return "unreachable";
    case AZ_ERR_NO_PRESET: return "no_preset";
    case AZ_ERR_SAVE: return "save_failed";
    case AZ_ERR_LIMIT: return "limit";
    default: return "error";
  }
}

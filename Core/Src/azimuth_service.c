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
  turn = Az_Wrap((int32_t)azimuth_x10 - (int32_t)settings->az_cal_azimuth_x10);
  for (attempt = 0U; attempt < 2U; attempt++)
  {
    int32_t offset = Az_DivRound(turn * span, (int32_t)settings->az_range_x10);
    int32_t candidate = (int32_t)settings->az_cal_pulse + ((settings->az_invert != 0U) ? -offset : offset);

    if ((candidate >= (int32_t)settings->az_pulse_min) && (candidate <= (int32_t)settings->az_pulse_max))
    {
      *pulse = (uint16_t)candidate;
      return AZ_OK;
    }
    turn -= AZ_FULL_CIRCLE_X10;
  }
  return AZ_ERR_UNREACHABLE;
}

Az_Error_t Az_Goto(uint16_t azimuth_x10)
{
  uint16_t pulse;
  Az_Error_t error = Az_PulseFor(azimuth_x10, &pulse);

  if (error == AZ_OK)
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
  return error;
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
    default: return "error";
  }
}

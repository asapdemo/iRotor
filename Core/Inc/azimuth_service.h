#ifndef __AZIMUTH_SERVICE_H
#define __AZIMUTH_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Azimuth <-> servo pulse, on the board, integer only.
 * Azimuth unit: 0.1 deg, 0..3599, 0 = north, clockwise. Pulse unit: us.
 * Linear model from the settings: az_pulse_min..az_pulse_max covers az_range_x10 of rotation,
 * az_invert gives the direction, and the binding (az_cal_pulse = az_cal_azimuth_x10) anchors it. */

#define AZ_UNKNOWN (-1)          /* azimuth not known: not bound, or servo output off */
#define AZ_ZERO_PULSE 1346U      /* "zero" (home) position of the mechanism, us */

typedef enum
{
  AZ_OK = 0,
  AZ_ERR_BAD_VALUE,              /* value out of range / not a number */
  AZ_ERR_NOT_CALIBRATED,         /* no binding yet: /az/cal first */
  AZ_ERR_UNREACHABLE,            /* azimuth lies in the dead zone of the mechanism */
  AZ_ERR_NO_PRESET,              /* preset slot is empty */
  AZ_ERR_SAVE,                   /* flash write failed */
  AZ_ERR_LIMIT                   /* the mechanism could get there, but the rotation limits forbid it */
} Az_Error_t;

#define AZ_LIMIT_NONE (-1L)      /* Az_SetLimits: no limit on that side */

/* Azimuth (0.1 deg) for a pulse, or AZ_UNKNOWN. */
int16_t Az_FromPulse(uint16_t pulse);
/* Pulse for an azimuth (0.1 deg). */
Az_Error_t Az_PulseFor(uint16_t azimuth_x10, uint16_t *pulse);
/* Set the azimuth servo's target; it moves with its current speed. */
Az_Error_t Az_Goto(uint16_t azimuth_x10);
Az_Error_t Az_GotoPreset(uint8_t slot);
/* Move the azimuth servo to AZ_ZERO_PULSE; works without a binding. */
Az_Error_t Az_GotoZero(void);
/* Rotation limits, relative to the binding: plus = clockwise, minus = counter-clockwise, 0.1 deg
 * (AZ_LIMIT_NONE = that side is free). Needs a binding; stored as pulses and saved to flash. */
Az_Error_t Az_SetLimits(int32_t plus_x10, int32_t minus_x10);
Az_Error_t Az_ClearLimits(void);
/* Limits relative to the current binding (signed, 0.1 deg); returns 0 when not bound. */
uint8_t Az_GetLimits(int32_t *plus_x10, int32_t *minus_x10);
/* Hands the allowed pulse window to pwm_service, which clamps every target of the azimuth servo.
 * Call at boot (before PWM_ServiceApplyDefaults) and after every change of the azimuth settings. */
void Az_ApplyLimits(void);
/* Current position and target of the azimuth servo, AZ_UNKNOWN when not known. */
void Az_GetState(int16_t *position_x10, int16_t *target_x10);
const char *Az_ErrorName(Az_Error_t error);

#ifdef __cplusplus
}
#endif

#endif /* __AZIMUTH_SERVICE_H */

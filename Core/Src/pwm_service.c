#include "pwm_service.h"

#define PWM_SERVO_COUNT 2U
#define PWM_SERVO_FRAME_MS 20U
/* Position is kept in 1/256 us. Step per 20 ms frame = speed * 20 / 256 us,
 * i.e. ~3.9 us/s per speed unit: 1 -> ~4 us/s, 254 -> ~990 us/s (about the servo's own maximum). */
#define PWM_SERVO_Q8_SHIFT 8U
#define PWM_SERVO_STEP_Q8_PER_SPEED 20UL

typedef struct
{
  uint32_t channel;
  uint32_t position_q8;
  uint16_t target;
  uint8_t speed;
} PWM_Servo_t;

static TIM_HandleTypeDef *pwm_tim1;
static PWM_Servo_t pwm_servos[PWM_SERVO_COUNT] =
{
  {TIM_CHANNEL_4, 0UL, 0U, PWM_SERVICE_SPEED_INSTANT},
  {TIM_CHANNEL_1, 0UL, 0U, PWM_SERVICE_SPEED_INSTANT}
};
static uint32_t pwm_last_tick;

static void PWM_ServiceWriteServo(PWM_Servo_t *servo, uint16_t position)
{
  servo->position_q8 = (uint32_t)position << PWM_SERVO_Q8_SHIFT;
  __HAL_TIM_SET_COMPARE(pwm_tim1, servo->channel, position);
}

static void PWM_ServiceSetTarget(PWM_Servo_t *servo, uint16_t value)
{
  servo->target = (value > PWM_SERVICE_SERVO_MAX) ? PWM_SERVICE_SERVO_MAX : value;

  /* 0 means "no pulse", so turning the output on or off never ramps through invalid widths. */
  if ((servo->speed == PWM_SERVICE_SPEED_INSTANT) || (servo->position_q8 == 0UL) || (servo->target == 0U))
  {
    PWM_ServiceWriteServo(servo, servo->target);
  }
}

static void PWM_ServiceSetSpeed(PWM_Servo_t *servo, uint8_t speed)
{
  servo->speed = speed;
  if (speed == PWM_SERVICE_SPEED_INSTANT)
  {
    PWM_ServiceWriteServo(servo, servo->target);
  }
}

void PWM_ServiceInit(TIM_HandleTypeDef *tim1)
{
  pwm_tim1 = tim1;

  if (pwm_tim1 == NULL)
  {
    Error_Handler();
  }

  PWM_ServiceWriteServo(&pwm_servos[0], 0U);
  PWM_ServiceWriteServo(&pwm_servos[1], 0U);
  pwm_last_tick = HAL_GetTick();

  if (HAL_TIM_PWM_Start(pwm_tim1, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_TIM_PWM_Start(pwm_tim1, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
}

void PWM_ServiceApplyDefaults(uint16_t servo1, uint16_t servo2,
                              uint8_t servo1_speed, uint8_t servo2_speed)
{
  PWM_ServiceSetSpeed(&pwm_servos[0], servo1_speed);
  PWM_ServiceSetSpeed(&pwm_servos[1], servo2_speed);
  PWM_ServiceSetTarget(&pwm_servos[0], servo1);
  PWM_ServiceSetTarget(&pwm_servos[1], servo2);
}

void PWM_ServiceTask(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t frames = (now - pwm_last_tick) / PWM_SERVO_FRAME_MS;
  uint8_t index;

  if (frames == 0UL)
  {
    return;
  }
  pwm_last_tick += frames * PWM_SERVO_FRAME_MS;

  for (index = 0U; index < PWM_SERVO_COUNT; index++)
  {
    PWM_Servo_t *servo = &pwm_servos[index];
    uint32_t target_q8 = (uint32_t)servo->target << PWM_SERVO_Q8_SHIFT;
    uint32_t step_q8 = frames * servo->speed * PWM_SERVO_STEP_Q8_PER_SPEED;
    uint32_t position_q8 = servo->position_q8;

    if ((servo->speed == 0U) || (position_q8 == target_q8))
    {
      continue;
    }

    if (position_q8 < target_q8)
    {
      position_q8 = ((target_q8 - position_q8) > step_q8) ? (position_q8 + step_q8) : target_q8;
    }
    else
    {
      position_q8 = ((position_q8 - target_q8) > step_q8) ? (position_q8 - step_q8) : target_q8;
    }
    servo->position_q8 = position_q8;
    __HAL_TIM_SET_COMPARE(pwm_tim1, servo->channel, position_q8 >> PWM_SERVO_Q8_SHIFT);
  }
}

void PWM_ServiceSetServo1(uint16_t value)
{
  PWM_ServiceSetTarget(&pwm_servos[0], value);
}

void PWM_ServiceSetServo2(uint16_t value)
{
  PWM_ServiceSetTarget(&pwm_servos[1], value);
}

void PWM_ServiceSetServo1Speed(uint8_t speed)
{
  PWM_ServiceSetSpeed(&pwm_servos[0], speed);
}

void PWM_ServiceSetServo2Speed(uint8_t speed)
{
  PWM_ServiceSetSpeed(&pwm_servos[1], speed);
}

uint16_t PWM_ServiceGetServo1(void)
{
  return pwm_servos[0].target;
}

uint16_t PWM_ServiceGetServo2(void)
{
  return pwm_servos[1].target;
}

uint16_t PWM_ServiceGetServo1Position(void)
{
  return (uint16_t)(pwm_servos[0].position_q8 >> PWM_SERVO_Q8_SHIFT);
}

uint16_t PWM_ServiceGetServo2Position(void)
{
  return (uint16_t)(pwm_servos[1].position_q8 >> PWM_SERVO_Q8_SHIFT);
}

uint8_t PWM_ServiceGetServo1Speed(void)
{
  return pwm_servos[0].speed;
}

uint8_t PWM_ServiceGetServo2Speed(void)
{
  return pwm_servos[1].speed;
}

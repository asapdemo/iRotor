#ifndef __PWM_SERVICE_H
#define __PWM_SERVICE_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PWM_SERVICE_SERVO_MAX 3000U
/* Servo speed: 0 = hold the current position, 1..254 = ~3.9 us/s per unit (254 ~ 990 us/s), 255 = instant. */
#define PWM_SERVICE_SPEED_MAX 255U
#define PWM_SERVICE_SPEED_INSTANT 255U

void PWM_ServiceInit(TIM_HandleTypeDef *tim1);
void PWM_ServiceApplyDefaults(uint16_t servo1, uint16_t servo2,
                              uint8_t servo1_speed, uint8_t servo2_speed);
void PWM_ServiceTask(void);
void PWM_ServiceSetServo1(uint16_t value);
void PWM_ServiceSetServo2(uint16_t value);
void PWM_ServiceSetServo1Speed(uint8_t speed);
void PWM_ServiceSetServo2Speed(uint8_t speed);
uint16_t PWM_ServiceGetServo1(void);
uint16_t PWM_ServiceGetServo2(void);
uint16_t PWM_ServiceGetServo1Position(void);
uint16_t PWM_ServiceGetServo2Position(void);
uint8_t PWM_ServiceGetServo1Speed(void);
uint8_t PWM_ServiceGetServo2Speed(void);

#ifdef __cplusplus
}
#endif

#endif /* __PWM_SERVICE_H */

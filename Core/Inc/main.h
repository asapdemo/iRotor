/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"
#include "stm32f1xx_ll_crc.h"
#include "stm32f1xx_ll_iwdg.h"
#include "stm32f1xx_ll_rcc.h"
#include "stm32f1xx_ll_bus.h"
#include "stm32f1xx_ll_system.h"
#include "stm32f1xx_ll_exti.h"
#include "stm32f1xx_ll_cortex.h"
#include "stm32f1xx_ll_utils.h"
#include "stm32f1xx_ll_pwr.h"
#include "stm32f1xx_ll_dma.h"
#include "stm32f1xx_ll_gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED0_Pin GPIO_PIN_14
#define LED0_GPIO_Port GPIOC
#define LED1_Pin GPIO_PIN_15
#define LED1_GPIO_Port GPIOC
#define W5500_INT_Pin GPIO_PIN_0
#define W5500_INT_GPIO_Port GPIOA
#define DO0_Pin GPIO_PIN_0
#define DO0_GPIO_Port GPIOB
#define DO1_Pin GPIO_PIN_1
#define DO1_GPIO_Port GPIOB
#define DO2_Pin GPIO_PIN_2
#define DO2_GPIO_Port GPIOB
#define W5500_CS_Pin GPIO_PIN_12
#define W5500_CS_GPIO_Port GPIOB
#define Servo0_Pin GPIO_PIN_8
#define Servo0_GPIO_Port GPIOA
#define Servo1_Pin GPIO_PIN_11
#define Servo1_GPIO_Port GPIOA
#define W5500_Reset_Pin GPIO_PIN_12
#define W5500_Reset_GPIO_Port GPIOA
#define U3_DE_Pin GPIO_PIN_15
#define U3_DE_GPIO_Port GPIOA
#define DO3_Pin GPIO_PIN_3
#define DO3_GPIO_Port GPIOB
#define DO4_Pin GPIO_PIN_4
#define DO4_GPIO_Port GPIOB
#define G15_Pin GPIO_PIN_5
#define G15_GPIO_Port GPIOB
#define W5500_PINMODE2_Pin GPIO_PIN_6
#define W5500_PINMODE2_GPIO_Port GPIOB
#define W5500_PINMODE1_Pin GPIO_PIN_7
#define W5500_PINMODE1_GPIO_Port GPIOB
#define W5500_PINMODE0_Pin GPIO_PIN_8
#define W5500_PINMODE0_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

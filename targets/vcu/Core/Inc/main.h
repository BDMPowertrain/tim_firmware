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
#include "stm32g4xx_hal.h"

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

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define Can_Term_2_Pin GPIO_PIN_13
#define Can_Term_2_GPIO_Port GPIOC
#define FAULT_TP_Pin GPIO_PIN_14
#define FAULT_TP_GPIO_Port GPIOC
#define GPIO_GNDDETECT_Pin GPIO_PIN_15
#define GPIO_GNDDETECT_GPIO_Port GPIOC
#define RSV_WHEEL1_Pin GPIO_PIN_0
#define RSV_WHEEL1_GPIO_Port GPIOC
#define RSV_WHEEL2_Pin GPIO_PIN_1
#define RSV_WHEEL2_GPIO_Port GPIOC
#define RSV_WHEEL3_Pin GPIO_PIN_2
#define RSV_WHEEL3_GPIO_Port GPIOC
#define VBAT_ADC_Pin GPIO_PIN_3
#define VBAT_ADC_GPIO_Port GPIOC
#define APPS1_Pin GPIO_PIN_0
#define APPS1_GPIO_Port GPIOA
#define SENSOR_SUPPLY1_Pin GPIO_PIN_1
#define SENSOR_SUPPLY1_GPIO_Port GPIOA
#define RSV_COOLANT_Pin GPIO_PIN_2
#define RSV_COOLANT_GPIO_Port GPIOA
#define BSE_FRONT_Pin GPIO_PIN_3
#define BSE_FRONT_GPIO_Port GPIOA
#define SENSOR_SUPPLY2_Pin GPIO_PIN_4
#define SENSOR_SUPPLY2_GPIO_Port GPIOA
#define BSE_REAR_Pin GPIO_PIN_5
#define BSE_REAR_GPIO_Port GPIOA
#define SDC_MON_N_Pin GPIO_PIN_6
#define SDC_MON_N_GPIO_Port GPIOA
#define BRAKE_LIGHT_Pin GPIO_PIN_7
#define BRAKE_LIGHT_GPIO_Port GPIOA
#define APPS2_Pin GPIO_PIN_4
#define APPS2_GPIO_Port GPIOC
#define RTD_SOUND_Pin GPIO_PIN_5
#define RTD_SOUND_GPIO_Port GPIOC
#define FAULT_LED_Pin GPIO_PIN_0
#define FAULT_LED_GPIO_Port GPIOB
#define RSV_FAN_ISENSE_Pin GPIO_PIN_1
#define RSV_FAN_ISENSE_GPIO_Port GPIOB
#define RSV_PUMP_ISENSE_Pin GPIO_PIN_2
#define RSV_PUMP_ISENSE_GPIO_Port GPIOB
#define RSV_WHEEL4_Pin GPIO_PIN_10
#define RSV_WHEEL4_GPIO_Port GPIOB
#define SD_DETECT_Pin GPIO_PIN_11
#define SD_DETECT_GPIO_Port GPIOB
#define SPI2_CS_Pin GPIO_PIN_12
#define SPI2_CS_GPIO_Port GPIOB
#define GPIO_CAN2LED_Pin GPIO_PIN_6
#define GPIO_CAN2LED_GPIO_Port GPIOC
#define GPIO_CAN1LED_Pin GPIO_PIN_7
#define GPIO_CAN1LED_GPIO_Port GPIOC
#define GPIO_HBLED_Pin GPIO_PIN_8
#define GPIO_HBLED_GPIO_Port GPIOC
#define Can_Term_1_Pin GPIO_PIN_9
#define Can_Term_1_GPIO_Port GPIOC
#define FDCAN1_STB_Pin GPIO_PIN_8
#define FDCAN1_STB_GPIO_Port GPIOA
#define SWDIO_Pin GPIO_PIN_13
#define SWDIO_GPIO_Port GPIOA
#define SWCLK_Pin GPIO_PIN_14
#define SWCLK_GPIO_Port GPIOA
#define START_BTN_N_Pin GPIO_PIN_15
#define START_BTN_N_GPIO_Port GPIOA
#define SPI3_CS_Pin GPIO_PIN_2
#define SPI3_CS_GPIO_Port GPIOD
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB
#define IMU_INT_Pin GPIO_PIN_4
#define IMU_INT_GPIO_Port GPIOB
#define IMU_INT_EXTI_IRQn EXTI4_IRQn
#define FDCAN2_STB_Pin GPIO_PIN_9
#define FDCAN2_STB_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

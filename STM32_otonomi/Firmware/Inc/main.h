#ifndef SEASTARS_MAIN_H
#define SEASTARS_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32l1xx_hal.h"

#define STEP1_Pin              GPIO_PIN_10
#define STEP1_GPIO_Port        GPIOA
#define DIR1_Pin               GPIO_PIN_3
#define DIR1_GPIO_Port         GPIOB
#define STEP2_Pin              GPIO_PIN_5
#define STEP2_GPIO_Port        GPIOB
#define DIR2_Pin               GPIO_PIN_4
#define DIR2_GPIO_Port         GPIOB
#define STATUS_LED_Pin         GPIO_PIN_5
#define STATUS_LED_GPIO_Port   GPIOA

#define ESC1_Pin               GPIO_PIN_6
#define ESC1_GPIO_Port         GPIOA
#define ESC2_Pin               GPIO_PIN_7
#define ESC2_GPIO_Port         GPIOA

#define IMU_I2C_SCL_Pin            GPIO_PIN_8
#define IMU_I2C_SCL_GPIO_Port      GPIOB
#define IMU_I2C_SDA_Pin            GPIO_PIN_9
#define IMU_I2C_SDA_GPIO_Port      GPIOB

#define D300_I2C_SCL_Pin           GPIO_PIN_10
#define D300_I2C_SCL_GPIO_Port     GPIOB
#define D300_I2C_SDA_Pin           GPIO_PIN_11
#define D300_I2C_SDA_GPIO_Port     GPIOB

void Error_Handler(void);
void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

#ifdef __cplusplus
}
#endif

#endif

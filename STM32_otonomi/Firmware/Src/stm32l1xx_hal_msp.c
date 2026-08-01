#include "main.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    if (hi2c->Instance == I2C1) {
        __HAL_RCC_I2C1_CLK_ENABLE();
        gpio.Pin = IMU_I2C_SCL_Pin | IMU_I2C_SDA_Pin;
        gpio.Alternate = GPIO_AF4_I2C1;
        HAL_GPIO_Init(GPIOB, &gpio);
    } else if (hi2c->Instance == I2C2) {
        __HAL_RCC_I2C2_CLK_ENABLE();
        gpio.Pin = D300_I2C_SCL_Pin | D300_I2C_SDA_Pin;
        gpio.Alternate = GPIO_AF4_I2C2;
        HAL_GPIO_Init(GPIOB, &gpio);
    }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1) {
        __HAL_RCC_I2C1_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOB, IMU_I2C_SCL_Pin | IMU_I2C_SDA_Pin);
    } else if (hi2c->Instance == I2C2) {
        __HAL_RCC_I2C2_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOB, D300_I2C_SCL_Pin | D300_I2C_SDA_Pin);
    }
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gpio = {0};
    if (huart->Instance == USART2) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_USART2_CLK_ENABLE();
        gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        gpio.Mode = GPIO_MODE_AF_PP;
        gpio.Pull = GPIO_PULLUP;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        gpio.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &gpio);
        HAL_NVIC_SetPriority(USART2_IRQn, 6u, 0u);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
        HAL_NVIC_SetPriority(TIM2_IRQn, 4u, 0u);
        HAL_NVIC_EnableIRQ(TIM2_IRQn);
    } else if (htim->Instance == TIM5) {
        __HAL_RCC_TIM5_CLK_ENABLE();
        HAL_NVIC_SetPriority(TIM5_IRQn, 4u, 0u);
        HAL_NVIC_EnableIRQ(TIM5_IRQn);
    }
}

void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        __HAL_RCC_TIM3_CLK_ENABLE();
    }
}

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef gpio = {0};
    if (htim->Instance != TIM3) return;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpio.Pin = ESC1_Pin | ESC2_Pin;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOA, &gpio);
}

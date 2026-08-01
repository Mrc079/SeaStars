#include "main.h"
#include "stm32l1xx_it.h"

extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim5;
extern UART_HandleTypeDef huart2;

void NMI_Handler(void) {}

void HardFault_Handler(void)
{
    Error_Handler();
}

void MemManage_Handler(void)
{
    Error_Handler();
}

void BusFault_Handler(void)
{
    Error_Handler();
}

void UsageFault_Handler(void)
{
    Error_Handler();
}

void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

void SysTick_Handler(void)
{
    HAL_IncTick();
    HAL_SYSTICK_IRQHandler();
}

void TIM2_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim2);
}

void TIM5_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim5);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

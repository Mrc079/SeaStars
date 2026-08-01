/*
 * STM32L152 HAL integration example for the SEA STARS App modules.
 *
 * This file is intentionally outside App/Src and is not built automatically.
 * Copy/merge it only after CubeMX has generated main.h, i2c.h and usart.h.
 * The board_* functions are the narrow hardware layer that must be connected
 * to the team's confirmed ESC PWM pins and existing TIM2/TIM5 stepper code.
 */

#include "main.h"
#include "i2c.h"
#include "usart.h"

#include "imu_i2c.h"
#include "ms5837.h"
#include "seastars_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CONTROL_PERIOD_MS   20u
#define DEPTH_PERIOD_MS    100u
#define TELEMETRY_PERIOD_MS 250u
#define COMMAND_SIZE         80u

/* Implement these against the confirmed timer/GPIO configuration. */
extern void board_set_thruster_percent(uint8_t index, float percent);
extern void board_set_tank_target(uint8_t index, int32_t absolute_steps);
extern void board_set_tank_speed(uint8_t index, uint32_t steps_per_second);
extern void board_zero_tank_position(uint8_t index);
extern int32_t board_get_tank_position(uint8_t index);
extern void board_set_armed(bool armed);

static ms5837_t d300;
static imu_i2c_t imu;
static seastars_runtime_t runtime;
static imu_sample_t imu_sample;
static ms5837_sample_t depth_sample;

static uint8_t station_rx_byte;
static char rx_build[COMMAND_SIZE];
static char rx_ready[COMMAND_SIZE];
static uint8_t rx_index;
static volatile bool command_ready;
static bool d300_ready;
static bool imu_ready;
static uint32_t control_at;
static uint32_t depth_at;
static uint32_t telemetry_at;

static bool d300_write(uint8_t address_7bit, const uint8_t *data, uint16_t length)
{
    return HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(address_7bit << 1),
                                   (uint8_t *)data, length, 100u) == HAL_OK;
}

static bool d300_read(uint8_t address_7bit, uint8_t *data, uint16_t length)
{
    return HAL_I2C_Master_Receive(&hi2c1, (uint16_t)(address_7bit << 1),
                                  data, length, 100u) == HAL_OK;
}

static void d300_delay(uint32_t milliseconds)
{
    HAL_Delay(milliseconds);
}

static bool imu_device_ready(void *context, uint8_t address)
{
    I2C_HandleTypeDef *i2c = context;
    return HAL_I2C_IsDeviceReady(i2c, (uint16_t)(address << 1), 3u, 30u) == HAL_OK;
}

static bool imu_write(void *context, uint8_t address, const uint8_t *data,
                      uint16_t length)
{
    I2C_HandleTypeDef *i2c = context;
    return HAL_I2C_Master_Transmit(i2c, (uint16_t)(address << 1),
                                   (uint8_t *)data, length, 50u) == HAL_OK;
}

static bool imu_read(void *context, uint8_t address, uint8_t *data,
                     uint16_t length)
{
    I2C_HandleTypeDef *i2c = context;
    return HAL_I2C_Master_Receive(i2c, (uint16_t)(address << 1),
                                  data, length, 50u) == HAL_OK;
}

static bool imu_mem_write(void *context, uint8_t address, uint8_t reg,
                          const uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = context;
    return HAL_I2C_Mem_Write(i2c, (uint16_t)(address << 1), reg,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)data,
                             length, 50u) == HAL_OK;
}

static bool imu_mem_read(void *context, uint8_t address, uint8_t reg,
                         uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = context;
    return HAL_I2C_Mem_Read(i2c, (uint16_t)(address << 1), reg,
                            I2C_MEMADD_SIZE_8BIT, data, length, 50u) == HAL_OK;
}

static void imu_delay(void *context, uint32_t milliseconds)
{
    (void)context;
    HAL_Delay(milliseconds);
}

static uint32_t imu_now_us(void *context)
{
    (void)context;
    return HAL_GetTick() * 1000u;
}

static uint32_t platform_now(void *context)
{
    (void)context;
    return HAL_GetTick();
}

static void platform_thruster(void *context, uint8_t index, float percent)
{
    (void)context;
    board_set_thruster_percent(index, percent);
}

static void platform_tank_target(void *context, uint8_t index, int32_t target)
{
    (void)context;
    board_set_tank_target(index, target);
}

static void platform_tank_speed(void *context, uint8_t index, uint32_t speed)
{
    (void)context;
    board_set_tank_speed(index, speed);
}

static void platform_tank_zero(void *context, uint8_t index)
{
    (void)context;
    board_zero_tank_position(index);
}

static void platform_arm(void *context, bool armed)
{
    (void)context;
    board_set_armed(armed);
}

/* Call once after MX_I2C1_Init() and MX_USART2_UART_Init(). */
bool seastars_app_init(void)
{
    const ms5837_bus_t bus = {
        .write = d300_write,
        .read = d300_read,
        .delay_ms = d300_delay,
    };
    const imu_i2c_bus_t imu_bus = {
        .context = &hi2c1,
        .device_ready = imu_device_ready,
        .write = imu_write,
        .read = imu_read,
        .mem_write = imu_mem_write,
        .mem_read = imu_mem_read,
        .delay_ms = imu_delay,
        .now_us = imu_now_us,
    };
    const seastars_platform_t platform = {
        .context = NULL,
        .now_ms = platform_now,
        .set_thruster_percent = platform_thruster,
        .set_tank_target = platform_tank_target,
        .set_armed = platform_arm,
        .set_tank_speed = platform_tank_speed,
        .zero_tank_position = platform_tank_zero,
    };

    d300_ready = ms5837_init(&d300, &bus);
    if (!seastars_runtime_init(&runtime, &platform, &d300)) {
        return false;
    }
    imu_ready = imu_i2c_init(&imu, &imu_bus);
    HAL_UART_Receive_IT(&huart2, &station_rx_byte, 1u);
    HAL_UART_Transmit(&huart2, (uint8_t *)"READY SEA_STARS_V3\r\n", 20u, 100u);
    control_at = depth_at = telemetry_at = HAL_GetTick();
    return true;
}

/*
 * Call from the project's single HAL_UART_RxCpltCallback(). Do not define a
 * second HAL callback: merge this dispatch with the existing callback.
 */
void seastars_app_uart_rx_complete(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        if (station_rx_byte == '\r' || station_rx_byte == '\n') {
            if (rx_index > 0u && !command_ready) {
                rx_build[rx_index] = '\0';
                memcpy(rx_ready, rx_build, (size_t)rx_index + 1u);
                command_ready = true;
            }
            rx_index = 0u;
        } else if (rx_index < COMMAND_SIZE - 1u) {
            rx_build[rx_index++] = (char)station_rx_byte;
        } else {
            rx_index = 0u;
        }
        HAL_UART_Receive_IT(&huart2, &station_rx_byte, 1u);
    }
}

static void process_pending_command(void)
{
    char command[COMMAND_SIZE];
    char reply[128];
    bool available;

    __disable_irq();
    available = command_ready;
    if (available) {
        memcpy(command, rx_ready, sizeof(command));
        command_ready = false;
    }
    __enable_irq();
    if (!available) return;

    (void)seastars_runtime_command(&runtime, command, reply, sizeof(reply));
    HAL_UART_Transmit(&huart2, (uint8_t *)reply, (uint16_t)strlen(reply), 100u);
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2u, 100u);
}

/* Call continuously from while (1). No movement loop contains HAL_Delay(). */
void seastars_app_poll(void)
{
    uint32_t now = HAL_GetTick();
    char telemetry[320];
    char positions[64];
    int length;

    process_pending_command();
    if (imu_ready && imu_i2c_poll(&imu, &imu_sample)) {
        seastars_runtime_set_imu(&runtime, &imu_sample);
    }
    seastars_runtime_set_tank_position(&runtime,
        board_get_tank_position(1u), board_get_tank_position(2u));

    if ((uint32_t)(now - depth_at) >= DEPTH_PERIOD_MS) {
        depth_at = now;
        if (d300_ready && ms5837_read(&d300, &depth_sample)) {
            seastars_runtime_set_depth(&runtime, &depth_sample);
        }
    }
    if ((uint32_t)(now - control_at) >= CONTROL_PERIOD_MS) {
        control_at = now;
        seastars_runtime_tick(&runtime);
    }
    if ((uint32_t)(now - telemetry_at) >= TELEMETRY_PERIOD_MS) {
        telemetry_at = now;
        length = snprintf(positions, sizeof(positions),
            "POS1:%ld POS2:%ld Z1:%u Z2:%u\r\n",
            (long)board_get_tank_position(1u),
            (long)board_get_tank_position(2u),
            runtime.input.tank_zeroed[0] ? 1u : 0u,
            runtime.input.tank_zeroed[1] ? 1u : 0u);
        if (length > 0) {
            HAL_UART_Transmit(&huart2, (uint8_t *)positions, (uint16_t)length, 100u);
        }
        length = seastars_runtime_format_telemetry(&runtime, telemetry, sizeof(telemetry));
        if (length > 0) {
            HAL_UART_Transmit(&huart2, (uint8_t *)telemetry, (uint16_t)length, 150u);
        }
    }
}

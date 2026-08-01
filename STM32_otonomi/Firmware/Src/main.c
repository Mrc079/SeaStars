#include "main.h"

#include "imu_i2c.h"
#include "ms5837.h"
#include "seastars_runtime.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FIRMWARE_VERSION              "3.4.1"
#define COMMAND_CAPACITY              96u
#define COMMAND_QUEUE_DEPTH           32u
#define TELEMETRY_CAPACITY            512u
#define ESC_NEUTRAL_US                1500u
#define ESC_PERCENT_SPAN_US           100.0f
#define ESC_NEUTRAL_BOOT_MS           3000u
#define CONTROL_PERIOD_MS             20u
#define TELEMETRY_PERIOD_MS           250u
#define DEPTH_SAMPLE_PERIOD_MS        100u
#define SENSOR_RETRY_PERIOD_MS        3000u
#define D300_I2C_CLOCK_HZ             10000u
#define D300_I2C_SPEED_LABEL          "10KHZ"
#define I2C_SCAN_FIRST_ADDRESS        0x08u
#define I2C_SCAN_LAST_ADDRESS         0x77u
#define I2C_SCAN_RESULT_CAPACITY      8u
#define I2C_RECOVERY_CLOCK_PULSES     16u
#define IMU_STARTUP_GRACE_MS          3500u
#define IMU_STALE_TIMEOUT_MS          1500u
#define MANUAL_LINK_TIMEOUT_MS        1500u
#define DEFAULT_TANK_SPEED_SPS        800u
#define TIMER_TICK_HZ                 1000000u

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim5;
UART_HandleTypeDef huart2;

typedef struct {
    TIM_HandleTypeDef *timer;
    GPIO_TypeDef *step_port;
    uint16_t step_pin;
    GPIO_TypeDef *direction_port;
    uint16_t direction_pin;
    volatile int32_t position;
    volatile int32_t target;
    volatile int8_t direction;
    volatile bool pulse_high;
    volatile bool active;
    uint32_t speed_sps;
} stepper_axis_t;

static stepper_axis_t g_stepper[2];
static ms5837_t g_depth_sensor;
static imu_i2c_t g_imu_sensor;
static seastars_runtime_t g_runtime;

static uint8_t g_uart2_rx_byte;
static char g_rx_buffer[COMMAND_CAPACITY];
static char g_command_queue[COMMAND_QUEUE_DEPTH][COMMAND_CAPACITY];
static volatile uint16_t g_rx_length;
static volatile uint8_t g_command_head;
static volatile uint8_t g_command_tail;

static bool g_depth_online;
static bool g_imu_online;
static bool g_depth_recovery_lines_high;
static bool g_imu_recovery_lines_high;
static uint8_t g_depth_failures;
static bool g_esc_armed;
static float g_thruster_request[2];
static uint32_t g_pwm_started_ms;
static uint32_t g_last_host_contact_ms;
static uint32_t g_imu_started_ms;
static uint32_t g_imu_last_sample_ms;

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_I2C2_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM5_Init(void);
static void MX_USART2_UART_Init(void);
static void platform_init(void);
static void uart_write(const char *text);
static void send_ready(void);
static void send_imu_status(void);
static void send_depth_status(void);
static uint8_t scan_i2c_bus(I2C_HandleTypeDef *i2c, uint8_t *addresses,
                            uint8_t capacity, uint8_t *stored_count);
static bool recover_i2c_bus(I2C_HandleTypeDef *i2c, GPIO_TypeDef *port,
                            uint16_t scl_pin, uint16_t sda_pin,
                            bool *lines_high);
static bool recover_d300_i2c(void);
static bool recover_imu_i2c(void);
static void process_pending_command(void);
static void service_depth_sensor(uint32_t now);
static void service_imu_sensor(uint32_t now);
static void service_manual_link_watchdog(uint32_t now);
static void publish_telemetry(void);
static void set_stepper_target(stepper_axis_t *axis, int32_t target);
static void set_stepper_speed(stepper_axis_t *axis, uint32_t steps_per_second);
static void stop_stepper(stepper_axis_t *axis);
static void stepper_interrupt(stepper_axis_t *axis);
static void apply_esc_outputs(void);

static uint32_t board_now_ms(void *context)
{
    (void)context;
    return HAL_GetTick();
}

static void board_set_thruster(void *context, uint8_t index, float percent)
{
    (void)context;
    if (index < 1u || index > 2u) return;
    if (percent > 100.0f) percent = 100.0f;
    if (percent < -100.0f) percent = -100.0f;
    g_thruster_request[index - 1u] = percent;
    apply_esc_outputs();
}

static void board_set_tank_target(void *context, uint8_t index, int32_t target)
{
    (void)context;
    if (index < 1u || index > 2u) return;
    set_stepper_target(&g_stepper[index - 1u], target);
}

static void board_set_armed(void *context, bool armed)
{
    (void)context;
    g_esc_armed = armed;
    if (!armed) {
        g_thruster_request[0] = 0.0f;
        g_thruster_request[1] = 0.0f;
    }
    HAL_GPIO_WritePin(STATUS_LED_GPIO_Port, STATUS_LED_Pin,
                      armed ? GPIO_PIN_SET : GPIO_PIN_RESET);
    apply_esc_outputs();
}

static void board_set_tank_speed(void *context, uint8_t index,
                                 uint32_t steps_per_second)
{
    (void)context;
    if (index < 1u || index > 2u) return;
    set_stepper_speed(&g_stepper[index - 1u], steps_per_second);
}

static void board_zero_tank(void *context, uint8_t index)
{
    stepper_axis_t *axis;
    uint32_t primask;
    (void)context;
    if (index < 1u || index > 2u) return;
    axis = &g_stepper[index - 1u];
    primask = __get_PRIMASK();
    __disable_irq();
    stop_stepper(axis);
    axis->position = 0;
    axis->target = 0;
    if (primask == 0u) __enable_irq();
}

static bool d300_write(uint8_t address_7bit, const uint8_t *data, uint16_t length)
{
    return HAL_I2C_Master_Transmit(&hi2c2, (uint16_t)(address_7bit << 1),
                                   (uint8_t *)data, length, 100u) == HAL_OK;
}

static bool d300_read(uint8_t address_7bit, uint8_t *data, uint16_t length)
{
    return HAL_I2C_Master_Receive(&hi2c2, (uint16_t)(address_7bit << 1),
                                  data, length, 100u) == HAL_OK;
}

static void d300_delay(uint32_t milliseconds)
{
    HAL_Delay(milliseconds);
}

static bool imu_device_ready(void *context, uint8_t address_7bit)
{
    I2C_HandleTypeDef *i2c = (I2C_HandleTypeDef *)context;
    return HAL_I2C_IsDeviceReady(i2c, (uint16_t)(address_7bit << 1),
                                 3u, 30u) == HAL_OK;
}

static bool imu_write(void *context, uint8_t address_7bit,
                      const uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = (I2C_HandleTypeDef *)context;
    return HAL_I2C_Master_Transmit(i2c, (uint16_t)(address_7bit << 1),
                                   (uint8_t *)data, length, 50u) == HAL_OK;
}

static bool imu_read(void *context, uint8_t address_7bit,
                     uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = (I2C_HandleTypeDef *)context;
    return HAL_I2C_Master_Receive(i2c, (uint16_t)(address_7bit << 1),
                                  data, length, 50u) == HAL_OK;
}

static bool imu_mem_write(void *context, uint8_t address_7bit, uint8_t reg,
                          const uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = (I2C_HandleTypeDef *)context;
    return HAL_I2C_Mem_Write(i2c, (uint16_t)(address_7bit << 1), reg,
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)data,
                             length, 50u) == HAL_OK;
}

static bool imu_mem_read(void *context, uint8_t address_7bit, uint8_t reg,
                         uint8_t *data, uint16_t length)
{
    I2C_HandleTypeDef *i2c = (I2C_HandleTypeDef *)context;
    return HAL_I2C_Mem_Read(i2c, (uint16_t)(address_7bit << 1), reg,
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

int main(void)
{
    uint32_t last_control_ms;
    uint32_t last_telemetry_ms;
    ms5837_bus_t depth_bus = {d300_write, d300_read, d300_delay};
    imu_i2c_bus_t imu_bus = {
        .context = &hi2c1,
        .device_ready = imu_device_ready,
        .write = imu_write,
        .read = imu_read,
        .mem_write = imu_mem_write,
        .mem_read = imu_mem_read,
        .delay_ms = imu_delay,
        .now_us = imu_now_us,
    };
    seastars_platform_t platform = {
        .context = NULL,
        .now_ms = board_now_ms,
        .set_thruster_percent = board_set_thruster,
        .set_tank_target = board_set_tank_target,
        .set_armed = board_set_armed,
        .set_tank_speed = board_set_tank_speed,
        .zero_tank_position = board_zero_tank,
    };

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_TIM2_Init();
    MX_TIM3_Init();
    MX_TIM5_Init();
    MX_USART2_UART_Init();
    MX_I2C1_Init();
    MX_I2C2_Init();
    platform_init();

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ESC_NEUTRAL_US);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, ESC_NEUTRAL_US);
    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2) != HAL_OK) {
        Error_Handler();
    }
    g_pwm_started_ms = HAL_GetTick();

    g_depth_online = recover_d300_i2c() &&
                     ms5837_init(&g_depth_sensor, &depth_bus);
    if (!seastars_runtime_init(&g_runtime, &platform, &g_depth_sensor)) {
        Error_Handler();
    }
    g_imu_online = recover_imu_i2c() &&
                   imu_i2c_init(&g_imu_sensor, &imu_bus);
    g_imu_started_ms = HAL_GetTick();
    g_last_host_contact_ms = HAL_GetTick();

    if (HAL_UART_Receive_IT(&huart2, &g_uart2_rx_byte, 1u) != HAL_OK) {
        Error_Handler();
    }

    send_ready();
    send_depth_status();
    send_imu_status();

    last_control_ms = HAL_GetTick();
    last_telemetry_ms = HAL_GetTick();
    while (1) {
        uint32_t now = HAL_GetTick();

        seastars_runtime_set_tank_position(
            &g_runtime, g_stepper[0].position, g_stepper[1].position);
        process_pending_command();
        service_imu_sensor(now);
        service_depth_sensor(now);
        seastars_runtime_set_tank_position(
            &g_runtime, g_stepper[0].position, g_stepper[1].position);

        if ((uint32_t)(now - last_control_ms) >= CONTROL_PERIOD_MS) {
            last_control_ms = now;
            seastars_runtime_tick(&g_runtime);
        }
        service_manual_link_watchdog(now);
        apply_esc_outputs();

        if ((uint32_t)(now - last_telemetry_ms) >= TELEMETRY_PERIOD_MS) {
            last_telemetry_ms = now;
            publish_telemetry();
        }
    }
}

static void platform_init(void)
{
    memset(g_stepper, 0, sizeof(g_stepper));
    g_stepper[0].timer = &htim2;
    g_stepper[0].step_port = STEP1_GPIO_Port;
    g_stepper[0].step_pin = STEP1_Pin;
    g_stepper[0].direction_port = DIR1_GPIO_Port;
    g_stepper[0].direction_pin = DIR1_Pin;
    g_stepper[0].direction = 1;
    g_stepper[0].speed_sps = DEFAULT_TANK_SPEED_SPS;

    g_stepper[1].timer = &htim5;
    g_stepper[1].step_port = STEP2_GPIO_Port;
    g_stepper[1].step_pin = STEP2_Pin;
    g_stepper[1].direction_port = DIR2_GPIO_Port;
    g_stepper[1].direction_pin = DIR2_Pin;
    g_stepper[1].direction = 1;
    g_stepper[1].speed_sps = DEFAULT_TANK_SPEED_SPS;
}

static void uart_write(const char *text)
{
    if (text == NULL) return;
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)text,
                            (uint16_t)strlen(text), 150u);
}

static void send_ready(void)
{
    uart_write("READY SEA_STARS V:" FIRMWARE_VERSION
               " BOARD:NUCLEO-L152RE SAFE:DISARMED\r\n");
}

static void send_imu_status(void)
{
    char status[80];
    if (!g_imu_online) {
        snprintf(status, sizeof(status),
                 "SENSOR IMU:NOT_FOUND I2C:AUTO RECOVERY:%s\r\n",
                 g_imu_recovery_lines_high ? "LINES_HIGH" : "LINES_LOW");
        uart_write(status);
        return;
    }
    snprintf(status, sizeof(status), "SENSOR IMU:%s ADDR:0x%02X\r\n",
             imu_i2c_name(&g_imu_sensor),
             (unsigned)g_imu_sensor.address_7bit);
    uart_write(status);
}

static void send_depth_status(void)
{
    char status[192];
    char scan_text[64];
    uint8_t addresses[I2C_SCAN_RESULT_CAPACITY];
    uint8_t stored_count = 0u;
    uint8_t found_count;
    uint8_t index;
    size_t used = 0u;
    uint32_t init_hal_error;

    if (g_depth_online) {
        uart_write("SENSOR D300:READY ADDR:0x76 BUS:I2C2 PINS:PB10/PB11 "
                   "SPEED:" D300_I2C_SPEED_LABEL "\r\n");
        return;
    }

    /* Preserve the error left by ms5837_init before active probing changes it. */
    init_hal_error = HAL_I2C_GetError(&hi2c2);
    if (HAL_I2C_IsDeviceReady(&hi2c2,
                              (uint16_t)(MS5837_I2C_ADDRESS_7BIT << 1),
                              2u, 20u) == HAL_OK) {
        snprintf(status, sizeof(status),
                 "SENSOR D300:INIT_FAILED ADDR:0x76 ACK:YES "
                 "CAUSE:%s BUS:I2C2 SPEED:"
                 D300_I2C_SPEED_LABEL " RECOVERY:%s HAL:0x%08lX\r\n",
                 ms5837_error_name(g_depth_sensor.last_error),
                 g_depth_recovery_lines_high ? "LINES_HIGH" : "LINES_LOW",
                 (unsigned long)init_hal_error);
        uart_write(status);
        return;
    }

    found_count = scan_i2c_bus(&hi2c2, addresses,
                               I2C_SCAN_RESULT_CAPACITY, &stored_count);
    if (found_count == 0u) {
        snprintf(status, sizeof(status),
                 "SENSOR D300:NO_ACK EXPECTED:0x76 SCAN:NONE "
                 "BUS:I2C2 PINS:PB10/PB11 SPEED:"
                 D300_I2C_SPEED_LABEL " STAGE:%s RECOVERY:%s "
                 "HAL:0x%08lX\r\n",
                 ms5837_error_name(g_depth_sensor.last_error),
                 g_depth_recovery_lines_high ? "LINES_HIGH" : "LINES_LOW",
                 (unsigned long)init_hal_error);
        uart_write(status);
        return;
    }

    scan_text[0] = '\0';
    for (index = 0u; index < stored_count; ++index) {
        int written = snprintf(scan_text + used, sizeof(scan_text) - used,
                               "%s0x%02X", index == 0u ? "" : ",",
                               (unsigned)addresses[index]);
        if (written < 0 || (size_t)written >= sizeof(scan_text) - used) break;
        used += (size_t)written;
    }
    snprintf(status, sizeof(status),
             "SENSOR D300:NO_ACK EXPECTED:0x76 SCAN:%s%s "
             "BUS:I2C2 SPEED:" D300_I2C_SPEED_LABEL
             " STAGE:%s RECOVERY:%s HAL:0x%08lX\r\n",
             scan_text,
             found_count > stored_count ? ",MORE" : "",
             ms5837_error_name(g_depth_sensor.last_error),
             g_depth_recovery_lines_high ? "LINES_HIGH" : "LINES_LOW",
             (unsigned long)init_hal_error);
    uart_write(status);
}

static uint8_t scan_i2c_bus(I2C_HandleTypeDef *i2c, uint8_t *addresses,
                            uint8_t capacity, uint8_t *stored_count)
{
    uint8_t address;
    uint8_t found = 0u;
    uint8_t stored = 0u;

    if (i2c == NULL || stored_count == NULL) return 0u;
    for (address = I2C_SCAN_FIRST_ADDRESS;
         address <= I2C_SCAN_LAST_ADDRESS; ++address) {
        if (HAL_I2C_IsDeviceReady(i2c, (uint16_t)(address << 1),
                                  1u, 5u) == HAL_OK) {
            if (addresses != NULL && stored < capacity) {
                addresses[stored++] = address;
            }
            if (found < UINT8_MAX) ++found;
        }
    }
    *stored_count = stored;
    return found;
}

static bool recover_d300_i2c(void)
{
    return recover_i2c_bus(&hi2c2, GPIOB,
                           D300_I2C_SCL_Pin, D300_I2C_SDA_Pin,
                           &g_depth_recovery_lines_high);
}

static bool recover_imu_i2c(void)
{
    return recover_i2c_bus(&hi2c1, GPIOB,
                           IMU_I2C_SCL_Pin, IMU_I2C_SDA_Pin,
                           &g_imu_recovery_lines_high);
}

static bool recover_i2c_bus(I2C_HandleTypeDef *i2c, GPIO_TypeDef *port,
                            uint16_t scl_pin, uint16_t sda_pin,
                            bool *lines_high)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t pulse;
    GPIO_PinState scl_state;
    GPIO_PinState sda_state;

    if (i2c == NULL || port == NULL || lines_high == NULL) return false;
    (void)HAL_I2C_DeInit(i2c);
    if (i2c->Instance == I2C1) {
        __HAL_RCC_I2C1_FORCE_RESET();
        __HAL_RCC_I2C1_RELEASE_RESET();
    } else if (i2c->Instance == I2C2) {
        __HAL_RCC_I2C2_FORCE_RESET();
        __HAL_RCC_I2C2_RELEASE_RESET();
    } else {
        return false;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(port, scl_pin | sda_pin, GPIO_PIN_SET);
    gpio.Pin = scl_pin | sda_pin;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
    HAL_Delay(1u);

    for (pulse = 0u; pulse < I2C_RECOVERY_CLOCK_PULSES; ++pulse) {
        HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_RESET);
        HAL_Delay(1u);
        HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_SET);
        HAL_Delay(1u);
    }

    HAL_GPIO_WritePin(port, sda_pin, GPIO_PIN_RESET);
    HAL_Delay(1u);
    HAL_GPIO_WritePin(port, scl_pin, GPIO_PIN_SET);
    HAL_Delay(1u);
    HAL_GPIO_WritePin(port, sda_pin, GPIO_PIN_SET);
    HAL_Delay(1u);

    scl_state = HAL_GPIO_ReadPin(port, scl_pin);
    sda_state = HAL_GPIO_ReadPin(port, sda_pin);
    *lines_high = scl_state == GPIO_PIN_SET && sda_state == GPIO_PIN_SET;

    HAL_GPIO_DeInit(port, scl_pin | sda_pin);
    return HAL_I2C_Init(i2c) == HAL_OK;
}

static void normalize_command(char *command)
{
    size_t start = 0u;
    size_t length;
    size_t index;
    if (command == NULL) return;
    length = strlen(command);
    while (start < length && isspace((unsigned char)command[start])) start++;
    if (start > 0u) {
        memmove(command, command + start, length - start + 1u);
        length -= start;
    }
    while (length > 0u && isspace((unsigned char)command[length - 1u])) {
        command[--length] = '\0';
    }
    for (index = 0u; index < length; ++index) {
        command[index] = (char)toupper((unsigned char)command[index]);
    }
}

static void process_pending_command(void)
{
    char command[COMMAND_CAPACITY];
    char reply[128];
    uint32_t primask;
    if (g_command_tail == g_command_head) return;

    primask = __get_PRIMASK();
    __disable_irq();
    memcpy(command, g_command_queue[g_command_tail], sizeof(command));
    g_command_tail = (uint8_t)((g_command_tail + 1u) % COMMAND_QUEUE_DEPTH);
    if (primask == 0u) __enable_irq();
    command[sizeof(command) - 1u] = '\0';
    normalize_command(command);
    if (command[0] == '\0') return;
    g_last_host_contact_ms = HAL_GetTick();

    if (!strcmp(command, "HELLO")) {
        send_ready();
        send_depth_status();
        send_imu_status();
        return;
    }
    if (!strcmp(command, "PING")) {
        return;
    }
    if (!strcmp(command, "HELP")) {
        uart_write("CMDS HELLO PING STOP ARM DISARM T1/T2 S1/S2 V1/V2 "
                   "AUTO_SCHEDULE AUTO_PREPARE "
                   "ZERO1/2 POS CFG DEPTH_ZERO IMU_ZERO AUTO TURN\r\n");
        return;
    }
    if (!strcmp(command, "STATUS")) {
        publish_telemetry();
        return;
    }
    (void)seastars_runtime_command(&g_runtime, command, reply, sizeof(reply));
    uart_write(reply);
    uart_write("\r\n");
}

static void service_imu_sensor(uint32_t now)
{
    static uint32_t last_retry_ms;
    imu_sample_t sample;

    if (!g_imu_online) {
        if ((uint32_t)(now - last_retry_ms) < SENSOR_RETRY_PERIOD_MS) return;
        last_retry_ms = now;
        if (!recover_imu_i2c()) return;
        g_imu_online = imu_i2c_init(&g_imu_sensor, &g_imu_sensor.bus);
        if (g_imu_online) {
            g_imu_started_ms = now;
            g_imu_last_sample_ms = 0u;
            send_imu_status();
        }
        return;
    }
    if (imu_i2c_poll(&g_imu_sensor, &sample)) {
        g_imu_last_sample_ms = now;
        seastars_runtime_set_imu(&g_runtime, &sample);
        return;
    }
    if (!g_imu_sensor.initialized ||
        ((uint32_t)(now - g_imu_started_ms) > IMU_STARTUP_GRACE_MS &&
         (g_imu_last_sample_ms == 0u ||
          (uint32_t)(now - g_imu_last_sample_ms) > IMU_STALE_TIMEOUT_MS))) {
        imu_i2c_deinit(&g_imu_sensor);
        g_imu_online = false;
        last_retry_ms = now;
        uart_write("SENSOR IMU:LOST\r\n");
    }
}

static void service_depth_sensor(uint32_t now)
{
    static uint32_t last_sample_ms;
    static uint32_t last_retry_ms;
    ms5837_sample_t sample;

    if (!g_depth_online) {
        if ((uint32_t)(now - last_retry_ms) < SENSOR_RETRY_PERIOD_MS) return;
        last_retry_ms = now;
        if (!recover_d300_i2c()) return;
        {
            float density = g_depth_sensor.fluid_density_kg_m3;
            float offset = g_depth_sensor.sensor_to_reference_offset_m;
            ms5837_bus_t bus = {d300_write, d300_read, d300_delay};
            g_depth_online = ms5837_init(&g_depth_sensor, &bus);
            if (density >= 950.0f && density <= 1100.0f) {
                ms5837_set_fluid_density(&g_depth_sensor, density);
            }
            ms5837_set_mount_offset(&g_depth_sensor, offset);
        }
        if (g_depth_online) {
            g_depth_failures = 0u;
            uart_write("SENSOR D300:READY CALIBRATION_REQUIRED\r\n");
        }
        return;
    }
    if ((uint32_t)(now - last_sample_ms) < DEPTH_SAMPLE_PERIOD_MS) return;
    last_sample_ms = now;
    if (ms5837_read(&g_depth_sensor, &sample)) {
        g_depth_failures = 0u;
        seastars_runtime_set_depth(&g_runtime, &sample);
    } else if (++g_depth_failures >= 3u) {
        g_depth_online = false;
        g_depth_sensor.initialized = false;
        uart_write("SENSOR D300:LOST\r\n");
    }
}

static void service_manual_link_watchdog(uint32_t now)
{
    char reply[48];
    if (g_runtime.armed && !g_runtime.autonomy.running &&
        (uint32_t)(now - g_last_host_contact_ms) > MANUAL_LINK_TIMEOUT_MS) {
        (void)seastars_runtime_command(&g_runtime, "DISARM", reply, sizeof(reply));
        uart_write("SAFETY MANUAL_LINK_TIMEOUT DISARMED\r\n");
        g_last_host_contact_ms = now;
    }
}

static void publish_telemetry(void)
{
    char position[96];
    char telemetry[TELEMETRY_CAPACITY];
    int length;
    snprintf(position, sizeof(position),
             "POS1:%ld POS2:%ld Z1:%u Z2:%u\r\n",
             (long)g_stepper[0].position, (long)g_stepper[1].position,
             g_runtime.input.tank_zeroed[0] ? 1u : 0u,
             g_runtime.input.tank_zeroed[1] ? 1u : 0u);
    uart_write(position);
    length = seastars_runtime_format_telemetry(
        &g_runtime, telemetry, sizeof(telemetry));
    if (length > 0) uart_write(telemetry);
}

static void stop_stepper(stepper_axis_t *axis)
{
    if (axis == NULL || axis->timer == NULL) return;
    (void)HAL_TIM_Base_Stop_IT(axis->timer);
    HAL_GPIO_WritePin(axis->step_port, axis->step_pin, GPIO_PIN_RESET);
    axis->pulse_high = false;
    axis->active = false;
}

static void set_stepper_target(stepper_axis_t *axis, int32_t target)
{
    uint32_t primask;
    if (axis == NULL) return;
    primask = __get_PRIMASK();
    __disable_irq();
    stop_stepper(axis);
    axis->target = target;
    if (axis->target != axis->position) {
        axis->direction = (axis->target > axis->position) ? 1 : -1;
        HAL_GPIO_WritePin(axis->direction_port, axis->direction_pin,
                          axis->direction > 0 ? GPIO_PIN_SET : GPIO_PIN_RESET);
        __HAL_TIM_SET_COUNTER(axis->timer, 0u);
        __HAL_TIM_CLEAR_FLAG(axis->timer, TIM_FLAG_UPDATE);
        axis->active = HAL_TIM_Base_Start_IT(axis->timer) == HAL_OK;
    }
    if (primask == 0u) __enable_irq();
}

static void set_stepper_speed(stepper_axis_t *axis, uint32_t steps_per_second)
{
    uint32_t period_ticks;
    uint32_t primask;
    if (axis == NULL || steps_per_second < 50u || steps_per_second > 2000u) return;
    period_ticks = TIMER_TICK_HZ / (2u * steps_per_second);
    if (period_ticks < 2u) period_ticks = 2u;
    primask = __get_PRIMASK();
    __disable_irq();
    axis->speed_sps = steps_per_second;
    __HAL_TIM_SET_AUTORELOAD(axis->timer, period_ticks - 1u);
    __HAL_TIM_SET_COUNTER(axis->timer, 0u);
    if (primask == 0u) __enable_irq();
}

static void stepper_interrupt(stepper_axis_t *axis)
{
    if (axis == NULL || !axis->active) return;
    if (!axis->pulse_high) {
        if (axis->position == axis->target) {
            stop_stepper(axis);
            return;
        }
        HAL_GPIO_WritePin(axis->step_port, axis->step_pin, GPIO_PIN_SET);
        axis->pulse_high = true;
        return;
    }
    HAL_GPIO_WritePin(axis->step_port, axis->step_pin, GPIO_PIN_RESET);
    axis->pulse_high = false;
    axis->position += axis->direction;
    if (axis->position == axis->target) stop_stepper(axis);
}

static void apply_esc_outputs(void)
{
    uint32_t now = HAL_GetTick();
    bool ready = g_esc_armed &&
        (uint32_t)(now - g_pwm_started_ms) >= ESC_NEUTRAL_BOOT_MS;
    float left = ready ? g_thruster_request[0] : 0.0f;
    float right = ready ? g_thruster_request[1] : 0.0f;
    uint32_t pulse1 = (uint32_t)(ESC_NEUTRAL_US + left * ESC_PERCENT_SPAN_US / 100.0f);
    uint32_t pulse2 = (uint32_t)(ESC_NEUTRAL_US + right * ESC_PERCENT_SPAN_US / 100.0f);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse1);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, pulse2);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2) {
        stepper_interrupt(&g_stepper[0]);
    } else if (htim->Instance == TIM5) {
        stepper_interrupt(&g_stepper[1]);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        char received = (char)g_uart2_rx_byte;
        if (received == '\r' || received == '\n') {
            if (g_rx_length > 0u) {
                uint8_t next_head = (uint8_t)((g_command_head + 1u) % COMMAND_QUEUE_DEPTH);
                g_rx_buffer[g_rx_length] = '\0';
                if (next_head != g_command_tail) {
                    memcpy(g_command_queue[g_command_head], g_rx_buffer, g_rx_length + 1u);
                    __DMB();
                    g_command_head = next_head;
                }
                g_rx_length = 0u;
            }
        } else if (g_rx_length < COMMAND_CAPACITY - 1u) {
            g_rx_buffer[g_rx_length++] = received;
        } else {
            g_rx_length = 0u;
        }
        (void)HAL_UART_Receive_IT(&huart2, &g_uart2_rx_byte, 1u);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        (void)HAL_UART_Receive_IT(&huart2, &g_uart2_rx_byte, 1u);
    }
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_VOS) != RESET) {}

    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL6;
    oscillator.PLL.PLLDIV = RCC_PLL_DIV3;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) Error_Handler();

    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV1;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, STEP1_Pin | STATUS_LED_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, DIR1_Pin | STEP2_Pin | DIR2_Pin, GPIO_PIN_RESET);

    gpio.Pin = STEP1_Pin | STATUS_LED_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = DIR1_Pin | STEP2_Pin | DIR2_Pin;
    HAL_GPIO_Init(GPIOB, &gpio);
}

static void MX_I2C1_Init(void)
{
    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000u;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0u;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0u;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
}

static void MX_I2C2_Init(void)
{
    hi2c2.Instance = I2C2;
    hi2c2.Init.ClockSpeed = D300_I2C_CLOCK_HZ;
    hi2c2.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c2.Init.OwnAddress1 = 0u;
    hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c2.Init.OwnAddress2 = 0u;
    hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c2) != HAL_OK) Error_Handler();
}

static void init_step_timer(TIM_HandleTypeDef *timer, TIM_TypeDef *instance)
{
    timer->Instance = instance;
    timer->Init.Prescaler = 31u;
    timer->Init.CounterMode = TIM_COUNTERMODE_UP;
    timer->Init.Period = 624u;
    timer->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    timer->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(timer) != HAL_OK) Error_Handler();
}

static void MX_TIM2_Init(void)
{
    init_step_timer(&htim2, TIM2);
}

static void MX_TIM5_Init(void)
{
    init_step_timer(&htim5, TIM5);
}

static void MX_TIM3_Init(void)
{
    TIM_MasterConfigTypeDef master = {0};
    TIM_OC_InitTypeDef output = {0};
    htim3.Instance = TIM3;
    htim3.Init.Prescaler = 31u;
    htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3.Init.Period = 19999u;
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim3) != HAL_OK) Error_Handler();
    master.MasterOutputTrigger = TIM_TRGO_RESET;
    master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &master) != HAL_OK) {
        Error_Handler();
    }
    output.OCMode = TIM_OCMODE_PWM1;
    output.Pulse = ESC_NEUTRAL_US;
    output.OCPolarity = TIM_OCPOLARITY_HIGH;
    output.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&htim3, &output, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_PWM_ConfigChannel(&htim3, &output, TIM_CHANNEL_2) != HAL_OK) {
        Error_Handler();
    }
    HAL_TIM_MspPostInit(&htim3);
}

static void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200u;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

void Error_Handler(void)
{
    __disable_irq();
    TIM3->CCR1 = ESC_NEUTRAL_US;
    TIM3->CCR2 = ESC_NEUTRAL_US;
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM5->CR1 &= ~TIM_CR1_CEN;
    STEP1_GPIO_Port->BSRR = (uint32_t)STEP1_Pin << 16u;
    STEP2_GPIO_Port->BSRR = (uint32_t)STEP2_Pin << 16u;
    STATUS_LED_GPIO_Port->BSRR = (uint32_t)STATUS_LED_Pin << 16u;
    while (1) {}
}

int __io_putchar(int ch)
{
    uint8_t byte = (uint8_t)ch;
    (void)HAL_UART_Transmit(&huart2, &byte, 1u, 100u);
    return ch;
}

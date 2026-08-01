#ifndef SEASTARS_IMU_I2C_H
#define SEASTARS_IMU_I2C_H

#include "imu_sample.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    IMU_I2C_NONE = 0,
    IMU_I2C_BNO055,
    IMU_I2C_BNO085,
} imu_i2c_kind_t;

typedef struct {
    void *context;
    bool (*device_ready)(void *context, uint8_t address_7bit);
    bool (*write)(void *context, uint8_t address_7bit,
                  const uint8_t *data, uint16_t length);
    bool (*read)(void *context, uint8_t address_7bit,
                 uint8_t *data, uint16_t length);
    bool (*mem_write)(void *context, uint8_t address_7bit, uint8_t reg,
                      const uint8_t *data, uint16_t length);
    bool (*mem_read)(void *context, uint8_t address_7bit, uint8_t reg,
                     uint8_t *data, uint16_t length);
    void (*delay_ms)(void *context, uint32_t milliseconds);
    uint32_t (*now_us)(void *context);
} imu_i2c_bus_t;

typedef struct {
    imu_i2c_bus_t bus;
    imu_i2c_kind_t kind;
    uint8_t address_7bit;
    bool initialized;
    bool sample_ready;
    bool reset_seen;
    uint32_t last_poll_us;
    uint32_t valid_samples;
    imu_sample_t latest_sample;
} imu_i2c_t;

/* Only one BNO08x SH-2 instance can be active at a time. */
bool imu_i2c_init(imu_i2c_t *imu, const imu_i2c_bus_t *bus);
void imu_i2c_deinit(imu_i2c_t *imu);
bool imu_i2c_poll(imu_i2c_t *imu, imu_sample_t *sample);
const char *imu_i2c_name(const imu_i2c_t *imu);

#endif

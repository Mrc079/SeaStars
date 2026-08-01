#include "imu_i2c.h"

#include "sh2.h"
#include "sh2_SensorValue.h"
#include "sh2_err.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define BNO055_ADDRESS_A             0x28u
#define BNO055_ADDRESS_B             0x29u
#define BNO055_CHIP_ID_REG           0x00u
#define BNO055_CHIP_ID               0xA0u
#define BNO055_PAGE_ID_REG           0x07u
#define BNO055_EULER_H_LSB_REG       0x1Au
#define BNO055_CALIB_STAT_REG        0x35u
#define BNO055_UNIT_SEL_REG          0x3Bu
#define BNO055_OPR_MODE_REG          0x3Du
#define BNO055_PWR_MODE_REG          0x3Eu
#define BNO055_SYS_TRIGGER_REG       0x3Fu
#define BNO055_MODE_CONFIG           0x00u
#define BNO055_MODE_NDOF             0x0Cu

#define BNO085_ADDRESS_A             0x4Au
#define BNO085_ADDRESS_B             0x4Bu
#define BNO085_REPORT_INTERVAL_US    20000u
#define BNO085_I2C_CHUNK_SIZE        32u
#define BNO085_SERVICE_INTERVAL_US   5000u
#define BNO055_SAMPLE_INTERVAL_US    50000u
#define RADIANS_TO_DEGREES           57.29577951308232f

static imu_i2c_t *g_active_bno085;
static sh2_Hal_t g_sh2_hal;

static bool bus_is_valid(const imu_i2c_bus_t *bus)
{
    return bus != NULL && bus->device_ready != NULL && bus->write != NULL &&
           bus->read != NULL && bus->mem_write != NULL &&
           bus->mem_read != NULL && bus->delay_ms != NULL &&
           bus->now_us != NULL;
}

static uint16_t minimum_u16(uint16_t left, uint16_t right)
{
    return left < right ? left : right;
}

static bool write_register(imu_i2c_t *imu, uint8_t reg, uint8_t value)
{
    return imu->bus.mem_write(imu->bus.context, imu->address_7bit,
                              reg, &value, 1u);
}

static bool read_register(imu_i2c_t *imu, uint8_t reg, uint8_t *value)
{
    return imu->bus.mem_read(imu->bus.context, imu->address_7bit,
                             reg, value, 1u);
}

static bool init_bno055_at(imu_i2c_t *imu, uint8_t address)
{
    uint8_t chip_id = 0u;

    if (!imu->bus.device_ready(imu->bus.context, address)) return false;
    imu->address_7bit = address;
    if (!read_register(imu, BNO055_CHIP_ID_REG, &chip_id) ||
        chip_id != BNO055_CHIP_ID) {
        return false;
    }
    if (!write_register(imu, BNO055_OPR_MODE_REG, BNO055_MODE_CONFIG)) return false;
    imu->bus.delay_ms(imu->bus.context, 25u);
    if (!write_register(imu, BNO055_PAGE_ID_REG, 0u) ||
        !write_register(imu, BNO055_SYS_TRIGGER_REG, 0u)) return false;
    imu->bus.delay_ms(imu->bus.context, 10u);
    if (!write_register(imu, BNO055_PWR_MODE_REG, 0u)) return false;
    imu->bus.delay_ms(imu->bus.context, 10u);
    if (!write_register(imu, BNO055_UNIT_SEL_REG, 0u)) return false;
    imu->bus.delay_ms(imu->bus.context, 10u);
    if (!write_register(imu, BNO055_OPR_MODE_REG, BNO055_MODE_NDOF)) return false;
    imu->bus.delay_ms(imu->bus.context, 30u);
    imu->kind = IMU_I2C_BNO055;
    imu->initialized = true;
    imu->last_poll_us = imu->bus.now_us(imu->bus.context) - BNO055_SAMPLE_INTERVAL_US;
    return true;
}

static bool poll_bno055(imu_i2c_t *imu, imu_sample_t *sample)
{
    uint8_t raw[6];
    uint8_t calibration = 0u;
    int16_t heading;
    int16_t roll;
    int16_t pitch;

    if (!imu->bus.mem_read(imu->bus.context, imu->address_7bit,
                           BNO055_EULER_H_LSB_REG, raw, sizeof(raw))) {
        return false;
    }
    (void)read_register(imu, BNO055_CALIB_STAT_REG, &calibration);
    heading = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    roll = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    pitch = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    memset(sample, 0, sizeof(*sample));
    sample->yaw_deg = (float)heading / 16.0f;
    sample->roll_deg = (float)roll / 16.0f;
    sample->pitch_deg = (float)pitch / 16.0f;
    sample->calibration = calibration;
    sample->sequence = (uint8_t)imu->valid_samples;
    imu->valid_samples++;
    return true;
}

static int bno085_hal_open(sh2_Hal_t *self)
{
    static const uint8_t soft_reset[] = {5u, 0u, 1u, 0u, 1u};
    uint8_t attempt;
    (void)self;
    if (g_active_bno085 == NULL) return -1;
    for (attempt = 0u; attempt < 5u; ++attempt) {
        if (g_active_bno085->bus.write(
                g_active_bno085->bus.context,
                g_active_bno085->address_7bit,
                soft_reset, sizeof(soft_reset))) {
            g_active_bno085->bus.delay_ms(g_active_bno085->bus.context, 300u);
            return 0;
        }
        g_active_bno085->bus.delay_ms(g_active_bno085->bus.context, 30u);
    }
    return -1;
}

static void bno085_hal_close(sh2_Hal_t *self)
{
    (void)self;
}

static int bno085_hal_read(sh2_Hal_t *self, uint8_t *buffer, unsigned length,
                           uint32_t *timestamp_us)
{
    uint8_t header[4];
    uint8_t chunk[BNO085_I2C_CHUNK_SIZE];
    uint16_t packet_size;
    uint16_t remaining;
    bool first_read = true;
    (void)self;

    if (g_active_bno085 == NULL || buffer == NULL || length < 4u) return 0;
    if (!g_active_bno085->bus.read(g_active_bno085->bus.context,
                                   g_active_bno085->address_7bit,
                                   header, sizeof(header))) {
        return 0;
    }
    if (timestamp_us != NULL) {
        *timestamp_us = g_active_bno085->bus.now_us(g_active_bno085->bus.context);
    }
    packet_size = (uint16_t)((uint16_t)header[0] |
                             ((uint16_t)header[1] << 8));
    packet_size &= 0x7FFFu;
    if (packet_size < 4u || packet_size > length) return 0;

    remaining = packet_size;
    while (remaining > 0u) {
        uint16_t request_size;
        uint16_t copied;
        if (first_read) {
            request_size = minimum_u16(BNO085_I2C_CHUNK_SIZE, remaining);
        } else {
            request_size = minimum_u16(BNO085_I2C_CHUNK_SIZE,
                                       (uint16_t)(remaining + 4u));
        }
        if (!g_active_bno085->bus.read(g_active_bno085->bus.context,
                                       g_active_bno085->address_7bit,
                                       chunk, request_size)) {
            return 0;
        }
        if (first_read) {
            copied = request_size;
            memcpy(buffer, chunk, copied);
            first_read = false;
        } else {
            if (request_size <= 4u) return 0;
            copied = (uint16_t)(request_size - 4u);
            memcpy(buffer, chunk + 4u, copied);
        }
        buffer += copied;
        remaining = (uint16_t)(remaining - copied);
    }
    return (int)packet_size;
}

static int bno085_hal_write(sh2_Hal_t *self, uint8_t *buffer, unsigned length)
{
    (void)self;
    if (g_active_bno085 == NULL || buffer == NULL || length == 0u ||
        length > UINT16_MAX) {
        return -1;
    }
    if (!g_active_bno085->bus.write(g_active_bno085->bus.context,
                                    g_active_bno085->address_7bit,
                                    buffer, (uint16_t)length)) {
        return -1;
    }
    return (int)length;
}

static uint32_t bno085_hal_now_us(sh2_Hal_t *self)
{
    (void)self;
    if (g_active_bno085 == NULL) return 0u;
    return g_active_bno085->bus.now_us(g_active_bno085->bus.context);
}

static bool configure_bno085_report(void)
{
    sh2_SensorConfig_t config;
    memset(&config, 0, sizeof(config));
    config.reportInterval_us = BNO085_REPORT_INTERVAL_US;
    return sh2_setSensorConfig(SH2_ROTATION_VECTOR, &config) == SH2_OK;
}

static float clamp_unit(float value)
{
    if (value > 1.0f) return 1.0f;
    if (value < -1.0f) return -1.0f;
    return value;
}

static void quaternion_to_euler(const sh2_RotationVectorWAcc_t *quaternion,
                                imu_sample_t *sample)
{
    float x = quaternion->i;
    float y = quaternion->j;
    float z = quaternion->k;
    float w = quaternion->real;
    float sin_roll = 2.0f * (w * x + y * z);
    float cos_roll = 1.0f - 2.0f * (x * x + y * y);
    float sin_pitch = clamp_unit(2.0f * (w * y - z * x));
    float sin_yaw = 2.0f * (w * z + x * y);
    float cos_yaw = 1.0f - 2.0f * (y * y + z * z);

    sample->roll_deg = atan2f(sin_roll, cos_roll) * RADIANS_TO_DEGREES;
    sample->pitch_deg = asinf(sin_pitch) * RADIANS_TO_DEGREES;
    sample->yaw_deg = atan2f(sin_yaw, cos_yaw) * RADIANS_TO_DEGREES;
    if (sample->yaw_deg < 0.0f) sample->yaw_deg += 360.0f;
}

static void bno085_async_callback(void *cookie, sh2_AsyncEvent_t *event)
{
    imu_i2c_t *imu = (imu_i2c_t *)cookie;
    if (imu != NULL && event != NULL && event->eventId == SH2_RESET) {
        imu->reset_seen = true;
    }
}

static void bno085_sensor_callback(void *cookie, sh2_SensorEvent_t *event)
{
    imu_i2c_t *imu = (imu_i2c_t *)cookie;
    sh2_SensorValue_t value;
    if (imu == NULL || event == NULL) return;
    memset(&value, 0, sizeof(value));
    if (sh2_decodeSensorEvent(&value, event) != SH2_OK ||
        value.sensorId != SH2_ROTATION_VECTOR) {
        return;
    }
    memset(&imu->latest_sample, 0, sizeof(imu->latest_sample));
    quaternion_to_euler(&value.un.rotationVector, &imu->latest_sample);
    imu->latest_sample.sequence = value.sequence;
    imu->latest_sample.calibration = (uint8_t)((value.status & 0x03u) * 0x55u);
    imu->sample_ready = true;
    imu->valid_samples++;
}

static bool init_bno085_at(imu_i2c_t *imu, uint8_t address)
{
    sh2_ProductIds_t product_ids;
    int status;

    if (!imu->bus.device_ready(imu->bus.context, address)) return false;
    imu->address_7bit = address;
    imu->kind = IMU_I2C_BNO085;
    g_active_bno085 = imu;
    memset(&g_sh2_hal, 0, sizeof(g_sh2_hal));
    g_sh2_hal.open = bno085_hal_open;
    g_sh2_hal.close = bno085_hal_close;
    g_sh2_hal.read = bno085_hal_read;
    g_sh2_hal.write = bno085_hal_write;
    g_sh2_hal.getTimeUs = bno085_hal_now_us;

    status = sh2_open(&g_sh2_hal, bno085_async_callback, imu);
    if (status != SH2_OK) {
        g_active_bno085 = NULL;
        return false;
    }
    memset(&product_ids, 0, sizeof(product_ids));
    status = sh2_getProdIds(&product_ids);
    if (status != SH2_OK || product_ids.numEntries == 0u ||
        sh2_setSensorCallback(bno085_sensor_callback, imu) != SH2_OK ||
        !configure_bno085_report()) {
        sh2_close();
        g_active_bno085 = NULL;
        return false;
    }
    imu->initialized = true;
    imu->reset_seen = false;
    imu->last_poll_us = imu->bus.now_us(imu->bus.context) -
                        BNO085_SERVICE_INTERVAL_US;
    return true;
}

bool imu_i2c_init(imu_i2c_t *imu, const imu_i2c_bus_t *bus)
{
    imu_i2c_bus_t bus_copy;
    if (imu == NULL || !bus_is_valid(bus)) return false;
    bus_copy = *bus;
    memset(imu, 0, sizeof(*imu));
    imu->bus = bus_copy;

    if (init_bno085_at(imu, BNO085_ADDRESS_A) ||
        init_bno085_at(imu, BNO085_ADDRESS_B)) {
        return true;
    }
    imu->kind = IMU_I2C_NONE;
    imu->address_7bit = 0u;
    if (init_bno055_at(imu, BNO055_ADDRESS_A) ||
        init_bno055_at(imu, BNO055_ADDRESS_B)) {
        return true;
    }
    imu->kind = IMU_I2C_NONE;
    imu->address_7bit = 0u;
    return false;
}

void imu_i2c_deinit(imu_i2c_t *imu)
{
    if (imu == NULL) return;
    if (imu->kind == IMU_I2C_BNO085 && g_active_bno085 == imu &&
        imu->initialized) {
        sh2_close();
        g_active_bno085 = NULL;
    }
    imu->initialized = false;
    imu->kind = IMU_I2C_NONE;
    imu->address_7bit = 0u;
    imu->sample_ready = false;
}

bool imu_i2c_poll(imu_i2c_t *imu, imu_sample_t *sample)
{
    uint32_t now_us;
    if (imu == NULL || sample == NULL || !imu->initialized) return false;
    now_us = imu->bus.now_us(imu->bus.context);

    if (imu->kind == IMU_I2C_BNO055) {
        if ((uint32_t)(now_us - imu->last_poll_us) < BNO055_SAMPLE_INTERVAL_US) {
            return false;
        }
        imu->last_poll_us = now_us;
        return poll_bno055(imu, sample);
    }
    if (imu->kind != IMU_I2C_BNO085 || g_active_bno085 != imu) return false;
    if ((uint32_t)(now_us - imu->last_poll_us) >= BNO085_SERVICE_INTERVAL_US) {
        imu->last_poll_us = now_us;
        sh2_service();
        if (imu->reset_seen) {
            imu->reset_seen = false;
            if (!configure_bno085_report()) {
                imu_i2c_deinit(imu);
                return false;
            }
        }
    }
    if (!imu->sample_ready) return false;
    *sample = imu->latest_sample;
    imu->sample_ready = false;
    return true;
}

const char *imu_i2c_name(const imu_i2c_t *imu)
{
    if (imu == NULL) return "NONE";
    if (imu->kind == IMU_I2C_BNO085) return "BNO085_I2C";
    if (imu->kind == IMU_I2C_BNO055) return "BNO055_I2C";
    return "NONE";
}

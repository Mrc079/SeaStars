#include "ms5837.h"

#include <stddef.h>
#include <string.h>

/*
 * MS5837-30BA command sequence, CRC and compensation algorithm adapted to
 * the Sea Stars bus callbacks from the MIT-licensed Blue Robotics MS5837
 * library and checked against the TE Connectivity datasheet.
 * See Firmware/THIRD_PARTY_NOTICES.md.
 */

#define CMD_RESET       0x1Eu
#define CMD_ADC_READ    0x00u
#define CMD_D1_OSR8192  0x4Au
#define CMD_D2_OSR8192  0x5Au
#define CMD_PROM_BASE   0xA0u
#define CONVERSION_MS   20u
#define STANDARD_GRAVITY_M_S2 9.80665f

static bool write_command(ms5837_t *sensor, uint8_t command)
{
    return sensor->bus.write(MS5837_I2C_ADDRESS_7BIT, &command, 1u);
}

static bool read_adc(ms5837_t *sensor, uint8_t conversion_command, uint32_t *value)
{
    uint8_t bytes[3];
    if (!write_command(sensor, conversion_command)) {
        sensor->last_error = MS5837_ERROR_CONVERSION_COMMAND;
        return false;
    }
    sensor->bus.delay_ms(CONVERSION_MS);
    if (!write_command(sensor, CMD_ADC_READ)) {
        sensor->last_error = MS5837_ERROR_ADC_READ_COMMAND;
        return false;
    }
    if (!sensor->bus.read(MS5837_I2C_ADDRESS_7BIT, bytes, sizeof(bytes))) {
        sensor->last_error = MS5837_ERROR_ADC_READ;
        return false;
    }
    *value = ((uint32_t)bytes[0] << 16) |
             ((uint32_t)bytes[1] << 8) |
             (uint32_t)bytes[2];
    if (*value == 0u) {
        sensor->last_error = MS5837_ERROR_ADC_ZERO;
        return false;
    }
    return true;
}

uint8_t ms5837_crc4(const uint16_t source[8])
{
    uint16_t prom[8];
    uint16_t remainder = 0u;
    uint8_t count;
    uint8_t bit;

    memcpy(prom, source, sizeof(prom));
    prom[0] &= 0x0FFFu;
    prom[7] = 0u;
    for (count = 0u; count < 16u; ++count) {
        remainder ^= (count & 1u) ? (prom[count >> 1] & 0x00FFu)
                                  : (prom[count >> 1] >> 8);
        for (bit = 0u; bit < 8u; ++bit) {
            remainder = (remainder & 0x8000u)
                ? (uint16_t)((remainder << 1) ^ 0x3000u)
                : (uint16_t)(remainder << 1);
        }
    }
    return (uint8_t)((remainder >> 12) & 0x0Fu);
}

bool ms5837_init(ms5837_t *sensor, const ms5837_bus_t *bus)
{
    uint8_t bytes[2];
    uint8_t index;
    uint8_t expected_crc;
    bool coefficients_all_zero = true;
    bool coefficients_all_ones = true;

    if (sensor == NULL) {
        return false;
    }
    memset(sensor, 0, sizeof(*sensor));
    sensor->last_error = MS5837_ERROR_INVALID_ARGUMENT;
    if (bus == NULL || bus->write == NULL || bus->read == NULL ||
        bus->delay_ms == NULL) {
        return false;
    }
    sensor->bus = *bus;
    sensor->fluid_density_kg_m3 = 997.0f;
    if (!write_command(sensor, CMD_RESET)) {
        sensor->last_error = MS5837_ERROR_RESET_WRITE;
        return false;
    }
    sensor->bus.delay_ms(10u);

    /* The device exposes exactly seven PROM words: 0xA0 through 0xAC. */
    for (index = 0u; index < 7u; ++index) {
        uint8_t command = (uint8_t)(CMD_PROM_BASE + (index * 2u));
        if (!write_command(sensor, command)) {
            sensor->last_error = MS5837_ERROR_PROM_COMMAND;
            return false;
        }
        if (!sensor->bus.read(MS5837_I2C_ADDRESS_7BIT, bytes, sizeof(bytes))) {
            sensor->last_error = MS5837_ERROR_PROM_READ;
            return false;
        }
        sensor->prom[index] = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    }
    for (index = 1u; index <= 6u; ++index) {
        coefficients_all_zero = coefficients_all_zero &&
                                sensor->prom[index] == 0u;
        coefficients_all_ones = coefficients_all_ones &&
                                sensor->prom[index] == 0xFFFFu;
    }
    if (coefficients_all_zero || coefficients_all_ones) {
        sensor->last_error = MS5837_ERROR_PROM_INVALID;
        return false;
    }
    expected_crc = (uint8_t)((sensor->prom[0] >> 12) & 0x0Fu);
    if (ms5837_crc4(sensor->prom) != expected_crc) {
        sensor->last_error = MS5837_ERROR_PROM_CRC;
        return false;
    }
    sensor->last_error = MS5837_ERROR_NONE;
    sensor->initialized = true;
    return true;
}

bool ms5837_read(ms5837_t *sensor, ms5837_sample_t *sample)
{
    uint32_t d1;
    uint32_t d2;
    int64_t dt;
    int64_t sensitivity;
    int64_t offset;
    int64_t temperature;
    int64_t temperature_compensation;
    int64_t offset_compensation;
    int64_t sensitivity_compensation;
    int64_t pressure;
    float water_column_pressure_pa;

    if (sensor == NULL) {
        return false;
    }
    if (sample == NULL) {
        sensor->last_error = MS5837_ERROR_INVALID_ARGUMENT;
        return false;
    }
    if (!sensor->initialized) {
        sensor->last_error = MS5837_ERROR_NOT_INITIALIZED;
        return false;
    }
    if (!read_adc(sensor, CMD_D1_OSR8192, &d1) ||
        !read_adc(sensor, CMD_D2_OSR8192, &d2)) {
        return false;
    }

    dt = (int64_t)d2 - ((int64_t)sensor->prom[5] * 256LL);
    sensitivity = ((int64_t)sensor->prom[1] * 32768LL) +
                  (((int64_t)sensor->prom[3] * dt) / 256LL);
    offset = ((int64_t)sensor->prom[2] * 65536LL) +
             (((int64_t)sensor->prom[4] * dt) / 128LL);
    temperature = 2000LL + ((dt * (int64_t)sensor->prom[6]) / 8388608LL);

    if (temperature < 2000LL) {
        int64_t low = temperature - 2000LL;
        temperature_compensation = (3LL * dt * dt) / 8589934592LL;
        offset_compensation = (3LL * low * low) / 2LL;
        sensitivity_compensation = (5LL * low * low) / 8LL;
        if (temperature < -1500LL) {
            int64_t very_low = temperature + 1500LL;
            offset_compensation += 7LL * very_low * very_low;
            sensitivity_compensation += 4LL * very_low * very_low;
        }
    } else {
        int64_t high = temperature - 2000LL;
        temperature_compensation = (2LL * dt * dt) / 137438953472LL;
        offset_compensation = (high * high) / 16LL;
        sensitivity_compensation = 0LL;
    }

    temperature -= temperature_compensation;
    offset -= offset_compensation;
    sensitivity -= sensitivity_compensation;
    pressure = ((((int64_t)d1 * sensitivity) / 2097152LL) - offset) / 8192LL;

    sample->pressure_mbar = (float)pressure / 10.0f;
    sample->temperature_c = (float)temperature / 100.0f;
    sensor->last_error = MS5837_ERROR_NONE;
    if (!sensor->surface_calibrated) {
        sample->sensor_depth_m = 0.0f;
        sample->vehicle_reference_depth_m = sensor->sensor_to_reference_offset_m;
        return true;
    }
    water_column_pressure_pa =
        (sample->pressure_mbar - sensor->surface_pressure_mbar) * 100.0f;
    sample->sensor_depth_m = water_column_pressure_pa /
        (sensor->fluid_density_kg_m3 * STANDARD_GRAVITY_M_S2);
    if (sample->sensor_depth_m < 0.0f) {
        sample->sensor_depth_m = 0.0f;
    }
    sample->vehicle_reference_depth_m =
        sample->sensor_depth_m + sensor->sensor_to_reference_offset_m;
    return true;
}

bool ms5837_calibrate_surface(ms5837_t *sensor, uint8_t sample_count)
{
    ms5837_sample_t sample;
    float total = 0.0f;
    uint8_t index;

    if (sensor == NULL) {
        return false;
    }
    if (!sensor->initialized) {
        sensor->last_error = MS5837_ERROR_NOT_INITIALIZED;
        return false;
    }
    if (sample_count < 4u) {
        sample_count = 4u;
    }
    if (sample_count > 32u) {
        sample_count = 32u;
    }
    sensor->surface_calibrated = false;
    for (index = 0u; index < sample_count; ++index) {
        if (!ms5837_read(sensor, &sample)) {
            return false;
        }
        total += sample.pressure_mbar;
    }
    sensor->surface_pressure_mbar = total / (float)sample_count;
    sensor->surface_calibrated = true;
    return true;
}

void ms5837_set_surface_pressure(ms5837_t *sensor, float pressure_mbar)
{
    if (sensor != NULL && pressure_mbar > 100.0f && pressure_mbar < 40000.0f) {
        sensor->surface_pressure_mbar = pressure_mbar;
        sensor->surface_calibrated = true;
    }
}

void ms5837_set_fluid_density(ms5837_t *sensor, float density_kg_m3)
{
    if (sensor != NULL && density_kg_m3 >= 950.0f && density_kg_m3 <= 1100.0f) {
        sensor->fluid_density_kg_m3 = density_kg_m3;
    }
}

void ms5837_set_mount_offset(ms5837_t *sensor, float offset_m)
{
    if (sensor != NULL && offset_m >= -1.0f && offset_m <= 1.0f) {
        sensor->sensor_to_reference_offset_m = offset_m;
    }
}

const char *ms5837_error_name(ms5837_error_t error)
{
    switch (error) {
    case MS5837_ERROR_NONE: return "NONE";
    case MS5837_ERROR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case MS5837_ERROR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case MS5837_ERROR_RESET_WRITE: return "RESET_WRITE";
    case MS5837_ERROR_PROM_COMMAND: return "PROM_COMMAND";
    case MS5837_ERROR_PROM_READ: return "PROM_READ";
    case MS5837_ERROR_PROM_INVALID: return "PROM_INVALID";
    case MS5837_ERROR_PROM_CRC: return "PROM_CRC";
    case MS5837_ERROR_CONVERSION_COMMAND: return "CONVERSION_COMMAND";
    case MS5837_ERROR_ADC_READ_COMMAND: return "ADC_READ_COMMAND";
    case MS5837_ERROR_ADC_READ: return "ADC_READ";
    case MS5837_ERROR_ADC_ZERO: return "ADC_ZERO";
    default: return "UNKNOWN";
    }
}

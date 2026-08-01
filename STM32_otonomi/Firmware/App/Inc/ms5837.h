#ifndef SEASTARS_MS5837_H
#define SEASTARS_MS5837_H

#include <stdbool.h>
#include <stdint.h>

#define MS5837_I2C_ADDRESS_7BIT 0x76u

typedef bool (*ms5837_write_fn)(uint8_t address_7bit, const uint8_t *data, uint16_t length);
typedef bool (*ms5837_read_fn)(uint8_t address_7bit, uint8_t *data, uint16_t length);
typedef void (*ms5837_delay_fn)(uint32_t milliseconds);

typedef struct {
    ms5837_write_fn write;
    ms5837_read_fn read;
    ms5837_delay_fn delay_ms;
} ms5837_bus_t;

typedef struct {
    float pressure_mbar;
    float temperature_c;
    float sensor_depth_m;
    float vehicle_reference_depth_m;
} ms5837_sample_t;

typedef enum {
    MS5837_ERROR_NONE = 0,
    MS5837_ERROR_INVALID_ARGUMENT,
    MS5837_ERROR_NOT_INITIALIZED,
    MS5837_ERROR_RESET_WRITE,
    MS5837_ERROR_PROM_COMMAND,
    MS5837_ERROR_PROM_READ,
    MS5837_ERROR_PROM_INVALID,
    MS5837_ERROR_PROM_CRC,
    MS5837_ERROR_CONVERSION_COMMAND,
    MS5837_ERROR_ADC_READ_COMMAND,
    MS5837_ERROR_ADC_READ,
    MS5837_ERROR_ADC_ZERO
} ms5837_error_t;

typedef struct {
    ms5837_bus_t bus;
    uint16_t prom[8];
    float surface_pressure_mbar;
    float fluid_density_kg_m3;
    float sensor_to_reference_offset_m;
    ms5837_error_t last_error;
    bool initialized;
    bool surface_calibrated;
} ms5837_t;

bool ms5837_init(ms5837_t *sensor, const ms5837_bus_t *bus);
bool ms5837_read(ms5837_t *sensor, ms5837_sample_t *sample);
bool ms5837_calibrate_surface(ms5837_t *sensor, uint8_t sample_count);
void ms5837_set_surface_pressure(ms5837_t *sensor, float pressure_mbar);
void ms5837_set_fluid_density(ms5837_t *sensor, float density_kg_m3);
void ms5837_set_mount_offset(ms5837_t *sensor, float offset_m);
uint8_t ms5837_crc4(const uint16_t prom[8]);
const char *ms5837_error_name(ms5837_error_t error);

#endif

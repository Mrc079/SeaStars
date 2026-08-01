#ifndef SEASTARS_RUNTIME_H
#define SEASTARS_RUNTIME_H

#include "autonomy.h"
#include "imu_sample.h"
#include "ms5837.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *context;
    uint32_t (*now_ms)(void *context);
    void (*set_thruster_percent)(void *context, uint8_t index, float percent);
    void (*set_tank_target)(void *context, uint8_t index, int32_t absolute_steps);
    void (*set_armed)(void *context, bool armed);
    void (*set_tank_speed)(void *context, uint8_t index, uint32_t steps_per_second);
    void (*zero_tank_position)(void *context, uint8_t index);
} seastars_platform_t;

typedef struct {
    seastars_platform_t platform;
    ms5837_t *depth_sensor;
    autonomy_t autonomy;
    autonomy_input_t input;
    autonomy_output_t output;
    ms5837_sample_t depth;
    imu_sample_t imu;
    uint32_t depth_updated_ms;
    uint32_t imu_updated_ms;
    float heading_offset_deg;
    float heading_sign;
    int32_t last_tank_target[2];
    float last_thruster[2];
    bool armed;
    bool estop_latched;
    bool auto_start_scheduled;
    bool auto_sequence_consumed;
    bool manual_turn_active;
    float manual_turn_target_deg;
} seastars_runtime_t;

bool seastars_runtime_init(seastars_runtime_t *runtime,
                           const seastars_platform_t *platform,
                           ms5837_t *depth_sensor);
void seastars_runtime_set_tank_position(seastars_runtime_t *runtime,
                                        int32_t tank1, int32_t tank2);
void seastars_runtime_set_imu(seastars_runtime_t *runtime,
                              const imu_sample_t *sample);
void seastars_runtime_set_depth(seastars_runtime_t *runtime,
                                const ms5837_sample_t *sample);
bool seastars_runtime_command(seastars_runtime_t *runtime, const char *command,
                              char *reply, size_t reply_size);
void seastars_runtime_tick(seastars_runtime_t *runtime);
int seastars_runtime_format_telemetry(const seastars_runtime_t *runtime,
                                      char *buffer, size_t buffer_size);

#endif

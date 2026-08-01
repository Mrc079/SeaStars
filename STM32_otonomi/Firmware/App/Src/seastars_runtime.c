#include "seastars_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SENSOR_FRESH_MS 600u
#define TARGET_CHANGE_MIN_STEPS 2

static float wrap_heading(float value)
{
    while (value >= 360.0f) value -= 360.0f;
    while (value < 0.0f) value += 360.0f;
    return value;
}

static float heading_error(float target, float current)
{
    float error = wrap_heading(target) - wrap_heading(current);
    while (error > 180.0f) error -= 360.0f;
    while (error < -180.0f) error += 360.0f;
    return error;
}

static bool parse_long(const char *text, long *value)
{
    char *end;
    if (text == NULL || value == NULL) return false;
    *value = strtol(text, &end, 10);
    return end != text && *end == '\0';
}

static void apply_armed(seastars_runtime_t *runtime, bool armed)
{
    if (runtime->armed != armed) {
        runtime->armed = armed;
        runtime->platform.set_armed(runtime->platform.context, armed);
    }
}

static void force_disarmed(seastars_runtime_t *runtime)
{
    runtime->platform.set_thruster_percent(runtime->platform.context, 1u, 0.0f);
    runtime->platform.set_thruster_percent(runtime->platform.context, 2u, 0.0f);
    runtime->last_thruster[0] = 0.0f;
    runtime->last_thruster[1] = 0.0f;
    runtime->armed = false;
    runtime->platform.set_armed(runtime->platform.context, false);
}

static void apply_thrusters(seastars_runtime_t *runtime, float left, float right)
{
    const float requested[2] = {left, right};
    uint8_t index;
    for (index = 0u; index < 2u; ++index) {
        if (fabsf(requested[index] - runtime->last_thruster[index]) >= 0.5f) {
            runtime->platform.set_thruster_percent(
                runtime->platform.context, (uint8_t)(index + 1u), requested[index]);
            runtime->last_thruster[index] = requested[index];
        }
    }
}

static void apply_tanks(seastars_runtime_t *runtime, const int32_t targets[2])
{
    uint8_t index;
    for (index = 0u; index < 2u; ++index) {
        if (labs((long)(targets[index] - runtime->last_tank_target[index])) >=
            TARGET_CHANGE_MIN_STEPS) {
            runtime->platform.set_tank_target(
                runtime->platform.context, (uint8_t)(index + 1u), targets[index]);
            runtime->last_tank_target[index] = targets[index];
        }
    }
}

static bool schedule_autonomy(seastars_runtime_t *runtime, uint32_t now,
                              bool deliberate_rearm, char *reply, size_t reply_size)
{
    uint8_t index;
    autonomy_config_t *config = &runtime->autonomy.config;

    if (runtime->estop_latched || runtime->autonomy.running ||
        runtime->auto_start_scheduled) {
        snprintf(reply, reply_size, "ERR AUTO LOCKED");
        return false;
    }
    if (config->auto_start_delay_ms == 0u) {
        snprintf(reply, reply_size, "ERR AUTO DISABLED");
        return false;
    }
    if (!deliberate_rearm && runtime->auto_sequence_consumed) {
        snprintf(reply, reply_size, "ERR AUTO ALREADY_CONSUMED");
        return false;
    }
    if (runtime->input.tank_full_steps[0] <= 0 ||
        runtime->input.tank_full_steps[1] <= 0) {
        runtime->autonomy.fault = AUTO_FAULT_TANK_CALIBRATION;
        snprintf(reply, reply_size, "ERR AUTO TANK_CALIBRATION");
        return false;
    }
    if (config->auto_zero_tanks) {
        if (runtime->platform.zero_tank_position == NULL) {
            snprintf(reply, reply_size, "ERR AUTO ZERO_UNAVAILABLE");
            return false;
        }
        for (index = 0u; index < 2u; ++index) {
            runtime->platform.zero_tank_position(runtime->platform.context,
                                                 (uint8_t)(index + 1u));
            runtime->input.tank_position[index] = 0;
            runtime->input.tank_zeroed[index] = true;
            runtime->last_tank_target[index] = 0;
        }
    } else if (!runtime->input.tank_zeroed[0] || !runtime->input.tank_zeroed[1] ||
               runtime->input.tank_position[0] > runtime->input.tank_full_steps[0] / 100 ||
               runtime->input.tank_position[1] > runtime->input.tank_full_steps[1] / 100) {
        runtime->autonomy.fault = AUTO_FAULT_TANK_CALIBRATION;
        snprintf(reply, reply_size, "ERR AUTO TANK_NOT_EMPTY");
        return false;
    }

    force_disarmed(runtime);
    runtime->manual_turn_active = false;
    runtime->auto_sequence_consumed = true;
    runtime->auto_start_scheduled = true;
    runtime->autonomy.running = false;
    runtime->autonomy.fault = AUTO_FAULT_NONE;
    runtime->autonomy.state = AUTO_STATE_COUNTDOWN;
    runtime->autonomy.state_started_ms = now;
    snprintf(reply, reply_size, "OK AUTO COUNTDOWN %lu",
             (unsigned long)config->auto_start_delay_ms);
    return true;
}

bool seastars_runtime_init(seastars_runtime_t *runtime,
                           const seastars_platform_t *platform,
                           ms5837_t *depth_sensor)
{
    autonomy_config_t defaults;
    if (runtime == NULL || platform == NULL || depth_sensor == NULL ||
        platform->now_ms == NULL || platform->set_thruster_percent == NULL ||
        platform->set_tank_target == NULL || platform->set_armed == NULL) {
        return false;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->platform = *platform;
    runtime->depth_sensor = depth_sensor;
    runtime->heading_sign = 1.0f;
    runtime->last_tank_target[0] = -1000000;
    runtime->last_tank_target[1] = -1000000;
    autonomy_default_config(&defaults);
    autonomy_init(&runtime->autonomy, &defaults);
    return true;
}

void seastars_runtime_set_tank_position(seastars_runtime_t *runtime,
                                        int32_t tank1, int32_t tank2)
{
    if (runtime == NULL) return;
    runtime->input.tank_position[0] = tank1;
    runtime->input.tank_position[1] = tank2;
}

void seastars_runtime_set_imu(seastars_runtime_t *runtime,
                              const imu_sample_t *sample)
{
    if (runtime == NULL || sample == NULL) return;
    runtime->imu = *sample;
    runtime->imu_updated_ms = runtime->platform.now_ms(runtime->platform.context);
}

void seastars_runtime_set_depth(seastars_runtime_t *runtime,
                                const ms5837_sample_t *sample)
{
    if (runtime == NULL || sample == NULL) return;
    runtime->depth = *sample;
    runtime->depth_updated_ms = runtime->platform.now_ms(runtime->platform.context);
}

static bool apply_config(seastars_runtime_t *runtime, const char *name, long value)
{
    autonomy_config_t *config = &runtime->autonomy.config;
    if (!strcmp(name, "TANK1_FULL")) runtime->input.tank_full_steps[0] = (int32_t)value;
    else if (!strcmp(name, "TANK2_FULL")) runtime->input.tank_full_steps[1] = (int32_t)value;
    else if (!strcmp(name, "OFFSET_MM")) ms5837_set_mount_offset(runtime->depth_sensor, (float)value / 1000.0f);
    else if (!strcmp(name, "DENSITY")) ms5837_set_fluid_density(runtime->depth_sensor, (float)value);
    else if (!strcmp(name, "HEADING_OFFSET_CDEG")) runtime->heading_offset_deg = (float)value / 100.0f;
    else if (!strcmp(name, "HEADING_SIGN") && (value == -1 || value == 1)) runtime->heading_sign = (float)value;
    else if (!strcmp(name, "BALANCE_AXIS") && value >= 0 && value <= 2) config->balance_axis = (autonomy_balance_axis_t)value;
    else if (!strcmp(name, "BALANCE_SIGN") && (value == -1 || value == 1)) config->balance_sign = (float)value;
    else if (!strcmp(name, "LEVEL_KP_X100") && value >= 0 && value <= 500) config->level_kp_pct_per_degree = (float)value / 100.0f;
    else if (!strcmp(name, "MAX_BALANCE_PCT") && value >= 0 && value <= 40) config->maximum_balance_pct = (float)value;
    else if (!strcmp(name, "ROUTE") && value >= 0 && value <= 1) config->route = (autonomy_route_t)value;
    else if (!strcmp(name, "DIVE_BALLAST_PCT") && value >= 0 && value <= 100) config->dive_ballast_pct = (float)value;
    else if (!strcmp(name, "HOVER_BALLAST_PCT") && value >= 0 && value <= 100) config->hover_ballast_pct = (float)value;
    else if (!strcmp(name, "DIVE_MS") && value >= 500 && value <= 120000) config->dive_duration_ms = (uint32_t)value;
    else if (!strcmp(name, "HOVER_SETTLE_MS") && value >= 0 && value <= 60000) config->hover_settle_ms = (uint32_t)value;
    else if (!strcmp(name, "TEST_FORWARD_MS") && value >= 1000 && value <= 120000) config->test_forward_duration_ms = (uint32_t)value;
    else if (!strcmp(name, "TEST_FORWARD_PCT") && value >= 0 && value <= 100) config->test_forward_power_pct = (float)value;
    else if (!strcmp(name, "AUTO_DELAY_MS") && value >= 0 && value <= 86400000L) config->auto_start_delay_ms = (uint32_t)value;
    else if (!strcmp(name, "AUTO_ZERO_TANKS") && (value == 0 || value == 1)) config->auto_zero_tanks = value == 1;
    else if (!strcmp(name, "DEPTH_MM")) config->target_depth_m = (float)value / 1000.0f;
    else if (!strcmp(name, "MAX_DEPTH_MM")) config->maximum_depth_m = (float)value / 1000.0f;
    else if (!strcmp(name, "DEPTH_TOL_MM")) config->depth_tolerance_m = (float)value / 1000.0f;
    else if (!strcmp(name, "STRAIGHT_MS") && value >= 15000) config->straight_duration_ms = (uint32_t)value;
    else if (!strcmp(name, "FORWARD_PCT") && value > 0 && value <= 100) config->forward_power_pct = (float)value;
    else if (!strcmp(name, "CIRCLE_FORWARD_PCT") && value > 0 && value <= 100) config->circle_forward_power_pct = (float)value;
    else if (!strcmp(name, "CIRCLE_TURN_PCT") && value > 0 && value <= 100) config->circle_turn_power_pct = (float)value;
    else if (!strcmp(name, "HEADING_KP_X100") && value >= 5 && value <= 500) config->heading_kp_pct_per_degree = (float)value / 100.0f;
    else if (!strcmp(name, "BALLAST_GAIN_X100") && value >= 100 && value <= 20000) config->ballast_rate_pct_per_m_s = (float)value / 100.0f;
    else return false;
    return true;
}

bool seastars_runtime_command(seastars_runtime_t *runtime, const char *command,
                              char *reply, size_t reply_size)
{
    char copy[80];
    char *name;
    char *value_text;
    long value;
    uint32_t now;
    int32_t tank_targets[2];
    uint8_t index;
    if (runtime == NULL || command == NULL || reply == NULL || reply_size == 0u) return false;
    snprintf(copy, sizeof(copy), "%s", command);
    now = runtime->platform.now_ms(runtime->platform.context);

    if (!strcmp(copy, "STOP")) {
        runtime->auto_start_scheduled = false;
        runtime->autonomy.running = false;
        runtime->autonomy.state = AUTO_STATE_IDLE;
        runtime->autonomy.fault = AUTO_FAULT_NONE;
        runtime->autonomy.state_started_ms = now;
        runtime->manual_turn_active = false;
        runtime->estop_latched = true;
        force_disarmed(runtime);
        for (index = 0u; index < 2u; ++index) {
            runtime->platform.set_tank_target(runtime->platform.context,
                                               (uint8_t)(index + 1u),
                                               runtime->input.tank_position[index]);
            runtime->last_tank_target[index] = runtime->input.tank_position[index];
        }
        snprintf(reply, reply_size, "OK STOPPED");
        return true;
    }
    if (!strcmp(copy, "DISARM")) {
        if (runtime->auto_start_scheduled) {
            runtime->auto_start_scheduled = false;
            runtime->autonomy.state = AUTO_STATE_IDLE;
            runtime->autonomy.fault = AUTO_FAULT_NONE;
            runtime->autonomy.state_started_ms = now;
        }
        if (runtime->autonomy.running) {
            autonomy_abort(&runtime->autonomy, now);
        }
        runtime->manual_turn_active = false;
        runtime->estop_latched = false;
        force_disarmed(runtime);
        snprintf(reply, reply_size, "OK DISARMED");
        return true;
    }
    if (!strcmp(copy, "ARM")) {
        if (runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled) {
            snprintf(reply, reply_size, "ERR ARM LOCKED");
            return false;
        }
        apply_armed(runtime, true);
        snprintf(reply, reply_size, "OK ARMED");
        return true;
    }
    if ((!strncmp(copy, "T1 ", 3u) || !strncmp(copy, "T2 ", 3u)) &&
        parse_long(copy + 3, &value)) {
        float requested[2] = {runtime->last_thruster[0], runtime->last_thruster[1]};
        index = (uint8_t)(copy[1] - '1');
        if (!runtime->armed || runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled ||
            value < -200 || value > 200) {
            snprintf(reply, reply_size, "ERR THRUSTER");
            return false;
        }
        requested[index] = (float)value / 2.0f;
        apply_thrusters(runtime, requested[0], requested[1]);
        snprintf(reply, reply_size, "OK T%u", (unsigned)(index + 1u));
        return true;
    }
    if ((!strncmp(copy, "S1 ", 3u) || !strncmp(copy, "S2 ", 3u)) &&
        parse_long(copy + 3, &value)) {
        index = (uint8_t)(copy[1] - '1');
        if (runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled ||
            value < -1000000L || value > 1000000L) {
            snprintf(reply, reply_size, "ERR TANK");
            return false;
        }
        tank_targets[0] = runtime->input.tank_position[0];
        tank_targets[1] = runtime->input.tank_position[1];
        tank_targets[index] += (int32_t)value;
        if (runtime->input.tank_full_steps[index] > 0 &&
            (tank_targets[index] < 0 ||
             tank_targets[index] > runtime->input.tank_full_steps[index])) {
            snprintf(reply, reply_size, "ERR TANK LIMIT");
            return false;
        }
        apply_tanks(runtime, tank_targets);
        snprintf(reply, reply_size, "OK S%u", (unsigned)(index + 1u));
        return true;
    }
    if ((!strncmp(copy, "V1 ", 3u) || !strncmp(copy, "V2 ", 3u)) &&
        parse_long(copy + 3, &value)) {
        index = (uint8_t)(copy[1] - '1');
        if (runtime->platform.set_tank_speed == NULL || value < 50 || value > 2000 ||
            runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled) {
            snprintf(reply, reply_size, "ERR TANK SPEED");
            return false;
        }
        runtime->platform.set_tank_speed(runtime->platform.context,
                                         (uint8_t)(index + 1u), (uint32_t)value);
        snprintf(reply, reply_size, "OK V%u", (unsigned)(index + 1u));
        return true;
    }
    if (!strcmp(copy, "ZERO1") || !strcmp(copy, "ZERO2")) {
        index = (uint8_t)(copy[4] - '1');
        if (runtime->platform.zero_tank_position == NULL || runtime->estop_latched ||
            runtime->autonomy.running || runtime->auto_start_scheduled) {
            snprintf(reply, reply_size, "ERR ZERO");
            return false;
        }
        runtime->platform.zero_tank_position(runtime->platform.context,
                                             (uint8_t)(index + 1u));
        runtime->input.tank_position[index] = 0;
        runtime->input.tank_zeroed[index] = true;
        runtime->last_tank_target[index] = 0;
        snprintf(reply, reply_size, "OK ZERO%u", (unsigned)(index + 1u));
        return true;
    }
    if (!strcmp(copy, "POS")) {
        snprintf(reply, reply_size, "POS1:%ld POS2:%ld Z1:%u Z2:%u",
                 (long)runtime->input.tank_position[0],
                 (long)runtime->input.tank_position[1],
                 runtime->input.tank_zeroed[0] ? 1u : 0u,
                 runtime->input.tank_zeroed[1] ? 1u : 0u);
        return true;
    }

    if (!strcmp(copy, "DEPTH_ZERO")) {
        if (runtime->armed || runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled ||
            runtime->depth_updated_ms == 0u || !runtime->depth_sensor->initialized) {
            snprintf(reply, reply_size, "ERR DEPTH_ZERO LOCKED");
            return false;
        }
        bool ok = ms5837_calibrate_surface(runtime->depth_sensor, 8u);
        snprintf(reply, reply_size, ok ? "OK DEPTH_ZERO" : "ERR DEPTH_ZERO");
        return ok;
    }
    if (!strcmp(copy, "IMU_ZERO")) {
        if (runtime->armed || runtime->estop_latched || runtime->autonomy.running ||
            runtime->auto_start_scheduled ||
            runtime->imu_updated_ms == 0u ||
            (now - runtime->imu_updated_ms) > SENSOR_FRESH_MS) {
            snprintf(reply, reply_size, "ERR IMU_ZERO LOCKED");
            return false;
        }
        runtime->heading_offset_deg = -runtime->imu.yaw_deg * runtime->heading_sign;
        snprintf(reply, reply_size, "OK IMU_ZERO");
        return true;
    }
    if (!strcmp(copy, "AUTO START")) {
        if (runtime->estop_latched || runtime->auto_start_scheduled ||
            runtime->autonomy.running) {
            snprintf(reply, reply_size, "ERR AUTO ESTOP");
            return false;
        }
        runtime->input.now_ms = now;
        if (!autonomy_start(&runtime->autonomy, &runtime->input)) {
            snprintf(reply, reply_size, "ERR AUTO %s", autonomy_fault_name(runtime->autonomy.fault));
            return false;
        }
        runtime->auto_sequence_consumed = true;
        apply_armed(runtime, true);
        snprintf(reply, reply_size, "OK AUTO START");
        return true;
    }
    if (!strcmp(copy, "AUTO SCHEDULE")) {
        return schedule_autonomy(runtime, now, false, reply, reply_size);
    }
    if (!strcmp(copy, "AUTO PREPARE")) {
        return schedule_autonomy(runtime, now, true, reply, reply_size);
    }
    if (!strcmp(copy, "AUTO ABORT")) {
        runtime->auto_start_scheduled = false;
        autonomy_abort(&runtime->autonomy, now);
        runtime->manual_turn_active = false;
        force_disarmed(runtime);
        snprintf(reply, reply_size, "OK AUTO ABORT");
        return true;
    }
    if (!strncmp(copy, "TURN ", 5u) && parse_long(copy + 5, &value) &&
        value >= -180 && value <= 180 && value != 0 && runtime->armed &&
        !runtime->autonomy.running && !runtime->auto_start_scheduled &&
        !runtime->estop_latched) {
        runtime->manual_turn_target_deg = wrap_heading(runtime->input.heading_deg + (float)value);
        runtime->manual_turn_active = true;
        snprintf(reply, reply_size, "OK TURN %ld", value);
        return true;
    }
    if (strncmp(copy, "CFG ", 4u) != 0) {
        snprintf(reply, reply_size, "UNHANDLED");
        return false;
    }
    if (runtime->autonomy.running || runtime->auto_start_scheduled) {
        snprintf(reply, reply_size, "ERR CFG MISSION_ACTIVE");
        return false;
    }
    name = copy + 4;
    value_text = strchr(name, ' ');
    if (value_text == NULL) {
        snprintf(reply, reply_size, "ERR CFG");
        return false;
    }
    *value_text++ = '\0';
    if (!parse_long(value_text, &value) || !apply_config(runtime, name, value)) {
        snprintf(reply, reply_size, "ERR CFG %s", name);
        return false;
    }
    snprintf(reply, reply_size, "OK CFG %s", name);
    return true;
}

void seastars_runtime_tick(seastars_runtime_t *runtime)
{
    uint32_t now;
    if (runtime == NULL) return;
    now = runtime->platform.now_ms(runtime->platform.context);
    runtime->input.now_ms = now;
    runtime->input.depth_m = runtime->depth.vehicle_reference_depth_m;
    runtime->input.sensor_depth_m = runtime->depth.sensor_depth_m;
    runtime->input.heading_deg = wrap_heading(
        runtime->imu.yaw_deg * runtime->heading_sign + runtime->heading_offset_deg);
    runtime->input.roll_deg = runtime->imu.roll_deg;
    runtime->input.pitch_deg = runtime->imu.pitch_deg;
    runtime->input.depth_fresh = runtime->depth_updated_ms != 0u &&
        (now - runtime->depth_updated_ms) <= SENSOR_FRESH_MS &&
        runtime->depth_sensor->surface_calibrated;
    runtime->input.imu_fresh = runtime->imu_updated_ms != 0u &&
        (now - runtime->imu_updated_ms) <= SENSOR_FRESH_MS;

    if (runtime->estop_latched) {
        force_disarmed(runtime);
        return;
    }

    if (runtime->auto_start_scheduled) {
        uint32_t elapsed = now - runtime->autonomy.state_started_ms;
        force_disarmed(runtime);
        if (elapsed < runtime->autonomy.config.auto_start_delay_ms) {
            return;
        }
        runtime->auto_start_scheduled = false;
        if (!autonomy_start(&runtime->autonomy, &runtime->input)) {
            runtime->autonomy.running = false;
            runtime->autonomy.state = AUTO_STATE_FAULT_SURFACE;
            runtime->autonomy.state_started_ms = now;
        } else {
            apply_armed(runtime, true);
        }
    }

    if (runtime->autonomy.running ||
        runtime->autonomy.state == AUTO_STATE_ABORTED ||
        runtime->autonomy.state == AUTO_STATE_FAULT_SURFACE ||
        runtime->autonomy.state == AUTO_STATE_SURFACE) {
        autonomy_tick(&runtime->autonomy, &runtime->input, &runtime->output);
        apply_armed(runtime, runtime->output.thrusters_armed);
        apply_thrusters(runtime, runtime->output.left_thruster_pct,
                        runtime->output.right_thruster_pct);
        if (runtime->output.tank_target_valid) {
            apply_tanks(runtime, runtime->output.tank_target_steps);
        }
        return;
    }
    if (runtime->manual_turn_active) {
        float error = heading_error(runtime->manual_turn_target_deg, runtime->input.heading_deg);
        if (!runtime->input.imu_fresh || fabsf(error) <= 2.0f) {
            runtime->manual_turn_active = false;
            apply_thrusters(runtime, 0.0f, 0.0f);
        } else {
            float correction = error * runtime->autonomy.config.heading_kp_pct_per_degree;
            if (fabsf(correction) < runtime->autonomy.config.turn_minimum_pct) {
                correction = (error > 0.0f) ? runtime->autonomy.config.turn_minimum_pct
                                            : -runtime->autonomy.config.turn_minimum_pct;
            }
            if (correction > 30.0f) correction = 30.0f;
            if (correction < -30.0f) correction = -30.0f;
            apply_thrusters(runtime, correction, -correction);
        }
    }
}

int seastars_runtime_format_telemetry(const seastars_runtime_t *runtime,
                                      char *buffer, size_t buffer_size)
{
    uint32_t now;
    uint32_t elapsed;
    size_t used = 0u;
    int written;
    bool active;
    if (runtime == NULL || buffer == NULL || buffer_size == 0u) return -1;
    now = runtime->platform.now_ms(runtime->platform.context);
    elapsed = now - runtime->autonomy.state_started_ms;
    active = runtime->autonomy.running || runtime->auto_start_scheduled;
    buffer[0] = '\0';

    /* Do not re-transmit stale samples: the browser must be able to tell that
     * a physical sensor stopped producing data. */
    if (runtime->imu_updated_ms != 0u &&
        (now - runtime->imu_updated_ms) <= SENSOR_FRESH_MS) {
        written = snprintf(buffer + used, buffer_size - used,
            "IMU H:%.2f R:%.2f P:%.2f C:%u\r\n",
            (double)runtime->input.heading_deg, (double)runtime->input.roll_deg,
            (double)runtime->input.pitch_deg,
            (unsigned)runtime->imu.calibration);
        if (written < 0) return -1;
        if ((size_t)written >= buffer_size - used) return (int)(buffer_size - 1u);
        used += (size_t)written;
    }
    if (runtime->depth_updated_ms != 0u &&
        (now - runtime->depth_updated_ms) <= SENSOR_FRESH_MS) {
        written = snprintf(buffer + used, buffer_size - used,
            "DEPTH M:%.3f T:%.2f P:%.2f S:%.3f Z:%u\r\n",
            (double)runtime->depth.vehicle_reference_depth_m,
            (double)runtime->depth.temperature_c, (double)runtime->depth.pressure_mbar,
            (double)runtime->depth.sensor_depth_m,
            runtime->depth_sensor->surface_calibrated ? 1u : 0u);
        if (written < 0) return -1;
        if ((size_t)written >= buffer_size - used) return (int)(buffer_size - 1u);
        used += (size_t)written;
    }
    written = snprintf(buffer + used, buffer_size - used,
        "AUTO STATE:%s ELAPSED:%lu FAULT:%s ACTIVE:%u\r\n",
        runtime->estop_latched ? "ESTOP" : autonomy_state_name(runtime->autonomy.state),
        (unsigned long)elapsed,
        autonomy_fault_name(runtime->autonomy.fault), active ? 1u : 0u);
    if (written < 0) return -1;
    if ((size_t)written >= buffer_size - used) return (int)(buffer_size - 1u);
    return (int)(used + (size_t)written);
}

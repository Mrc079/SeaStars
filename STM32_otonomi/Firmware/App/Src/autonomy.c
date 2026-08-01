#include "autonomy.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define COMPETITION_MIN_STRAIGHT_MS 15000u
static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float wrap_heading(float heading)
{
    while (heading >= 360.0f) heading -= 360.0f;
    while (heading < 0.0f) heading += 360.0f;
    return heading;
}

/* Positive result means clockwise/right turn when heading increases clockwise. */
static float heading_error(float target, float current)
{
    float error = wrap_heading(target) - wrap_heading(current);
    while (error > 180.0f) error -= 360.0f;
    while (error < -180.0f) error += 360.0f;
    return error;
}

static bool valid_config(const autonomy_config_t *config)
{
    return config != NULL &&
           config->route <= AUTO_ROUTE_COMPETITION &&
           config->dive_ballast_pct >= 0.0f && config->dive_ballast_pct <= 100.0f &&
           config->hover_ballast_pct >= 0.0f && config->hover_ballast_pct <= 100.0f &&
           config->test_forward_power_pct >= 0.0f &&
           config->test_forward_power_pct <= 100.0f &&
           config->dive_duration_ms >= 500u && config->dive_duration_ms <= 120000u &&
           config->hover_settle_ms <= 60000u &&
           config->test_forward_duration_ms >= 1000u &&
           config->test_forward_duration_ms <= 120000u &&
           config->straight_duration_ms >= COMPETITION_MIN_STRAIGHT_MS &&
           config->forward_power_pct > 0.0f && config->forward_power_pct <= 100.0f &&
           config->circle_forward_power_pct > 0.0f && config->circle_forward_power_pct <= 100.0f &&
           config->circle_turn_power_pct > 0.0f && config->circle_turn_power_pct <= 100.0f &&
           config->turn_degrees >= 45.0f && config->turn_degrees <= 180.0f &&
           config->heading_kp_pct_per_degree > 0.0f &&
           config->maximum_heading_correction_pct > 0.0f &&
           config->turn_minimum_pct > 0.0f &&
           config->turn_tolerance_deg > 0.0f && config->surface_timeout_ms > 0u &&
           (config->balance_sign == -1.0f || config->balance_sign == 1.0f) &&
           config->balance_axis <= BALANCE_PITCH;
}

static void enter_state(autonomy_t *autonomy, autonomy_state_t state, uint32_t now_ms)
{
    autonomy->state = state;
    autonomy->state_started_ms = now_ms;
    autonomy->stable_since_ms = 0u;
}

static void enter_fault_surface(autonomy_t *autonomy, autonomy_fault_t fault, uint32_t now_ms)
{
    autonomy->fault = fault;
    autonomy->running = false;
    enter_state(autonomy, AUTO_STATE_FAULT_SURFACE, now_ms);
}

static bool settled_for(autonomy_t *autonomy, bool inside_tolerance,
                        uint32_t now_ms, uint32_t required_ms)
{
    if (!inside_tolerance) {
        autonomy->stable_since_ms = 0u;
        return false;
    }
    if (autonomy->stable_since_ms == 0u) {
        autonomy->stable_since_ms = now_ms;
    }
    return (now_ms - autonomy->stable_since_ms) >= required_ms;
}

static void set_thrusters(autonomy_output_t *output, float left, float right)
{
    output->left_thruster_pct = clampf(left, -100.0f, 100.0f);
    output->right_thruster_pct = clampf(right, -100.0f, 100.0f);
}

static void heading_hold(const autonomy_t *autonomy, const autonomy_input_t *input,
                         autonomy_output_t *output, float forward)
{
    float error = heading_error(autonomy->target_heading_deg, input->heading_deg);
    float correction = clampf(
        error * autonomy->config.heading_kp_pct_per_degree,
        -autonomy->config.maximum_heading_correction_pct,
        autonomy->config.maximum_heading_correction_pct);
    set_thrusters(output, forward + correction, forward - correction);
}

static void turn_to_heading(const autonomy_t *autonomy, const autonomy_input_t *input,
                            autonomy_output_t *output)
{
    float error = heading_error(autonomy->target_heading_deg, input->heading_deg);
    float correction = clampf(
        error * autonomy->config.heading_kp_pct_per_degree,
        -autonomy->config.maximum_heading_correction_pct,
        autonomy->config.maximum_heading_correction_pct);
    if (fabsf(error) > autonomy->config.turn_tolerance_deg &&
        fabsf(correction) < autonomy->config.turn_minimum_pct) {
        correction = (error >= 0.0f) ? autonomy->config.turn_minimum_pct
                                     : -autonomy->config.turn_minimum_pct;
    }
    set_thrusters(output, correction, -correction);
}

static void set_ballast(const autonomy_t *autonomy, const autonomy_input_t *input,
                        autonomy_output_t *output, float base_pct)
{
    float balance_angle = 0.0f;
    float balance_pct;
    float tank_pct[2];
    uint8_t index;

    if (autonomy->config.balance_axis == BALANCE_ROLL) {
        balance_angle = input->roll_deg;
    } else if (autonomy->config.balance_axis == BALANCE_PITCH) {
        balance_angle = input->pitch_deg;
    }
    balance_pct = clampf(
        balance_angle * autonomy->config.level_kp_pct_per_degree * autonomy->config.balance_sign,
        -autonomy->config.maximum_balance_pct,
        autonomy->config.maximum_balance_pct);
    tank_pct[0] = clampf(base_pct + balance_pct, 0.0f, 100.0f);
    tank_pct[1] = clampf(base_pct - balance_pct, 0.0f, 100.0f);
    for (index = 0u; index < 2u; ++index) {
        output->tank_target_steps[index] =
            (int32_t)((tank_pct[index] * (float)input->tank_full_steps[index] / 100.0f) + 0.5f);
    }
    output->tank_target_valid = true;
}

static void command_surface(const autonomy_input_t *input, autonomy_output_t *output)
{
    (void)input;
    output->thrusters_armed = false;
    output->tank_target_valid = true;
    output->tank_target_steps[0] = 0;
    output->tank_target_steps[1] = 0;
    set_thrusters(output, 0.0f, 0.0f);
}

void autonomy_default_config(autonomy_config_t *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->route = AUTO_ROUTE_TEST;
    config->dive_ballast_pct = 30.0f;
    config->hover_ballast_pct = 20.0f;
    config->test_forward_power_pct = 15.0f;
    config->dive_duration_ms = 5000u;
    config->hover_settle_ms = 3000u;
    config->test_forward_duration_ms = 5000u;
    config->auto_start_delay_ms = 0u;
    config->auto_zero_tanks = false;
    config->target_depth_m = 0.30f;
    config->maximum_depth_m = 1.50f;
    config->depth_tolerance_m = 0.02f;
    config->surface_complete_depth_m = 0.08f;
    config->ballast_rate_pct_per_m_s = 28.0f;
    config->level_kp_pct_per_degree = 0.5f;
    config->maximum_balance_pct = 12.0f;
    config->heading_kp_pct_per_degree = 0.8f;
    config->maximum_heading_correction_pct = 30.0f;
    config->turn_minimum_pct = 12.0f;
    config->forward_power_pct = 30.0f;
    config->circle_forward_power_pct = 30.0f;
    config->circle_turn_power_pct = 12.0f;
    config->turn_degrees = 90.0f;
    config->turn_tolerance_deg = 3.0f;
    config->balance_sign = 1.0f;
    config->straight_duration_ms = 16000u;
    config->depth_settle_ms = 2000u;
    config->turn_settle_ms = 800u;
    config->dive_timeout_ms = 45000u;
    config->turn_timeout_ms = 20000u;
    config->circle_minimum_ms = 10000u;
    config->circle_timeout_ms = 60000u;
    config->surface_timeout_ms = 60000u;
    config->balance_axis = BALANCE_ROLL;
}

void autonomy_init(autonomy_t *autonomy, const autonomy_config_t *config)
{
    autonomy_config_t defaults;
    if (autonomy == NULL) return;
    memset(autonomy, 0, sizeof(*autonomy));
    autonomy_default_config(&defaults);
    autonomy->config = (config != NULL) ? *config : defaults;
    autonomy->state = AUTO_STATE_IDLE;
}

bool autonomy_start(autonomy_t *autonomy, const autonomy_input_t *input)
{
    if (autonomy == NULL || input == NULL || !valid_config(&autonomy->config)) {
        if (autonomy != NULL) autonomy->fault = AUTO_FAULT_INVALID_CONFIG;
        return false;
    }
    if (!input->imu_fresh) {
        autonomy->fault = AUTO_FAULT_IMU;
        return false;
    }
    if (input->tank_full_steps[0] <= 0 || input->tank_full_steps[1] <= 0 ||
        !input->tank_zeroed[0] || !input->tank_zeroed[1] ||
        input->tank_position[0] > input->tank_full_steps[0] / 100 ||
        input->tank_position[1] > input->tank_full_steps[1] / 100) {
        autonomy->fault = AUTO_FAULT_TANK_CALIBRATION;
        return false;
    }
    autonomy->ballast_fill_pct = autonomy->config.dive_ballast_pct;
    autonomy->target_heading_deg = wrap_heading(input->heading_deg);
    autonomy->previous_heading_deg = autonomy->target_heading_deg;
    autonomy->circle_accumulated_deg = 0.0f;
    autonomy->fault = AUTO_FAULT_NONE;
    autonomy->running = true;
    autonomy->last_update_ms = input->now_ms;
    enter_state(autonomy, AUTO_STATE_DIVE, input->now_ms);
    return true;
}

void autonomy_abort(autonomy_t *autonomy, uint32_t now_ms)
{
    if (autonomy == NULL) return;
    autonomy->running = false;
    enter_state(autonomy, AUTO_STATE_ABORTED, now_ms);
}

void autonomy_tick(autonomy_t *autonomy, const autonomy_input_t *input,
                   autonomy_output_t *output)
{
    uint32_t elapsed;
    bool heading_reached;

    if (autonomy == NULL || input == NULL || output == NULL) return;
    memset(output, 0, sizeof(*output));
    elapsed = input->now_ms - autonomy->state_started_ms;
    autonomy->last_update_ms = input->now_ms;

    if (autonomy->state == AUTO_STATE_IDLE ||
        autonomy->state == AUTO_STATE_COMPLETE) {
        return;
    }
    if (autonomy->state == AUTO_STATE_COUNTDOWN) {
        output->mission_active = true;
        return;
    }
    if (autonomy->state == AUTO_STATE_ABORTED ||
        autonomy->state == AUTO_STATE_FAULT_SURFACE) {
        command_surface(input, output);
        return;
    }
    if (!input->imu_fresh) {
        enter_fault_surface(autonomy, AUTO_FAULT_IMU, input->now_ms);
        command_surface(input, output);
        return;
    }
    if (input->tank_full_steps[0] <= 0 || input->tank_full_steps[1] <= 0 ||
        !input->tank_zeroed[0] || !input->tank_zeroed[1]) {
        enter_fault_surface(autonomy, AUTO_FAULT_TANK_CALIBRATION, input->now_ms);
        command_surface(input, output);
        return;
    }
    if (autonomy->state == AUTO_STATE_SURFACE) {
        bool tanks_empty = input->tank_position[0] <= input->tank_full_steps[0] / 100 &&
                           input->tank_position[1] <= input->tank_full_steps[1] / 100;
        command_surface(input, output);
        if (tanks_empty) {
            autonomy->running = false;
            enter_state(autonomy, AUTO_STATE_COMPLETE, input->now_ms);
        } else if (elapsed >= autonomy->config.surface_timeout_ms) {
            enter_fault_surface(autonomy, AUTO_FAULT_SURFACE_TIMEOUT, input->now_ms);
        }
        return;
    }

    output->mission_active = true;
    output->thrusters_armed = true;

    switch (autonomy->state) {
    case AUTO_STATE_DIVE:
        set_ballast(autonomy, input, output, autonomy->config.dive_ballast_pct);
        set_thrusters(output, 0.0f, 0.0f);
        if (elapsed >= autonomy->config.dive_duration_ms) {
            autonomy->target_heading_deg = wrap_heading(input->heading_deg);
            autonomy->ballast_fill_pct = autonomy->config.hover_ballast_pct;
            enter_state(autonomy, AUTO_STATE_HOVER_SETTLE, input->now_ms);
        }
        break;

    case AUTO_STATE_HOVER_SETTLE:
        set_ballast(autonomy, input, output, autonomy->config.hover_ballast_pct);
        set_thrusters(output, 0.0f, 0.0f);
        if (elapsed >= autonomy->config.hover_settle_ms) {
            autonomy->target_heading_deg = wrap_heading(input->heading_deg);
            enter_state(autonomy,
                autonomy->config.route == AUTO_ROUTE_TEST ? AUTO_STATE_TEST_FORWARD
                                                          : AUTO_STATE_STRAIGHT_1,
                input->now_ms);
        }
        break;

    case AUTO_STATE_TEST_FORWARD:
        set_ballast(autonomy, input, output, autonomy->config.hover_ballast_pct);
        heading_hold(autonomy, input, output, autonomy->config.test_forward_power_pct);
        if (elapsed >= autonomy->config.test_forward_duration_ms) {
            enter_state(autonomy, AUTO_STATE_SURFACE, input->now_ms);
            command_surface(input, output);
        }
        break;

    case AUTO_STATE_STRAIGHT_1:
    case AUTO_STATE_STRAIGHT_2:
    case AUTO_STATE_STRAIGHT_3:
    case AUTO_STATE_STRAIGHT_4:
        set_ballast(autonomy, input, output, autonomy->config.hover_ballast_pct);
        heading_hold(autonomy, input, output, autonomy->config.forward_power_pct);
        if (elapsed >= autonomy->config.straight_duration_ms) {
            if (autonomy->state == AUTO_STATE_STRAIGHT_1 ||
                autonomy->state == AUTO_STATE_STRAIGHT_3) {
                autonomy->target_heading_deg = wrap_heading(
                    input->heading_deg + autonomy->config.turn_degrees);
                enter_state(autonomy,
                    autonomy->state == AUTO_STATE_STRAIGHT_1 ? AUTO_STATE_TURN_1 : AUTO_STATE_TURN_2,
                    input->now_ms);
            } else if (autonomy->state == AUTO_STATE_STRAIGHT_2) {
                autonomy->previous_heading_deg = wrap_heading(input->heading_deg);
                autonomy->circle_accumulated_deg = 0.0f;
                enter_state(autonomy, AUTO_STATE_CIRCLE, input->now_ms);
            } else {
                enter_state(autonomy, AUTO_STATE_SURFACE, input->now_ms);
                command_surface(input, output);
            }
        }
        break;

    case AUTO_STATE_TURN_1:
    case AUTO_STATE_TURN_2:
        set_ballast(autonomy, input, output, autonomy->config.hover_ballast_pct);
        turn_to_heading(autonomy, input, output);
        heading_reached = fabsf(heading_error(
            autonomy->target_heading_deg, input->heading_deg)) <= autonomy->config.turn_tolerance_deg;
        if (settled_for(autonomy, heading_reached, input->now_ms,
                        autonomy->config.turn_settle_ms)) {
            autonomy->target_heading_deg = wrap_heading(input->heading_deg);
            enter_state(autonomy,
                autonomy->state == AUTO_STATE_TURN_1 ? AUTO_STATE_STRAIGHT_2 : AUTO_STATE_STRAIGHT_4,
                input->now_ms);
        } else if (elapsed >= autonomy->config.turn_timeout_ms) {
            enter_fault_surface(autonomy, AUTO_FAULT_TURN_TIMEOUT, input->now_ms);
            command_surface(input, output);
        }
        break;

    case AUTO_STATE_CIRCLE: {
        float delta = heading_error(input->heading_deg, autonomy->previous_heading_deg);
        set_ballast(autonomy, input, output, autonomy->config.hover_ballast_pct);
        autonomy->previous_heading_deg = wrap_heading(input->heading_deg);
        if (delta > 0.0f && delta < 45.0f) {
            autonomy->circle_accumulated_deg += delta;
        }
        set_thrusters(output,
            autonomy->config.circle_forward_power_pct + autonomy->config.circle_turn_power_pct,
            autonomy->config.circle_forward_power_pct - autonomy->config.circle_turn_power_pct);
        if (autonomy->circle_accumulated_deg >= 360.0f &&
            elapsed >= autonomy->config.circle_minimum_ms) {
            autonomy->target_heading_deg = wrap_heading(input->heading_deg);
            enter_state(autonomy, AUTO_STATE_STRAIGHT_3, input->now_ms);
        } else if (elapsed >= autonomy->config.circle_timeout_ms) {
            enter_fault_surface(autonomy, AUTO_FAULT_CIRCLE_TIMEOUT, input->now_ms);
            command_surface(input, output);
        }
        break;
    }

    default:
        enter_fault_surface(autonomy, AUTO_FAULT_INVALID_CONFIG, input->now_ms);
        command_surface(input, output);
        break;
    }
}

const char *autonomy_state_name(autonomy_state_t state)
{
    static const char *const names[] = {
        "IDLE", "COUNTDOWN", "DIVE", "HOVER_SETTLE", "TEST_FORWARD",
        "STRAIGHT_1", "TURN_1", "STRAIGHT_2", "CIRCLE", "STRAIGHT_3",
        "TURN_2", "STRAIGHT_4", "SURFACE", "COMPLETE", "ABORTED",
        "FAULT_SURFACE"
    };
    return (state <= AUTO_STATE_FAULT_SURFACE) ? names[state] : "UNKNOWN";
}

const char *autonomy_fault_name(autonomy_fault_t fault)
{
    static const char *const names[] = {
        "NONE", "INVALID_CONFIG", "DEPTH_SENSOR", "IMU", "TANK_CALIBRATION",
        "DIVE_TIMEOUT", "TURN_TIMEOUT", "CIRCLE_TIMEOUT", "DEPTH_LIMIT",
        "SURFACE_TIMEOUT"
    };
    return (fault <= AUTO_FAULT_SURFACE_TIMEOUT) ? names[fault] : "UNKNOWN";
}

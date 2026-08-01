#ifndef SEASTARS_AUTONOMY_H
#define SEASTARS_AUTONOMY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUTO_STATE_IDLE = 0,
    AUTO_STATE_COUNTDOWN,
    AUTO_STATE_DIVE,
    AUTO_STATE_HOVER_SETTLE,
    AUTO_STATE_TEST_FORWARD,
    AUTO_STATE_STRAIGHT_1,
    AUTO_STATE_TURN_1,
    AUTO_STATE_STRAIGHT_2,
    AUTO_STATE_CIRCLE,
    AUTO_STATE_STRAIGHT_3,
    AUTO_STATE_TURN_2,
    AUTO_STATE_STRAIGHT_4,
    AUTO_STATE_SURFACE,
    AUTO_STATE_COMPLETE,
    AUTO_STATE_ABORTED,
    AUTO_STATE_FAULT_SURFACE
} autonomy_state_t;

typedef enum {
    AUTO_FAULT_NONE = 0,
    AUTO_FAULT_INVALID_CONFIG,
    AUTO_FAULT_DEPTH_SENSOR,
    AUTO_FAULT_IMU,
    AUTO_FAULT_TANK_CALIBRATION,
    AUTO_FAULT_DIVE_TIMEOUT,
    AUTO_FAULT_TURN_TIMEOUT,
    AUTO_FAULT_CIRCLE_TIMEOUT,
    AUTO_FAULT_DEPTH_LIMIT,
    AUTO_FAULT_SURFACE_TIMEOUT
} autonomy_fault_t;

typedef enum {
    AUTO_ROUTE_TEST = 0,
    AUTO_ROUTE_COMPETITION
} autonomy_route_t;

typedef enum {
    BALANCE_DISABLED = 0,
    BALANCE_ROLL,
    BALANCE_PITCH
} autonomy_balance_axis_t;

typedef struct {
    autonomy_route_t route;
    float dive_ballast_pct;
    float hover_ballast_pct;
    float test_forward_power_pct;
    uint32_t dive_duration_ms;
    uint32_t hover_settle_ms;
    uint32_t test_forward_duration_ms;
    uint32_t auto_start_delay_ms;
    bool auto_zero_tanks;
    /* Retained for protocol compatibility. Depth feedback is deliberately
     * not used by the open-loop ballast mission. */
    float target_depth_m;
    float maximum_depth_m;
    float depth_tolerance_m;
    float surface_complete_depth_m;
    float ballast_rate_pct_per_m_s;
    float level_kp_pct_per_degree;
    float maximum_balance_pct;
    float heading_kp_pct_per_degree;
    float maximum_heading_correction_pct;
    float turn_minimum_pct;
    float forward_power_pct;
    float circle_forward_power_pct;
    float circle_turn_power_pct;
    float turn_degrees;
    float turn_tolerance_deg;
    float balance_sign;
    uint32_t straight_duration_ms;
    uint32_t depth_settle_ms;
    uint32_t turn_settle_ms;
    uint32_t dive_timeout_ms;
    uint32_t turn_timeout_ms;
    uint32_t circle_minimum_ms;
    uint32_t circle_timeout_ms;
    uint32_t surface_timeout_ms;
    autonomy_balance_axis_t balance_axis;
} autonomy_config_t;

typedef struct {
    uint32_t now_ms;
    float depth_m;
    float sensor_depth_m;
    float heading_deg;
    float roll_deg;
    float pitch_deg;
    int32_t tank_position[2];
    int32_t tank_full_steps[2];
    bool tank_zeroed[2];
    bool depth_fresh;
    bool imu_fresh;
} autonomy_input_t;

typedef struct {
    float left_thruster_pct;
    float right_thruster_pct;
    int32_t tank_target_steps[2];
    bool thrusters_armed;
    bool tank_target_valid;
    bool mission_active;
} autonomy_output_t;

typedef struct {
    autonomy_config_t config;
    autonomy_state_t state;
    autonomy_fault_t fault;
    uint32_t state_started_ms;
    uint32_t stable_since_ms;
    uint32_t last_update_ms;
    float target_heading_deg;
    float previous_heading_deg;
    float circle_accumulated_deg;
    float ballast_fill_pct;
    bool running;
} autonomy_t;

void autonomy_default_config(autonomy_config_t *config);
void autonomy_init(autonomy_t *autonomy, const autonomy_config_t *config);
bool autonomy_start(autonomy_t *autonomy, const autonomy_input_t *input);
void autonomy_abort(autonomy_t *autonomy, uint32_t now_ms);
void autonomy_tick(autonomy_t *autonomy, const autonomy_input_t *input, autonomy_output_t *output);
const char *autonomy_state_name(autonomy_state_t state);
const char *autonomy_fault_name(autonomy_fault_t fault);

#endif

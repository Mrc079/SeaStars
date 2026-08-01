#ifndef SEASTARS_IMU_SAMPLE_H
#define SEASTARS_IMU_SAMPLE_H

#include <stdint.h>

typedef struct {
    float yaw_deg;
    float pitch_deg;
    float roll_deg;
    float accel_x_m_s2;
    float accel_y_m_s2;
    float accel_z_m_s2;
    uint8_t sequence;
    uint8_t calibration;
} imu_sample_t;

#endif

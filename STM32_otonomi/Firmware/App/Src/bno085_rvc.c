#include "bno085_rvc.h"

#include <stddef.h>
#include <string.h>

#define DEGREE_SCALE 0.01f
#define MILLIG_TO_M_S2 0.0098067f

static int16_t little_endian_i16(const uint8_t *bytes)
{
    return (int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

void bno085_rvc_init(bno085_rvc_t *imu)
{
    if (imu != NULL) {
        memset(imu, 0, sizeof(*imu));
    }
}

bool bno085_rvc_feed(bno085_rvc_t *imu, uint8_t byte, bno085_rvc_sample_t *sample)
{
    uint8_t checksum = 0u;
    uint8_t index;

    if (imu == NULL || sample == NULL) {
        return false;
    }
    if (imu->index == 0u) {
        if (byte == 0xAAu) {
            imu->frame[imu->index++] = byte;
        }
        return false;
    }
    if (imu->index == 1u) {
        if (byte == 0xAAu) {
            imu->frame[imu->index++] = byte;
        } else {
            imu->index = 0u;
        }
        return false;
    }
    imu->frame[imu->index++] = byte;
    if (imu->index < BNO085_RVC_FRAME_SIZE) {
        return false;
    }
    imu->index = 0u;
    for (index = 2u; index < 18u; ++index) {
        checksum = (uint8_t)(checksum + imu->frame[index]);
    }
    if (checksum != imu->frame[18]) {
        imu->checksum_errors++;
        return false;
    }
    sample->sequence = imu->frame[2];
    sample->yaw_deg = (float)little_endian_i16(&imu->frame[3]) * DEGREE_SCALE;
    sample->pitch_deg = (float)little_endian_i16(&imu->frame[5]) * DEGREE_SCALE;
    sample->roll_deg = (float)little_endian_i16(&imu->frame[7]) * DEGREE_SCALE;
    sample->accel_x_m_s2 = (float)little_endian_i16(&imu->frame[9]) * MILLIG_TO_M_S2;
    sample->accel_y_m_s2 = (float)little_endian_i16(&imu->frame[11]) * MILLIG_TO_M_S2;
    sample->accel_z_m_s2 = (float)little_endian_i16(&imu->frame[13]) * MILLIG_TO_M_S2;
    sample->calibration = 255u;
    imu->valid_frames++;
    return true;
}

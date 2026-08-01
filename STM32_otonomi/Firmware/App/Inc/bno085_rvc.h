#ifndef SEASTARS_BNO085_RVC_H
#define SEASTARS_BNO085_RVC_H

#include "imu_sample.h"

#include <stdbool.h>
#include <stdint.h>

#define BNO085_RVC_FRAME_SIZE 19u

/* Kept only for source compatibility with the retired UART-RVC parser. */
typedef imu_sample_t bno085_rvc_sample_t;

typedef struct {
    uint8_t frame[BNO085_RVC_FRAME_SIZE];
    uint8_t index;
    uint32_t valid_frames;
    uint32_t checksum_errors;
} bno085_rvc_t;

void bno085_rvc_init(bno085_rvc_t *imu);
bool bno085_rvc_feed(bno085_rvc_t *imu, uint8_t byte, bno085_rvc_sample_t *sample);

#endif

#ifndef ANDROID_DVR_POSE_IPC_H_
#define ANDROID_DVR_POSE_IPC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DVR_POSE_SERVICE_BASE "system/vr/pose"
#define DVR_POSE_SERVICE_CLIENT (DVR_POSE_SERVICE_BASE "/client")

enum {
  DVR_POSE_FREEZE = 0,
  DVR_POSE_SET_MODE,
  DVR_POSE_GET_MODE,
  DVR_POSE_GET_CONTROLLER_RING_BUFFER,
  DVR_POSE_LOG_CONTROLLER,
  DVR_POSE_SENSORS_ENABLE,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ANDROID_DVR_POSE_IPC_H_

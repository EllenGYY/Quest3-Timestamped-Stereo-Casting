#ifndef DEVICE_TIME_H
#define DEVICE_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Get device boot time in milliseconds
int64_t get_device_boot_time(const char* ip_port, const char* adb_path);

// Convert timestamp to human readable format
const char* fromTimestamp(int64_t timestamp);

#ifdef __cplusplus
}
#endif

#endif 
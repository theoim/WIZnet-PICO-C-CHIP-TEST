/*
 * Shim for esp_log.h, so the vendor sensor driver compiles unmodified.
 *
 * Errors and warnings go to stdout; info and below are compiled out. The driver
 * logs one line per register group at debug level, and on a 2 MB part with USB
 * CDC as the only console that is both wasted flash and a real time cost during
 * sensor init, which runs several hundred register writes.
 *
 * Raise CAM_LOG_LEVEL to 3 to get the info lines back while bringing a new
 * sensor up.
 */
#ifndef _CAM_SHIM_ESP_LOG_H_
#define _CAM_SHIM_ESP_LOG_H_

#include <stdio.h>

#ifndef CAM_LOG_LEVEL
#define CAM_LOG_LEVEL 2         /* 0 none, 1 error, 2 +warn, 3 +info, 4 +debug */
#endif

#if CAM_LOG_LEVEL >= 1
#define ESP_LOGE(tag, fmt, ...) printf("[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif

#if CAM_LOG_LEVEL >= 2
#define ESP_LOGW(tag, fmt, ...) printf("[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#endif

#if CAM_LOG_LEVEL >= 3
#define ESP_LOGI(tag, fmt, ...) printf("[I %s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#endif

#if CAM_LOG_LEVEL >= 4
#define ESP_LOGD(tag, fmt, ...) printf("[D %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) printf("[V %s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
#endif

#endif /* _CAM_SHIM_ESP_LOG_H_ */

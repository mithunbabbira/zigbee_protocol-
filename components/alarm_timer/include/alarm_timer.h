#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t alarm_timer_arg_t;
typedef void (*alarm_timer_callback_t)(alarm_timer_arg_t arg);

esp_err_t alarm_timer_schedule(alarm_timer_callback_t cb, alarm_timer_arg_t arg, uint32_t time_ms);

#ifdef __cplusplus
}
#endif

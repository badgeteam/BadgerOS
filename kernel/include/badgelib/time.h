
// SPDX-License-Identifier: MIT

#pragma once

#include "port/time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TIMESTAMP_US_MIN INT64_MIN
#define TIMESTAMP_US_MAX INT64_MAX

typedef int64_t timestamp_us_t;

// Timer callback function.
typedef void (*timer_fn_t)(void *cookie);

// Timer callback.
typedef struct {
    // Timer task no.
    int64_t        taskno;
    // Timestamp to run callback at.
    timestamp_us_t timestamp;
    // Timer callback function.
    timer_fn_t     callback;
    // Cookie for timer callback function.
    void          *cookie;
} timertask_t;

// Sets the alarm time when the next callback switch should occur.
void           time_set_next_task_switch(timestamp_us_t timestamp);
// Attach a callback to a timer interrupt.
// The callback with the lowest timestamp is likeliest, but not guaranteed, to run first.
// Returns whether the task was successfully added.
bool           time_add_async_task(timertask_t *task);
// Cancel a callback created with `time_add_async_task`.
bool           time_cancel_async_task(int64_t taskno);
// Get current time in microseconds.
timestamp_us_t time_us();

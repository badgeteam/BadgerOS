#pragma once

#define BADGEROS_MALLOC_DEBUG_NONE  0
#define BADGEROS_MALLOC_DEBUG_ERROR 1
#define BADGEROS_MALLOC_DEBUG_WARN  2
#define BADGEROS_MALLOC_DEBUG_INFO  3
#define BADGEROS_MALLOC_DEBUG_DEBUG 4
#define BADGEROS_MALLOC_DEBUG_TRACE 5

#ifndef BADGEROS_MALLOC_DEBUG_LEVEL
#define BADGEROS_MALLOC_DEBUG_LEVEL BADEROS_MALLOC_DEBUG_NONE
#endif

#ifdef BADGEROS_MALLOC_STANDALONE
#define _GNU_SOURCE
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>

#define BADGEROS_MALLOC_ASSERT(cond, level, format, ...)                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%i: %-7s: %s %s():%d : Assertion failed: " format "\n", gettid(), level, __FILE__, __func__, __LINE__, ##__VA_ARGS__);    \
            raise(SIGINT);                                                                                             \
        }                                                                                                              \
    } while (0)


#define BADGEROS_MALLOC_DEBUG_MSG(level, format, ...)                                                                  \
    do {                                                                                                               \
        fprintf(stderr, "%i: %-7s: %s %s():%d : " format "\n", gettid(), level, __FILE__, __func__, __LINE__, ##__VA_ARGS__);        \
    } while (0)

#define BADGEROS_MALLOC_ASSERT_ERROR(cond, format, ...) BADGEROS_MALLOC_ASSERT(cond, "ERROR", format, ##__VA_ARGS__)
#define BADGEROS_MALLOC_MSG_ERROR(format, ...)  BADGEROS_MALLOC_DEBUG_MSG("ERROR", format, ##__VA_ARGS__)

#if BADGEROS_MALLOC_DEBUG_LEVEL >= BADGEROS_MALLOC_DEBUG_WARN
#define BADGEROS_MALLOC_ASSERT_WARN(cond, format, ...) BADGEROS_MALLOC_ASSERT(cond, "WARNING", format, ##__VA_ARGS__)
#define BADGEROS_MALLOC_MSG_WARN(format, ...)  BADGEROS_MALLOC_DEBUG_MSG("WARNING", format, ##__VA_ARGS__)
#endif

#if BADGEROS_MALLOC_DEBUG_LEVEL >= BADGEROS_MALLOC_DEBUG_INFO
#define BADGEROS_MALLOC_ASSERT_INFO(cond, format, ...) BADGEROS_MALLOC_ASSERT(cond, "INFO", format, ##__VA_ARGS__)
#define BADGEROS_MALLOC_MSG_INFO(format, ...)  BADGEROS_MALLOC_DEBUG_MSG("INFO", format, ##__VA_ARGS__)
#endif

#if BADGEROS_MALLOC_DEBUG_LEVEL >= BADGEROS_MALLOC_DEBUG_DEBUG
#define BADGEROS_MALLOC_ASSERT_DEBUG(cond, format, ...) BADGEROS_MALLOC_ASSERT(cond, "DEBUG", format, ##__VA_ARGS__)
#define BADGEROS_MALLOC_MSG_DEBUG(format, ...)  BADGEROS_MALLOC_DEBUG_MSG("DEBUG", format, ##__VA_ARGS__)
#endif

#if BADGEROS_MALLOC_DEBUG_LEVEL >= BADGEROS_MALLOC_DEBUG_TRACE
#define BADGEROS_MALLOC_ASSERT_TRACE(cond, format, ...) BADGEROS_MALLOC_ASSERT(cond, "TRACE", format, ##__VA_ARGS__)
#define BADGEROS_MALLOC_MSG_TRACE(format, ...)  BADGEROS_MALLOC_DEBUG_MSG("TRACE", format, ##__VA_ARGS__)
#endif

#endif

#ifdef BADGEROS_KERNEL
#endif

// Define all of the macros in case they were not defined above

#ifndef BADGEROS_MALLOC_ASSERT_ERROR
#define BADGEROS_MALLOC_ASSERT_ERROR(cond, format, ...)
#endif
#ifndef BADGEROS_MALLOC_ASSERT_WARN
#define BADGEROS_MALLOC_ASSERT_WARN(cond, format, ...)
#endif
#ifndef BADGEROS_MALLOC_ASSERT_INFO
#define BADGEROS_MALLOC_ASSERT_INFO(cond, format, ...)
#endif
#ifndef BADGEROS_MALLOC_ASSERT_DEBUG
#define BADGEROS_MALLOC_ASSERT_DEBUG(cond, format, ...)
#endif
#ifndef BADGEROS_MALLOC_ASSERT_TRACE
#define BADGEROS_MALLOC_ASSERT_TRACE(cond, format, ...)
#endif

#ifndef BADGEROS_MALLOC_MSG_ERROR
#define BADGEROS_MALLOC_MSG_ERROR(format, ...)
#endif
#ifndef BADGEROS_MALLOC_MSG_WARN
#define BADGEROS_MALLOC_MSG_WARN(format, ...)
#endif
#ifndef BADGEROS_MALLOC_MSG_INFO
#define BADGEROS_MALLOC_MSG_INFO(format, ...)
#endif
#ifndef BADGEROS_MALLOC_MSG_DEBUG
#define BADGEROS_MALLOC_MSG_DEBUG(format, ...)
#endif
#ifndef BADGEROS_MALLOC_MSG_TRACE
#define BADGEROS_MALLOC_MSG_TRACE(format, ...)
#endif

/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_logging.c

DESCRIPTION
    Implementation file for the logging function of Cloud Connect MQTT service.
*/
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

#include "cloud_connect_mqtt_logging.h"
#include "cloud_connect_mqtt_status.h"

#define TIMESTAMP_BUFFER_SIZE 50
/* Offset to convert tm_year to the actual year */
#define YEAR_OFFSET 1900
/* Number of milliseconds in a second */
#define MILLISECONDS_IN_SECOND 1000

/* Global variable to store the current log level */
static LogLevel current_log_level = LOG_DEBUG;

int set_log_level(LogLevel level)
{
    current_log_level = level;
    return 0;
}

static const char *get_timestamp()
{
    static char buffer[TIMESTAMP_BUFFER_SIZE];
    struct timeval tv;
    struct tm *tm;
    gettimeofday(&tv, NULL);
    tm = localtime(&tv.tv_sec);

    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d,%03d",
             tm->tm_year + YEAR_OFFSET,
             tm->tm_mon + 1,
             tm->tm_mday,
             tm->tm_hour,
             tm->tm_min,
             tm->tm_sec,
             (int)tv.tv_usec / MILLISECONDS_IN_SECOND);
    return buffer;
}

static const char *get_log_level(LogLevel level)
{
    switch (level)
    {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARNING:
            return "WARNING";
        case LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

int get_log_level_config(char *level)
{
    if (strcmp(level, "DEBUG") == 0)
        return LOG_DEBUG;

    if (strcmp(level, "WARNING") == 0)
        return LOG_WARNING;

    if (strcmp(level, "INFO") == 0)
        return LOG_INFO;

    if (strcmp(level, "ERROR") == 0)
        return LOG_ERROR;

    return CCM_STATUS_FAIL;
}

void log_printf(LogLevel level, const char *filename, int line, const char *format, ...)
{
    if (level < current_log_level)
    {
        return; // Skip logging if the message level is lower than the current log level
    }

    va_list args;
    va_start(args, format);

    // Print the log header
    printf("[CCM %s, %s, %s:%d] - ", get_timestamp(), get_log_level(level), filename, line);

    // Print the actual message
    vprintf(format, args);
    printf("\n");

    va_end(args);
}
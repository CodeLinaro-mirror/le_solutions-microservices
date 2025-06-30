/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_logging.h

DESCRIPTION
    Header file for the logging function of Cloud Connect kafka service.
*/

#ifndef CLOUD_CONNECT_KAFKA_LOGGING_H_
#define CLOUD_CONNECT_KAFKA_LOGGING_H_

#include <stdint.h>
#include <stdarg.h>
#include <time.h>

/**
 * @enum LogLevel
 * @brief Enum for different log levels
 */
typedef enum
{
    LOG_DEBUG = 10,   /*!< log level debug */
    LOG_INFO = 11,    /*!< log level info */
    LOG_WARNING = 12, /*!< log level warning */
    LOG_ERROR = 13    /*!< log lever error */
} LogLevel;

/**
 * @fn set_log_level(LogLevel level)
 * @brief Function to set the log level.
 *
 * @param level LogLevel enum
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int set_log_level(LogLevel level);

/**
 * @fn get_log_level_config(char *level)
 * @brief Function to get the log level.
 *
 * @param level string loglevel
 * @return int
 * @retval LogLevel: respective int log level
 * @retval CCM_STATUS_FAIL: in case of unrecognised log_level
 */
int get_log_level_config(char *level);

// Wrapper function for printf
void log_printf(LogLevel level, const char *filename, int line, const char *format, ...);

// Macros to simplify logging
#define LOGD(format, ...) log_printf(LOG_DEBUG, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOGI(format, ...) log_printf(LOG_INFO, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOGW(format, ...) log_printf(LOG_WARNING, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOGE(format, ...) log_printf(LOG_ERROR, __FILE__, __LINE__, format, ##__VA_ARGS__)

#endif /* CLOUD_CONNECT_KAFKA_LOGGING_H_ */
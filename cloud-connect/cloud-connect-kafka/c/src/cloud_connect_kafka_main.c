/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_main.c

DESCRIPTION
    This file is the entry point of Cloud Connect kafka service.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "cloud_connect_kafka_logging.h"
#include "cloud_connect_kafka_config.h"
#include "cloud_connect_kafka_redis_communicator.h"
#include "cloud_connect_kafka_message_communicator.h"
#include "cloud_connect_kafka_status.h"
#include "cloud_connect_kafka_message_communicator.h"

#define SLEEP_TIME_MAIN_THREAD 5

/**
 * @fn  void signalHandler(int signum)
 * @brief Function to to catch the SIGINT signal and gracefull exit the application.-
 *
 * @param signum int the signal id
 *
 */
static void signalHandler(int signum)
{
    LOGI("Interrupt signal (%d) received.\n", signum);
    exit(signum);
}

int main()
{
    LOGD("Cloud connect KAFKA service entering main()..!\n");

    // Register a signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGKILL, signalHandler);

    CCKStatus status = cloud_connect_kafka_init_config();
    if (CCK_STATUS_SUCCESS != status)
    {
        LOGE("Failed to get the configuration, status = %d\n", status);
        return EXIT_FAILURE;
    }

    cck_sys_config_t *p_config = cloud_connect_kafka_get_config();
    if (p_config == NULL)
    {
        LOGE("Failed to get the configuration, status\n");
        return EXIT_FAILURE;
    }

    /* set log level */
    if (p_config->log_level != NULL)
    {
        int log_level = get_log_level_config(p_config->log_level);
        if (log_level != CCK_STATUS_FAIL)
        {
            set_log_level(log_level);
        }
    }

    status = cloud_connect_kafka_init_redis_communicator(p_config);
    if (status != 0)
    {
        LOGE("Failed to initialize redis communicator module\n");
        return EXIT_FAILURE;
    }
    status = cloud_connect_kafka_init_msg_communicator(p_config);
    if (status != CCK_STATUS_SUCCESS)
    {
        LOGE("Failed to initialize kafka message communicator\n");
        return EXIT_FAILURE;
    }
    else
    {
        LOGI("Success: Kafka publisher initialized\n");
    }

    while (1)
    {
        sleep(SLEEP_TIME_MAIN_THREAD);
    }

    return EXIT_SUCCESS;
}

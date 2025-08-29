/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_main.c

DESCRIPTION
    This file is the entry point of Cloud Connect MQTT service.
*/
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "cloud_connect_mqtt_status.h"
#include "cloud_connect_mqtt_logging.h"
#include "cloud_connect_mqtt_config.h"
#include "cloud_connect_mqtt_redis_communicator.h"
#include "cloud_connect_mqtt_message_communicator.h"

#define SLEEP_TIME_MAIN_THREAD 2
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
    LOGD("Cloud connect MQTT service started..!\n");

    // Register a signal handler
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    CCMStatus status = cloud_connect_mqtt_init_config();
    if (CCM_STATUS_SUCCESS != status)
    {
        LOGE("Failed to get the configuration, status = %d\n", status);
        return EXIT_FAILURE;
    }

    ccm_sys_config_t *p_config = cloud_connect_mqtt_get_config();

    if (p_config == NULL)
    {
        LOGE("Failed to get the configuration, status\n");
        return EXIT_FAILURE;
    }

    /* set log level */
    if (p_config->log_level != NULL)
    {
        int log_level = get_log_level_config(p_config->log_level);
        if (log_level != CCM_STATUS_FAIL)
        {
            set_log_level(log_level);
        }
    }

    status = cloud_connect_mqtt_init_redis_communicator(p_config);
    if (status != CCM_STATUS_SUCCESS)
    {
        LOGE("Failed to initialize redis communicator module\n");
        return EXIT_FAILURE;
    }

    status = cloud_connect_mqtt_init_msg_communicator(p_config);
    if (status != CCM_STATUS_SUCCESS)
    {
        LOGE("Failed to initialize mqtt message communicator\n");
    }
    else
    {
        LOGI("Success: mqtt message communicator initialized\n");
    }

    while (1)
    {
        // To keep the main process alive
        sleep(SLEEP_TIME_MAIN_THREAD);

        /* Retry connection if previous attempt was failed */
        if (status != CCM_STATUS_SUCCESS)
        {
            status = cloud_connect_mqtt_init_msg_communicator(p_config);
        }
    }

    return EXIT_SUCCESS;
}

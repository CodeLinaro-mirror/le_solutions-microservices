/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_env_utils.c

DESCRIPTION
    Implementation file for reading the environment variables.
*/
#include <stdlib.h>
#include <string.h>

#include "cloud_connect_mqtt_env_utils.h"

const char *cloud_connect_mqtt_get_env_value_str(const char *env_var, const char *default_value)
{
    const char *value = getenv(env_var);
    return (value != NULL && strlen(value) > 0) ? value : default_value;
}

int cloud_connect_mqtt_get_env_value_int(const char *env_var, int default_value)
{
    const char *value = getenv(env_var);
    return (value != NULL && strlen(value) > 0) ? atoi(value) : default_value;
}
/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_env_utils.h

DESCRIPTION
    Header file for the reading the environment variables.
*/

#ifndef CLOUD_CONNECT_MQTT_ENV_UTILS_H_
#define CLOUD_CONNECT_MQTT_ENV_UTILS_H_

/**
 * @fn cloud_connect_mqtt_get_env_value_str(const char* env_var_key, const char* default_value)
 * @brief Function to get environment variable value or default value of type string.
 *
 * @param env_var_key The key for environment variable.
 * @return const char* the value or the default value.
 */
const char *cloud_connect_mqtt_get_env_value_str(const char *env_var_key, const char *default_value);

/**
 * @fn cloud_connect_mqtt_get_env_value_int(const char* env_var_key, int default_value);
 * @brief Function to get environment variable value or default value of type int.
 *
 * @param env_var_key The key for environment variable.
 * @return int the value or the default value.
 */
int cloud_connect_mqtt_get_env_value_int(const char *env_var_key, int default_value);

#endif /* CLOUD_CONNECT_MQTT_ENV_UTILS_H_ */
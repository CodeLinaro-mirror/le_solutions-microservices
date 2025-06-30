/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_config.h

DESCRIPTION
    Header file for the configuration of Cloud Connect MQTT service.
*/

#ifndef CLOUD_CONNECT_MQTT_CONFIG_H_
#define CLOUD_CONNECT_MQTT_CONFIG_H_

#include <stdbool.h>

/**
 * @struct Defines channel type.
 * @brief Enum representing different types of channel to subscribe mqtt client.
 */
typedef enum channel_map_type
{
    PATTERN = 0,
    CHANNEL
} channel_map_type_t;

/**
 * @struct channel_mapping
 * @brief Cloud connect channel mapping structure
 */
typedef struct ccm_ssl_config
{   
    char *root_ca_file;               /*!< rootca file path .*/
    char *client_cert_file;           /*!< cert file path .*/
    char *client_private_key_file;    /*!< private key file path .*/
    char *client_cert_password;       /*!< client certificate password to be used.*/
    bool is_domain_validation_enabled;/*!< True if certificates support domain name validation */
}ccm_ssl_config_t;

/**
 * @struct channel_mapping
 * @brief Cloud connect channel mapping structure
 */
typedef struct ccm_channel_mapping
{
    int num_channels;   /*!< number of channels */
    char **channel_src; /*!< redis channel name or pattern to be subscribed */
    char **channel_dst; /*!< mqtt channel name or pattern over which data to be published */
} ccm_channel_mapping_t;

/**
 * @struct sys_config
 * @brief Cloud connect config structure
 */
typedef struct ccm_sys_config
{
    /* Environment variable driven params*/
    char *redis_host;                       /*!< redis host name */
    int redis_port;                         /*!< redis port number */
    char *mqtt_host;                        /*!< mqtt server host name */
    int mqtt_port;                          /*!< mqtt port number */
    /* Configuration file driven params*/
    char *log_level;                        /*!< log level */
    bool msg_err_handling;                  /*!< a boolean to indicate msg error handling*/
    int channel_len;                        /*!< redis channel name or pattern to be subscribed */
    ccm_channel_mapping_t *channel_mapping; /*!< list of channel mapping */
    char *client_id_prefix;                 /*!< Prefix to be used for client id. Max 20 characters */
    bool enable_ssl_connection;             /*!< a boolean to indicate secure connection is enabled or not */
    ccm_ssl_config_t ssl_config;              /*!< SSL configurations */
} ccm_sys_config_t;

/**
 * @fn cloud_connect_mqtt_init_config()
 * @brief Function to initialize the configuration of the cloud connect mqtt microservice.
 *
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_mqtt_init_config();

/**
 * @fn cloud_connect_mqtt_get_config()
 * @brief Function to get loaded configuration of the cloud connect mqtt microservice.
 *
 * @return ccm_sys_config_t *
 */
ccm_sys_config_t *cloud_connect_mqtt_get_config();

/**
 * @fn cloud_connect_mqtt_deinit_config()
 * @brief Function to free the configuration of the cloud connect mqtt microservice.
 *
 * @return void
 */
void cloud_connect_mqtt_deinit_config();

#endif /* CLOUD_CONNECT_MQTT_LOGGING_H_ */

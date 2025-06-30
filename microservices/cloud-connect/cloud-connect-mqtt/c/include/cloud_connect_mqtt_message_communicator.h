/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_message_communicator.h

DESCRIPTION
    Header file for the message communicator module of Cloud Connect MQTT service.
*/

#ifndef CLOUD_CONNECT_MQTT_MSG_COMMUNICATOR_H_
#define CLOUD_CONNECT_MQTT_MSG_COMMUNICATOR_H_

#include <stdint.h>
#include "cloud_connect_mqtt_config.h"
#ifdef UNIT_TEST
#include "../tests/mosquitto_mock.h"
#endif

/**
 * @struct ccm_message
 * @brief Cloud connect mqtt message structure
 */
typedef struct ccm_message
{
    char *channel_src;     /*!< redis channel name */
    char *channel_dst;     /*!< mqtt channel name */
    int msg_len_bytes;     /*!< length of message in bytes. */
    uint8_t *msg;          /*!< pointer to message data */
    uint8_t connection_id; /*!< When there are multiple mqtt publishers, the connection_id
                                indicates which publisher will handle this message. */
    uint64_t timestamp;    /*!< time of receving the message */

} ccm_message_t;

/* callback for MQTT connection status*/
typedef void (*ccm_mqtt_conn_cb_t)(int status, int conn_id);

/**
 * @fn cloud_connect_mqtt_init_msg_communicator(ccm_sys_config_t *config)
 * @brief Function to initialize the mqtt communicator module of the cloud connect mqtt microservice.
 *
 * @param config ccm_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_mqtt_init_msg_communicator(ccm_sys_config_t *config);

/**
 * @fn cloud_connect_mqtt_send_message(ccm_message_t *config)
 * @brief Function to send message received from redis to mqtt.
 *
 * @param msg ccm_message_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_mqtt_send_message(ccm_message_t *msg);

/**
 * @fn cloud_connect_mqtt_register_conn_event(ccm_mqtt_conn_cb_t cb)
 * @brief Function to register the callback to notify the mqtt connection status.
 * The connection/disconnection callback will be received even after the deinit of the
 * msg communicator until we de-register using the same function by passing the callback
 * as NULL.
 *
 * @param cb ccm_mqtt_conn_cb_t function pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_mqtt_register_conn_event(ccm_mqtt_conn_cb_t cb);

/**
 * @fn  cloud_connect_mqtt_reconnect(int conn_id)
 * @brief Function to reconnect a specific mqtt connection.
 *
 * @param conn_id int connection to be re-connected.
 * @param config ccm_sys_config_t structure pointer
 *
 */
void cloud_connect_mqtt_reconnect(int conn_id, ccm_sys_config_t *config);

/**
 * @fn cloud_connect_mqtt_deinit_msg_communicator(ccm_sys_config_t *config)
 * @brief Function to de-initialize the mqtt communicator module of the cloud connect mqtt microservice.
 *
 * @param config ccm_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_mqtt_deinit_msg_communicator(ccm_sys_config_t *config);

#endif /* CLOUD_CONNECT_MQTT_MSG_COMMUNICATOR_H_ */
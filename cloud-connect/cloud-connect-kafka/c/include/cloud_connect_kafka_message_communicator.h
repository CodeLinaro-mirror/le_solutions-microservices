/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_message_communicator.h

DESCRIPTION
    Header file for the message communicator module of Cloud Connect kafka service.
*/

#ifndef CLOUD_CONNECT_KAFKA_MSG_COMMUNICATOR_H_
#define CLOUD_CONNECT_KAFKA_MSG_COMMUNICATOR_H_

#include <stdint.h>
#include "cloud_connect_kafka_config.h"

/**
 * @struct cck_message
 * @brief Cloud connect kafka message structure
 */
typedef struct cck_message
{
    char *channel_src;     /*!< redis channel name */
    char *channel_dst;     /*!< kafka channel name */
    int msg_len;           /*!< length of message */
    uint8_t *msg;          /*!< pointer to message data */
    uint8_t connection_id; /*!< When there are multiple kafka publishers, the connection_id
                                indicates which publisher will handle this message. */
    uint64_t timestamp;    /*!< time of receving the message */
} cck_message_t;

/* callback for kafka connection status*/
typedef void (*cck_kafka_conn_cb_t)(int status, int conn_id);

/**
 * @fn cloud_connect_kafka_init_msg_communicator(cck_sys_config_t *config)
 * @brief Function to initialize the kafka communicator module of the cloud connect kafka microservice.
 *
 * @param config cck_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_init_msg_communicator(cck_sys_config_t *config);

/**
 * @fn cloud_connect_kafka_send_message(cck_message_t *config)
 * @brief Function to send message received from redis to kafka.
 *
 * @param msg cck_message_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_send_message(cck_message_t *msg);

/**
 * @fn cloud_connect_kafka_register_conn_event(cck_kafka_conn_cb_t cb)
 * @brief Function to register the callback to notify the kafka connection status.
 *
 * @param cb cck_kafka_conn_cb_t function pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_register_conn_event(cck_kafka_conn_cb_t cb);

/**
 * @fn  cloud_connect_kafka_reconnect(int conn_id)
 * @brief Function to reconnect a specific kafka connection.
 *
 * @param conn_id int connection to be re-connected.
 * @param config cck_sys_config_t structure pointer
 *
 */
void cloud_connect_kafka_reconnect(int conn_id, cck_sys_config_t *config);

/**
 * @fn cloud_connect_kafka_deinit_msg_communicator(cck_sys_config_t *config)
 * @brief Function to de-initialize the kafka communicator module of the cloud connect kafka microservice.
 *
 * @param config cck_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_deinit_msg_communicator(cck_sys_config_t *config);

#endif /* CLOUD_CONNECT_KAFKA_MSG_COMMUNICATOR_H_ */
/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    redis_mock.h

DESCRIPTION
    Header file for the test utils for mqtt microservice.
*/

#ifndef TEST_MQTT_UTILS_H_
#define TEST_MQTT_UTILS_H_

#include "redis_mock.h"
#include "mosquitto_mock.h"
// #include "mqtt_mock.h"
#include "../include/cloud_connect_mqtt_config.h"
#include "../include/cloud_connect_mqtt_status.h"
#include "../include/cloud_connect_mqtt_redis_communicator.h"
#include "../include/cloud_connect_mqtt_message_communicator.h"
#include "../include/cloud_connect_mqtt_logging.h"

/**
 * @enum Defines channel type.
 * @brief Enum representing different types of channel to subscribe mqtt client.
 */
typedef enum redis_mock_send_reply
{
    REPLY_TYPE_NONE = 0,
    REPLY_TYPE_PATTERN,
    REPLY_TYPE_CHANNEL,
    REPLY_TYPE_CHANNEL_UNMAPPED
} redis_mock_send_reply_t;

/**
 * @fn void ccm_sys_config_t *init_all_components(void)
 * @brief A test function to init all the interfaces of different modules required to run the test cases.
 *
 * @return config ccm_sys_config_t pointer holding the configuratiion.
 *
 */
ccm_sys_config_t *init_all_components(void);

/**
 * @fn void deinit_all_components(ccm_sys_config_t *config)
 * @brief A test function to deinit all the interfaces of different modules for gracefull exit.
 *
 * @param config ccm_sys_config_t pointer holding the configuratiion.
 *
 */
void deinit_all_components(ccm_sys_config_t *config);

/**
 * @fn void test_err_redis_communicator_init(redis_mock_err_type_t type)
 * @brief A function to test redis communicator init() error scenerios.
 *
 * @param type redis_mock_err_type_t type of error to be tested.
 *
 */
void test_err_redis_communicator_init(redis_mock_err_type_t type);

/**
 * @fn void reset_to_defaults(void)
 * @brief A helper function to reset all mock values to default.
 *
 */
void reset_to_defaults(void);

/**
 * @fn void test_err_redis_communicator_init(redis_mock_err_type_t type)
 * @brief A function to test redis communicator init() error scenerios.
 *
 * @param type redis_mock_err_type_t type of error to be tested.
 *
 */
void test_error_msg_communicator_init(mosquitto_mock_err_type err_type, ccm_sys_config_t *config);

/**
 * @fn void setReplyType(redis_mock_send_reply_t reply_type);
 * @brief A test function which facilitates different type of message's mocking.
 *
 * @param reply_type redisContext pointer holding the redis context.
 * @param tv timout for redis commands.
 *
 */
redisReply *setReplyType(redis_mock_send_reply_t reply_type);

/**
 * @fn void free_redis_reply(redisReply* reply)
 * @brief A test function which frres the mock redis reply created by UTs.
 *
 * @param reply redisReply pointer holding the redis reply object.
 *
 */
void free_redis_reply(redisReply *reply);

/**
 * @fn void send_connection_callback(ccm_mqtt_conn_cb_t *config)
 * @brief A test function which simulates the connection/disconnection.
 *
 * @param config ccm_sys_config_t pointer holding the configuratiion.
 * @param is_connect Boolean weather to mock connect callback or disconnect callback.
 *
 */
void send_connection_callback(ccm_sys_config_t *config, bool is_connect);

/**
 * @fn redisContext* setContext(void)
 * @brief A helper test function which allows UTs to set their redis context.
 *
 * @return redisContext a redisContext pointer holding the redis context object.
 *
 */
redisContext *setContext(void);

/**
 * @fn freeContext(redisContext *ctx)
 * @brief A helper test function which allows UTs to free their redis context.
 *
 * @param redisContext a redisContext pointer holding the redis context object.
 *
 */
void freeContext(redisContext *ctx);

/**
 * @fn enableConnectionError(redisContext *ctx, bool enable)
 * @brief A helper test function which allows UTs to enable/disable error in ongoing redis context.
 *
 * @param ctx a redisContext pointer holding the redis context object.
 * @param enable a boolean to indicate weather to enable or disable the error in context.
 *
 */
void enableConnectionError(redisContext *ctx, bool enable);

#endif
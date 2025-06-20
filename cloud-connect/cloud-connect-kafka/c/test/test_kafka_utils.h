/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    redis_mock.h

DESCRIPTION
    Header file for the test utils for kafka microservice.
*/

#ifndef TEST_KAFKA_UTILS_H_
#define TEST_KAFKA_UTILS_H_

#include "redis_mock.h"
#include "kafka_mock.h"
#include "../include/cloud_connect_kafka_config.h"
#include "../include/cloud_connect_kafka_status.h"
#include "../include/cloud_connect_kafka_redis_communicator.h"
#include "../include/cloud_connect_kafka_message_communicator.h"
#include "../include/cloud_connect_kafka_logging.h"

/**
 * @enum Defines channel type.
 * @brief Enum representing different types of channel to subscribe kafka client.
 */
typedef enum redis_mock_send_reply
{
    REPLY_TYPE_NONE = 0,
    REPLY_TYPE_PATTERN,
    REPLY_TYPE_CHANNEL,
    REPLY_TYPE_CHANNEL_UNMAPPED
} redis_mock_send_reply_t;

/**
 * @fn void cck_sys_config_t *init_all_components(void)
 * @brief A test function to init all the interfaces of different modules required to run the test cases.
 *
 * @return config cck_sys_config_t pointer holding the configuratiion.
 *
 */
cck_sys_config_t *init_all_components(void);

/**
 * @fn void deinit_all_components(cck_sys_config_t *config)
 * @brief A test function to deinit all the interfaces of different modules for gracefull exit.
 *
 * @param config cck_sys_config_t pointer holding the configuratiion.
 *
 */
void deinit_all_components(cck_sys_config_t *config);

/**
 * @fn void reset_to_defaults(void)
 * @brief A helper function to reset all mock values to default.
 *
 */
void reset_to_defaults(void);

/**
 * @fn test_error_msg_communicator_init(kafka_mock_err_type_t type, cck_sys_config_t *config)
 * @brief A function to test msg communicator error scenerios.
 *
 * @param type kafka_mock_err_type_t type of error to be tested.
 * @param config cck_sys_config_t pointer holding the configuratiion.
 *
 */
void test_error_msg_communicator_init(kafka_mock_err_type_t type, cck_sys_config_t *config);

/**
 * @fn void test_err_msg_communicator_send_message(kafka_mock_err_type_t type, cck_message_t *cck_msg)
 * @brief A function to test send_msg() error scenerios.
 *
 * @param type kafka_mock_err_type_t type of error to be tested.
 * @param cck_msg cck_message_t pointer holding test message.
 *
 */
void test_err_msg_communicator_send_message(kafka_mock_err_type_t type, cck_message_t *cck_msg);

/**
 * @fn void test_err_redis_communicator_init(redis_mock_err_type_t type)
 * @brief A function to test redis communicator init() error scenerios.
 *
 * @param type redis_mock_err_type_t type of error to be tested.
 *
 */
void test_err_redis_communicator_init(redis_mock_err_type_t type);

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
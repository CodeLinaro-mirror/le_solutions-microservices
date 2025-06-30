/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    redis_mock.c

DESCRIPTION
    Implementation file for utility functions which enables Unit Test to test different scnerios
    and mock different behaviors.
*/
extern "C"
{

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>

#include "test_kafka_utils.h"
#include "redis_mock.h"
#include "kafka_mock.h"

#include "../include/cloud_connect_kafka_logging.h"
#include "../include/cloud_connect_kafka_config.h"
#include "../include/cloud_connect_kafka_status.h"
#include "../include/cloud_connect_kafka_redis_communicator.h"
#include "../include/cloud_connect_kafka_message_communicator.h"
#include "../include/cloud_connect_utils.h"
}

#include "CppUTest/TestHarness.h"

#define RMOCK_CHANNEL_PATTERN "detection:ppe:1"
#define RMOCK_CHANNEL_NAME "detection:channel:1"
#define RMOCK_CHANNEL_UNMAPPED "unknown:channel"

#define RMOCK_PATTERN_ID "pmessage"
#define RMOCK_CHANNEL_ID "message"
#define RMOCK_REPLY_MSG "mock_data"
#define RMOCK_TEST_MSG "Test Message"
#define RMOCK_TEST_ERR_CODE "err-101"

#define SLEEP_TIME_SEC 1

cck_sys_config_t *init_all_components(void)
{
    /*Init config*/
    int status = cloud_connect_kafka_init_config();
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    /*Get config*/
    cck_sys_config_t *config = cloud_connect_kafka_get_config();
    CHECK_TEXT((config != NULL), "Config is NULL");

    /*Init redis communicator*/
    status = cloud_connect_kafka_init_redis_communicator(config);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    /*Init message communicator*/
    status = cloud_connect_kafka_init_msg_communicator(config);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    /* let all kafka connection be established */
    sleep(SLEEP_TIME_SEC);
    return config;
}

void deinit_all_components(cck_sys_config_t *config)
{
    int status = cloud_connect_kafka_deinit_redis_communicator(config);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    status = cloud_connect_kafka_deinit_msg_communicator(config);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    // Let everything be disconnected
    sleep(SLEEP_TIME_SEC);

    cloud_connect_kafka_deinit_config();
}

void reset_to_defaults(void)
{
    /* redis hooks */
    hookRedisEnableError(ERR_TYPE_REDIS_CONNECT_ERR, false);
    hookRedisEnableError(ERR_TYPE_REDIS_CONNECT_NULL, false);
    hookRedisEnableError(ERR_TYPE_REDIS_COMMAND_ERR, false);

    /* Kafka hooks */
    hook_rd_kafka_enable_error(ERR_TYPE_TOPIC, false);
    hook_rd_kafka_enable_error(ERR_TYPE_PRODUCE, false);
    hook_rd_kafka_enable_error(ERR_TYPE_CONF, false);
    hook_rd_kafka_enable_error(ERR_TYPE_CONF_SET, false);
    hook_rd_kafka_enable_error(ERR_TYPE_HANDLE, false);
    hook_rd_kafka_enable_error(ERR_TYPE_HANDLE_CB, false);

    /* set kafka msg count to 0 */
    hook_rd_kafka_set_msg_count(0);

    /* set redis publish count to 0*/
    hookRedisSetPublishCount(0);
}

void test_error_msg_communicator_init(kafka_mock_err_type_t type, cck_sys_config_t *config)
{
    /* Before init enable mock error */
    hook_rd_kafka_enable_error(type, true);

    /*Init msg communicator*/
    int status = cloud_connect_kafka_init_msg_communicator(config);
    CHECK_EQUAL(CCK_STATUS_FAIL, status);

    /* Before init disable mock error */
    hook_rd_kafka_enable_error(type, false);
}

void test_err_msg_communicator_send_message(kafka_mock_err_type_t type, cck_message_t *cck_msg)
{
    /* Before sending msg enable topic creation error */
    hook_rd_kafka_enable_error(type, true);

    /* send message which should fail */
    int status = cloud_connect_kafka_send_message(cck_msg);
    CHECK_EQUAL(CCK_STATUS_FAIL, status);

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    /* Now disable topic creation error */
    hook_rd_kafka_enable_error(type, false);
}

void test_err_redis_communicator_init(redis_mock_err_type_t type)
{
    redisContext *ctx = setContext();
    /* set kafka msg count to 0*/
    hook_rd_kafka_set_msg_count(0);

    /* Enable NULL context error */
    hookRedisEnableError(type, true);

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    setReplyType(REPLY_TYPE_NONE);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    /* Disable NULL context error */
    hookRedisEnableError(type, false);

    free_redis_reply(r);
    freeContext(ctx);
}

redisContext *setContext(void)
{
    redisContext *c = (redisContext *)malloc(sizeof(redisContext));
    c->err = NULL;
    hookRedisSetContext(c);
    return c;
}

void enableConnectionError(redisContext *ctx, bool enable)
{
    if (enable)
    {
        strlcpy(ctx->errstr, RMOCK_TEST_ERR_CODE, REDIS_DATA_LEN);
        ctx->err = ctx->errstr;
    }
    else
    {
        memset(ctx->errstr, 0, REDIS_DATA_LEN);
        ctx->err = NULL;
    }
}

void freeContext(redisContext *ctx)
{
    if (ctx != NULL)
    {
        free(ctx);
        ctx = NULL;
    }
}

/**
 * @fn void setReplyType(redis_mock_send_reply_t reply_type);
 * @brief A test function which facilitates different type of message's mocking.
 *
 * @param reply_type redisContext pointer holding the redis context.
 * @param tv timout for redis commands.
 *
 */
redisReply *setReplyType(redis_mock_send_reply_t reply_type)
{
    // Allocate memory for the redisReply struct
    redisReply *r = NULL;

    if (reply_type == REPLY_TYPE_NONE)
    {
        hookRedisSetReply(r);
        return r;
    }

    r = (redisReply *)malloc(sizeof(redisReply));
    if (r == NULL)
    {
        LOGE("Memory allocation failed\n");
        return r;
    }

    // Set the type to REDIS_REPLY_ARRAY
    r->type = REDIS_REPLY_ARRAY;

    // Allocate memory for the elements in the array
    r->elements = 3; // For channel 3 elements array
    if (reply_type == REPLY_TYPE_PATTERN)
    {
        r->elements++; // For pattern 4 elements array
    }
    r->element = (redisReply **)malloc(r->elements * sizeof(redisReply *));
    if (r->element == NULL)
    {
        LOGE("Memory allocation failed\n");
        free(r);
        return NULL;
    }

    int count = 0;
    // Create and set the first element
    r->element[count] = (redisReply *)malloc(sizeof(redisReply));
    r->element[count]->type = REDIS_REPLY_STRING;
    if (reply_type == REPLY_TYPE_PATTERN)
    {
        r->element[count]->str = strdup(RMOCK_PATTERN_ID);
    }
    else
    {
        r->element[count]->str = strdup(RMOCK_CHANNEL_ID);
    }
    r->element[count]->len = strlen(r->element[0]->str);

    if (reply_type == REPLY_TYPE_PATTERN)
    {
        count++;
        // Create and set the second element
        r->element[count] = (redisReply *)malloc(sizeof(redisReply));
        r->element[count]->type = REDIS_REPLY_STRING;
        r->element[count]->str = strdup(RMOCK_REPLY_MSG);
        r->element[count]->len = strlen(r->element[1]->str);
    }

    count++;
    // Create and set the third element
    LOGD("Replay type : %d", reply_type);
    r->element[count] = (redisReply *)malloc(sizeof(redisReply));
    r->element[count]->type = REDIS_REPLY_STRING;
    if (reply_type == REPLY_TYPE_PATTERN)
    {
        r->element[count]->str = strdup(RMOCK_CHANNEL_PATTERN);
    }
    else if (reply_type == REPLY_TYPE_CHANNEL)
    {
        r->element[count]->str = strdup(RMOCK_CHANNEL_NAME);
    }
    else
    {
        r->element[count]->str = strdup(RMOCK_CHANNEL_UNMAPPED);
    }
    r->element[count]->len = strlen(r->element[count]->str);

    count++;
    // Create and set the fourth element
    r->element[count] = (redisReply *)malloc(sizeof(redisReply));
    r->element[count]->type = REDIS_REPLY_STRING;
    r->element[count]->str = strdup(RMOCK_TEST_MSG);
    r->element[count]->len = strlen(r->element[count]->str);

    // Set the reply pointer to the created redisReply
    hookRedisSetReply(r);

    return r;
}

void free_redis_reply(redisReply *reply)
{
    if (reply == NULL)
        return;

    if (reply->type == REDIS_REPLY_ARRAY)
    {
        for (size_t i = 0; i < reply->elements; i++)
        {
            free(reply->element[i]->str);
            free(reply->element[i]);
        }
        free(reply->element);
    }
    free(reply);
}

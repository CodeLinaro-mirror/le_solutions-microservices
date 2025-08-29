/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- test_message_flow.cpp
 * Description :- Test case file used for testing message delivery using kafka
 */
extern "C"
{

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "redis_mock.h"
#include "kafka_mock.h"
#include "test_kafka_utils.h"

    // Include the mock header

#include "../include/cloud_connect_kafka_config.h"
#include "../include/cloud_connect_kafka_status.h"
#include "../include/cloud_connect_kafka_redis_communicator.h"
#include "../include/cloud_connect_kafka_message_communicator.h"
#include "../include/cloud_connect_kafka_logging.h"
}

#include "CppUTest/TestHarness.h"
#define TEST_MESSAGE "My message"

#define TEST_PATTERN_REDIS "detection:ppe:*"
#define TEST_PATTERN_KAFKA "detection.ppe.kafka.*"

#define TEST_CHANNEL_REDIS "detection:ppe:1"
#define TEST_CHANNEL_KAFKA "detection.ppe.kafka.1"

#define TEST_CHANNEL_REDIS_CHANNEL "detection:channel:1"
#define TEST_CHANNEL_KAFKA_CHANNEL "detection.channel.kafka.1"

#define TEST_MAX_READ_ATTEMPT 5

#define SLEEP_TIME_SEC 1
#define SLEEP_TIME_RECONN_SEC 2

static volatile sig_atomic_t run = 1;

static void stop(int sig)
{
    run = 0;
}

TEST_GROUP(MessageFlowGroup)
{
    void setup()
    {
        // Setup code if needed
        set_log_level(LOG_ERROR);
        reset_to_defaults();
    }

    void teardown()
    {
        // Teardown code if needed
    }
}
;

/* A test where we mock error while sending message and will test redis publish back */
TEST(MessageFlowGroup, TestRedisPublishBack)
{
    LOGE("TestRedisPublishBack=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable error in send msg flow to kafka */
    hook_rd_kafka_enable_error(ERR_TYPE_NONE, true);
    hook_rd_kafka_enable_error(ERR_TYPE_PRODUCE, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    setReplyType(REPLY_TYPE_NONE);

    /* msg reach to kafka should be zero */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    /* Now msgs published to redis should be greater than 0 */
    CHECK(hookRedisGetPublishCount() > 0);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where we mock broker down and  broker up scenerio */
TEST(MessageFlowGroup, TestKafkaConnectDisconnect)
{
    LOGE("TestKafkaConnectDisconnect=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* mock msg send error which will leads to connection failure and reconnection */
    hook_rd_kafka_enable_error(ERR_TYPE_PRODUCE, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    hook_rd_kafka_enable_error(ERR_TYPE_PRODUCE, false);

    sleep(SLEEP_TIME_RECONN_SEC);

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where we will mock all error scenerios of redis communicator init() */
TEST(MessageFlowGroup, TestErrorInitRedisComm)
{
    LOGE("TestErrorInitRedisComm=====");
    /* Test redis contect NULL case */
    test_err_redis_communicator_init(ERR_TYPE_REDIS_CONNECT_NULL);

    /* Test redis contect error case */
    test_err_redis_communicator_init(ERR_TYPE_REDIS_CONNECT_ERR);

    /* Test redis subscribe error case */
    test_err_redis_communicator_init(ERR_TYPE_REDIS_COMMAND_ERR);
}

/* A test where we will mock all error scenerios of msg communicator init() */
TEST(MessageFlowGroup, TestErrorInitMsgComm)
{
    LOGE("TestErrorInitMsgComm=====");
    redisContext *ctx = setContext();
    /*Init config*/
    int status = cloud_connect_kafka_init_config();
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    /* Set log level to error */
    status = set_log_level(LOG_ERROR);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    /*Get config*/
    cck_sys_config_t *config = cloud_connect_kafka_get_config();
    CHECK_TEXT((config != NULL), "Config is NULL");

    /* Test error conf create */
    test_error_msg_communicator_init(ERR_TYPE_CONF, config);

    /* Test error conf set */
    test_error_msg_communicator_init(ERR_TYPE_CONF_SET, config);

    /* Test error kafka handle */
    test_error_msg_communicator_init(ERR_TYPE_HANDLE, config);

    /* Deinit config */
    cloud_connect_kafka_deinit_config();

    /* Set log level to debug */
    status = set_log_level(LOG_DEBUG);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);
    freeContext(ctx);
}

/* A test where we will mock all error scenerios of send_message() interface of message communicator module*/
TEST(MessageFlowGroup, TestErrorSendMsgComm)
{
    LOGE("TestErrorSendMsgComm=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    char channel_src[] = TEST_CHANNEL_REDIS;
    char channel_dst[] = TEST_CHANNEL_KAFKA_CHANNEL;

    cck_message_t cck_msg;
    cck_msg.channel_src = (char *)channel_src;
    cck_msg.msg_len = strlen(TEST_MESSAGE);
    cck_msg.connection_id = 0;
    cck_msg.msg = (uint8_t *)TEST_MESSAGE;
    cck_msg.channel_dst = (char *)channel_dst;

    /* test topic error */
    test_err_msg_communicator_send_message(ERR_TYPE_TOPIC, &cck_msg);

    /* test produce error*/
    test_err_msg_communicator_send_message(ERR_TYPE_PRODUCE, &cck_msg);

    /*Deinit all interfaces*/
    deinit_all_components(config);
    freeContext(ctx);
}

/* A test where we dont send msg via redis instead will use send_message() interface of message communicator module*/
TEST(MessageFlowGroup, TestWithoutRedisKafkaFlow)
{
    LOGE("TestWithoutRedisKafkaFlow=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    char channel_src[] = TEST_CHANNEL_REDIS;
    char channel_dst[] = TEST_CHANNEL_KAFKA_CHANNEL;

    cck_message_t cck_msg;
    cck_msg.channel_src = (char *)channel_src;
    cck_msg.msg_len = strlen(TEST_MESSAGE);
    cck_msg.connection_id = 0;
    cck_msg.msg = (uint8_t *)TEST_MESSAGE;
    cck_msg.channel_dst = (char *)channel_dst;

    /* send message here */
    int status = cloud_connect_kafka_send_message(&cck_msg);
    CHECK_EQUAL(CCK_STATUS_SUCCESS, status);

    int count = 0;
    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    /*Deinit all interfaces*/
    deinit_all_components(config);
    freeContext(ctx);
}

/* A test where send a msg over an unmapped channel of redis */
TEST(MessageFlowGroup, TestRedisToKafkaFlowUnmapped)
{
    LOGE("TestRedisToKafkaFlowUnmapped=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL_UNMAPPED);
    int count = 0;

    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    setReplyType(REPLY_TYPE_NONE);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where send a msg over a redis channel which is part of pattern in config.json */
TEST(MessageFlowGroup, TestRedisToKafkaFlowPattern)
{
    LOGE("TestRedisToKafkaFlowPattern=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where send a msg over a redis channel which is part of channel in config.json */
TEST(MessageFlowGroup, TestRedisToKafkaFlowChannel)
{
    LOGE("TestRedisToKafkaFlowChannel=====");
    redisContext *ctx = setContext();
    /* set kafka msg count to 0*/
    hook_rd_kafka_set_msg_count(0);

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);
    int count = 0;

    /* Wait for max 5 seconds to reach message to kafka broker */
    while (hook_rd_kafka_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where we will test redis connection and reconnection */
TEST(MessageFlowGroup, TestRedisReConnection)
{
    LOGE("TestRedisReConnection=====");
    redisContext *ctx = setContext();

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);

    /* enable redis connection error */
    enableConnectionError(ctx, true);

    sleep(SLEEP_TIME_SEC);

    /* Reset msg count*/
    hook_rd_kafka_set_msg_count(0);

    sleep(SLEEP_TIME_SEC);

    /* msg sould not reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    /* disable redis connection error */
    enableConnectionError(ctx, false);

    sleep(SLEEP_TIME_RECONN_SEC);

    /* msg sould reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    sleep(SLEEP_TIME_SEC);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test which mocks redis down in the begning */
TEST(MessageFlowGroup, TestRedisDownAtBegning)
{
    LOGE("TestRedisDownAtBegning=====");
    redisContext *ctx = setContext();

    /* enable redis connection error at begning */
    enableConnectionError(ctx, true);

    /* Init all components */
    cck_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);

    sleep(SLEEP_TIME_SEC);

    /* Reset msg count*/
    hook_rd_kafka_set_msg_count(0);

    sleep(SLEEP_TIME_SEC);

    /* msg sould not reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() == 0);

    /* disable redis connection error */
    enableConnectionError(ctx, false);

    sleep(SLEEP_TIME_RECONN_SEC);

    /* msg sould reach to kafka */
    CHECK(hook_rd_kafka_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    sleep(SLEEP_TIME_SEC);

    free_redis_reply(r);
    freeContext(ctx);
}

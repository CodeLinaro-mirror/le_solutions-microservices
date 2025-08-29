/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- test_message_flow.cpp
 * Description :- Test case file used for unit test demo
 */
extern "C"
{

#include <unistd.h>

#include "../include/cloud_connect_mqtt_config.h"
#include "../include/cloud_connect_mqtt_status.h"
#include "../include/cloud_connect_mqtt_redis_communicator.h"
#include "../include/cloud_connect_mqtt_message_communicator.h"
#include "../include/cloud_connect_mqtt_logging.h"
#include "redis_mock.h"
#include "mosquitto_mock.h"
#include "test_mqtt_utils.h"
}

#include "CppUTest/TestHarness.h"
#define TEST_MESSAGE "My message"
#define TEST_CHANNEL_REDIS "detection:ppe:1"
#define TEST_CHANNEL_MQTT "detection/ppe/mqtt/1"

#define TEST_MAX_READ_ATTEMPT 5

#define SLEEP_TIME_SEC 1
#define SLEEP_TIME_RECONN_SEC 2

static int g_err_mock = MOSQ_ERR_TYPE_NONE;

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
    LOGE("TestRedisPublishBack....");
    redisContext *ctx = setContext();
    set_log_level(LOG_DEBUG);

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Mock connection success callback so that msg sending path is enabled */
    send_connection_callback(config, true);

    /* Enable error in send msg flow to mqtt */
    hook_mosquitto_enable_error(MOSQ_ERR_TYPE_SEND_MSG, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    sleep(SLEEP_TIME_SEC);

    /* msg reach to mqtt should be zero */
    CHECK(hook_mosquitto_get_msg_count() == 0);

    /* Now msgs published to redis should be greater than 0 */
    CHECK(hookRedisGetPublishCount() > 0);

    setReplyType(REPLY_TYPE_NONE);

    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where we mock broker down and  broker up scenerio */
TEST(MessageFlowGroup, TestMqttConnectDisconnect)
{
    set_log_level(LOG_INFO);
    LOGE("Starting TestMqttConnectDisconnect..");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    hook_mosquitto_enable_error(MOSQ_ERR_TYPE_NONE, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Mock connection failure callback */
    send_connection_callback(config, false);

    sleep(SLEEP_TIME_SEC);

    hook_mosquitto_set_msg_count(0);

    /* No msg should be received*/
    CHECK(hook_mosquitto_get_msg_count() == 0);

    /* Mock connection success callback */
    send_connection_callback(config, true);

    sleep(SLEEP_TIME_SEC);

    setReplyType(REPLY_TYPE_NONE);

    sleep(SLEEP_TIME_SEC);

    /* Now msgs should be received */
    CHECK(hook_mosquitto_get_msg_count() > 0);

    sleep(SLEEP_TIME_SEC);

    /* Mock connection success callback */
    send_connection_callback(config, false);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where we will mock all error scenerios of msg communicator init() */
TEST(MessageFlowGroup, TestErrorInitMsgComm)
{
    LOGE("TestErrorInitMsgComm....");
    /*Init config*/
    int status = cloud_connect_mqtt_init_config();
    CHECK_EQUAL(CCM_STATUS_SUCCESS, status);

    /* Set log level to error */
    status = set_log_level(LOG_ERROR);
    CHECK_EQUAL(CCM_STATUS_SUCCESS, status);

    /*Get config*/
    ccm_sys_config_t *config = cloud_connect_mqtt_get_config();
    CHECK_TEXT((config != NULL), "Config is NULL");

    /* Test client new */
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_CLIENT_NEW, config);

    /*Test tls set*/
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_TLS_SET, config);

    /*Test tls opts set*/
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_TLS_OPTS_SET, config);

    /*Test tls insecure set*/
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_TLS_INSECURE_SET, config);

    /* Test dialer connect */
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_CLIENT_CONNECT, config);

    /* Test dialer start */
    test_error_msg_communicator_init(MOSQ_ERR_TYPE_CLIENT_START, config);

    /* Deinit config */
    cloud_connect_mqtt_deinit_config();

    /* Set log level to debug */
    status = set_log_level(LOG_WARNING);
    CHECK_EQUAL(CCM_STATUS_SUCCESS, status);
}

/* A test where we will mock error scenerios of send_message() interface of message communicator module*/
TEST(MessageFlowGroup, TestErrorSendMsgComm)
{
    LOGE("TestErrorSendMsgComm....");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Enable send msg error */
    hook_mosquitto_enable_error(MOSQ_ERR_TYPE_SEND_MSG, true);

    char channel_src[] = TEST_CHANNEL_REDIS;
    char channel_dst[] = TEST_CHANNEL_MQTT;

    ccm_message_t ccm_msg;
    ccm_msg.channel_src = (char *)channel_src;
    ccm_msg.msg_len_bytes = strlen(TEST_MESSAGE);
    ccm_msg.connection_id = 0;
    ccm_msg.msg = (uint8_t *)TEST_MESSAGE;
    ccm_msg.channel_dst = (char *)channel_dst;

    /* send message here */
    int status = cloud_connect_mqtt_send_message(&ccm_msg);
    CHECK_EQUAL(CCM_CONN_STATUS_ERROR, status);

    /* check if msg could reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() == 0);

    /*Deinit all interfaces*/
    deinit_all_components(config);
    freeContext(ctx);
}

/* A test where we dont send msg via redis instead will use send_message() interface of message communicator module*/
TEST(MessageFlowGroup, TestWithoutRedisMqttFlow)
{
    LOGE("TestWithoutRedisMqttFlow....");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    char channel_src[] = TEST_CHANNEL_REDIS;
    char channel_dst[] = TEST_CHANNEL_MQTT;

    ccm_message_t ccm_msg;
    ccm_msg.channel_src = (char *)channel_src;
    ccm_msg.msg_len_bytes = strlen(TEST_MESSAGE);
    ccm_msg.connection_id = 0;
    ccm_msg.msg = (uint8_t *)TEST_MESSAGE;
    ccm_msg.channel_dst = (char *)channel_dst;

    /* send message here */
    int status = cloud_connect_mqtt_send_message(&ccm_msg);
    CHECK_EQUAL(CCM_STATUS_SUCCESS, status);

    int count = 0;
    /* Wait for max 5 seconds to reach message to mqtt broker */
    while (hook_mosquitto_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() > 0);

    /*Deinit all interfaces*/
    deinit_all_components(config);
    freeContext(ctx);
}

/* A test where send a msg over an unmapped channel of redis */
TEST(MessageFlowGroup, TestRedisToMqttFlowUnmapped)
{
    LOGE("TestRedisToMqttFlowUnmapped....");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL_UNMAPPED);
    int count = 0;

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Wait for max 5 seconds to reach message to mqtt broker */
    while (hook_mosquitto_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() == 0);

    setReplyType(REPLY_TYPE_NONE);
    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where send a msg over a redis channel which is part of pattern in config.json */
TEST(MessageFlowGroup, TestRedisToMqttFlowPattern)
{
    LOGE("TestRedisToMqttFlowPattern....");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_PATTERN);
    int count = 0;

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Wait for max 5 seconds to reach message to mqtt broker */
    while (hook_mosquitto_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);
    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test where send a msg over a redis channel which is part of channel in config.json */
TEST(MessageFlowGroup, TestRedisToMqttFlowChannel)
{
    LOGE("TestRedisToMqttFlowChannel....");
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);
    int count = 0;

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Wait for max 5 seconds to reach message to mqtt broker */
    while (hook_mosquitto_get_msg_count() == 0)
    {
        if (count >= TEST_MAX_READ_ATTEMPT)
        {
            break;
        }
        sleep(SLEEP_TIME_SEC);
        count++;
    }

    /* check if msg could reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);
    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    /*Let redis communicator be deallocated..*/
    sleep(SLEEP_TIME_SEC);

    free_redis_reply(r);
    freeContext(ctx);
}

/* A test which mocks redis disconnection and re-connection */
TEST(MessageFlowGroup, TestRedisReConnection)
{
    LOGE("TestRedisReConnection=====");
    set_log_level(LOG_DEBUG);
    redisContext *ctx = setContext();

    /* Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);

    /* enable redis connection error */
    enableConnectionError(ctx, true);

    sleep(SLEEP_TIME_SEC);

    /* Reset msg count*/
    hook_mosquitto_set_msg_count(0);

    sleep(SLEEP_TIME_SEC);

    /* msg sould not reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() == 0);

    /* disable redis connection error */
    enableConnectionError(ctx, false);

    sleep(SLEEP_TIME_RECONN_SEC);

    /* msg sould reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() > 0);

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
    set_log_level(LOG_DEBUG);
    redisContext *ctx = setContext();

    /* enable redis connection at begning */
    enableConnectionError(ctx, true);

    /* Now Init all components */
    ccm_sys_config_t *config = init_all_components();

    /* Mock connection success callback */
    send_connection_callback(config, true);

    /* Enable msg over redis */
    redisReply *r = setReplyType(REPLY_TYPE_CHANNEL);

    sleep(SLEEP_TIME_SEC);

    /* Reset msg count*/
    hook_mosquitto_set_msg_count(0);

    sleep(SLEEP_TIME_SEC);

    /* msg sould not reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() == 0);

    /* disable redis connection error */
    enableConnectionError(ctx, false);

    sleep(SLEEP_TIME_RECONN_SEC);

    /* msg sould reach to mqtt */
    CHECK(hook_mosquitto_get_msg_count() > 0);

    setReplyType(REPLY_TYPE_NONE);

    sleep(SLEEP_TIME_SEC);

    /*Deinit all interfaces*/
    deinit_all_components(config);

    sleep(SLEEP_TIME_SEC);

    free_redis_reply(r);
    freeContext(ctx);
}

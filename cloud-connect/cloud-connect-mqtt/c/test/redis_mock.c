/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    redis_mock.c

DESCRIPTION
    Implementation file for the redis mock to mock the redis client interfaces. It is currently mocking all the interfaces of redis. By default all functions related to connection and commnds
    are returning success. To mock the error scenerios for them Unit Tests can use function void hookRedisEnableError(redis_mock_err_type_t option, bool enable). The redisGetReply() function by default
    returns a NULL reply. A helper function setReplyType() is provided for Unit Tests to mock different kind of message from redisGetReply().
    Note: Since microservice is using redis in synchronous mode so mock is currently written considering it only which implies that subscribe and read operation suppose to happen in same thread.
*/

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include "redis_mock.h"

#include "../include/cloud_connect_mqtt_logging.h"

#define RMOCK_CHANNEL_PATTERN "detection:ppe:1"
#define RMOCK_CHANNEL_NAME "detection:channel:1"
#define RMOCK_CHANNEL_UNMAPPED "unknown:channel"

#define RMOCK_PATTERN_ID "pmessage"
#define RMOCK_CHANNEL_ID "message"
#define RMOCK_REPLY_MSG "mock_data"
#define RMOCK_TEST_MSG "Test Message"
#define RMOCK_TEST_ERR_CODE "err-101"

redisReply *g_mock_redis_reply = NULL;
redisReply g_mock_redis_cmd_reply;
redisContext *g_mock_redis_context = NULL;

/* To maintain the redis publish count for verification of error handling test cases*/
static int g_redis_publish_count = 0;

static redis_mock_err_type_t g_err_redis_mock = ERR_TYPE_REDIS_NONE;

void hookRedisSetContext(redisContext *ctx)
{
    g_mock_redis_context = ctx;
}

void hookRedisSetPublishCount(int count)
{
    g_redis_publish_count = 0;
}

int hookRedisGetPublishCount(void)
{
    return g_redis_publish_count;
}

void hookRedisSetReply(redisReply *redis_reply)
{
    g_mock_redis_reply = redis_reply;
}

// Mock function for redisConnect
redisContext *redisConnect(const char *ip, int port)
{
    if (g_err_redis_mock & ERR_TYPE_REDIS_CONNECT_NULL)
    {
        return NULL;
    }

    return g_mock_redis_context;
}

// Mock function for redisFree
void redisFree(redisContext *c)
{
}

// Mock function for redisCommand
void *redisCommand(redisContext *c, const char *format, ...)
{
    if (g_err_redis_mock & ERR_TYPE_REDIS_COMMAND_ERR)
    {
        return NULL;
    }
    va_list args;
    va_start(args, format);
    if (strcmp(format, "PUBLISH %s %s") == 0)
    {
        g_redis_publish_count++;
    }
    va_end(args);
    redisReply *r = &g_mock_redis_cmd_reply;
    return r;
}

// Mock function for redisGetReply

int redisGetReply(redisContext *c, void **reply)
{
    // LOGD("Getting reply");
    redisReply *r = NULL;
    r = g_mock_redis_reply;
    *reply = r;
    return REDIS_OK;
}

// Mock function for freeReplyObject
// Function to free the allocated memory for the redisReply
void freeReplyObject(redisReply *reply)
{
    /* The responsbility to free redis reply is with UT */
}

// Mock function for redisSetTimeout
int redisSetTimeout(redisContext *c, const struct timeval tv)
{
    if (c)
    {
        c->timeout = tv;
        LOGD("Timeout set to %ld seconds and %ld microseconds\n", tv.tv_sec, tv.tv_usec);
        return 0;
    }
    return -1;
}

void hookRedisEnableError(redis_mock_err_type_t option, bool enable)
{
    if (enable)
    {
        g_err_redis_mock |= option;
    }
    else
    {
        g_err_redis_mock &= ~option;
    }
}

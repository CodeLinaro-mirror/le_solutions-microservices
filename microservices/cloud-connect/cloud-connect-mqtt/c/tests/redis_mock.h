/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    redis_mock.h

DESCRIPTION
    Header file for the redis mock to mock the redis client interfaces.
*/

#ifndef TEST_REDIS_MOCK_H_
#define TEST_REDIS_MOCK_H_

#include <sys/time.h>
#include <stdbool.h>

#define REDIS_OK 0
#define REDIS_ERR -1
#define REDIS_REPLY_ARRAY 0
#define REDIS_REPLY_STRING 1
#define REDIS_IP_LEN 50
#define REDIS_CONTEXT_CHAR_LEN 256
#define REDIS_DATA_LEN 10
#define REDIS_VTYPE 4

/**
 * @enum Defines mock error type.
 * @brief Enum representing different types of channel to subscribe mqtt client.
 */
typedef enum redis_mock_err_type
{
    ERR_TYPE_REDIS_NONE = 0,
    ERR_TYPE_REDIS_CONNECT_NULL = 1 << 0,
    ERR_TYPE_REDIS_CONNECT_ERR = 1 << 1,
    ERR_TYPE_REDIS_COMMAND_ERR = 1 << 2
} redis_mock_err_type_t;

// Mock redisContext structure
typedef struct redisContext
{
    char ip[REDIS_IP_LEN];
    int port;
    struct
    {
        char key[REDIS_CONTEXT_CHAR_LEN];
        char value[REDIS_CONTEXT_CHAR_LEN];
    } data[REDIS_DATA_LEN];
    int data_count;
    char subscriptions[REDIS_DATA_LEN][REDIS_CONTEXT_CHAR_LEN];
    int sub_count;
    char errstr[REDIS_DATA_LEN];
    char *err;
    struct timeval timeout;
} redisContext;

// typedef struct redisContext redisContext;

// Mock redisReply structure
typedef struct redisReply
{
    int type;                    /* REDIS_REPLY_* */
    long long integer;           /* The integer when type is REDIS_REPLY_INTEGER */
    double dval;                 /* The double when type is REDIS_REPLY_DOUBLE */
    int len;                     /* Length of string */
    char *str;                   /* Used for REDIS_REPLY_ERROR, REDIS_REPLY_STRING
                                    REDIS_REPLY_VERB, REDIS_REPLY_DOUBLE (in additional to dval),
                                    and REDIS_REPLY_BIGNUM. */
    char vtype[REDIS_VTYPE];     /* Used for REDIS_REPLY_VERB, contains the null
                          terminated 3 character content type, such as "txt". */
    int elements;                /* number of elements, for REDIS_REPLY_ARRAY */
    struct redisReply **element; /* elements vector for REDIS_REPLY_ARRAY */
} redisReply;

/**
 * @fn redisContext* redisConnect(const char *ip, int port);
 * @brief A test mock function for hiredis redisConnect().
 *
 * @param ip const char pointer holding the ip of mock redis.
 * @param port int port of mock redis.
 * @return redisContext pointer holding the redis context.
 *
 */
redisContext *redisConnect(const char *ip, int port);

/**
 * @fn void redisFree(redisContext *c);
 * @brief A test mock function for hiredis redisFree().
 *
 * @param c redisContext pointer holding the redis context.
 *
 */
void redisFree(redisContext *c);

/**
 * @fn void* redisCommand(redisContext *c, const char *format, ...);
 * @brief A test mock function for hiredis redisCommand().
 *
 * @param c redisContext pointer holding the redis context.
 * @param format const char * a pointer holding the formatted redis command.
 *
 */
void *redisCommand(redisContext *c, const char *format, ...);

/**
 * @fn int redisGetReply(redisContext *c, void **reply);
 * @brief A test mock function for hiredis redisGetReply().
 *
 * @param c redisContext pointer holding the redis context.
 * @param reply void pointer holding the data read from topic.
 *
 */
int redisGetReply(redisContext *c, void **reply);

/**
 * @fn void freeReplyObject(redisReply *reply);
 * @brief A test mock function for hiredis freeReplyObject().
 *
 * @param reply void pointer holding the data read from topic.
 *
 */
void freeReplyObject(redisReply *reply);

/**
 * @fn int redisSetTimeout(redisContext *c, const struct timeval tv);
 * @brief A test mock function for hiredis redisSetTimeout().
 *
 * @param c redisContext pointer holding the redis context.
 * @param tv timout for redis commands.
 *
 */
int redisSetTimeout(redisContext *c, const struct timeval tv);

/**
 * @brief Enables the error mock for different functions
 *
 * @param option It will indicate for which function error should be mocked.
 * @param enable a bool to enable or disable the error mock
 */
void hookRedisEnableError(redis_mock_err_type_t option, bool enable);

/**
 * @brief Enables the Unit Tests to mock the differnt kind of redis messages
 *
 * @param redis_reply redisReply pointer which holds the mock redis message.
 *
 */
void hookRedisSetReply(redisReply *redis_reply);

/**
 * @brief Function to set/reset the redis msg publish count
 *
 * @param count number to be set.
 *
 */
void hookRedisSetPublishCount(int count);

/**
 * @brief A getter function to get the redis msg publish count
 *
 * @return int number of published messages.
 *
 */
int hookRedisGetPublishCount(void);

/**
 * @brief A hook provided to UTs for setting the redis context
 *
 * @param ctx a redisContext pointer holding the redis context object.
 *
 */
void hookRedisSetContext(redisContext *ctx);

#endif
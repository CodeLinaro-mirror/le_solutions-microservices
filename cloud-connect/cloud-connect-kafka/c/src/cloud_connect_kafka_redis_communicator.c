/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_redis_communicator.c

DESCRIPTION
    Implementation file for the redis communicator module of Cloud Connect kafka service.
*/
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#ifdef UNIT_TEST
#include "../test/redis_mock.h"
#else
#include <hiredis/hiredis.h>
#endif
#include <time.h>

#include "cloud_connect_kafka_redis_communicator.h"
#include "cloud_connect_kafka_status.h"
#include "cloud_connect_kafka_config.h"
#include "cloud_connect_kafka_logging.h"
#include "cloud_connect_kafka_message_communicator.h"
#include "cloud_connect_utils.h"

#define MAX_MSG_FAIL_COUNT 10
#define SLEEP_DURATION_ON_MSG_SEND_FAIL 2
#define SLEEP_DURATION_ON_REDIS_CONN_FAIL 1
#define REDIS_MAX_CONSUMER 10
#define REDIS_READ_TIMEOUT 5
/* Log interval for Kafka client connection status in seconds. */
#define KAFKA_CONNECTION_STATUS_LOG_INTERVAL_SECONDS 10
/* Size of temporary buffer to create channel name for kafka clients. */
#define TEMP_DST_CHANNEL_BUFFER_SIZE_BYTES 256
/* Max length of kafka clients channel name. */
#define KAFKA_CHANNEL_MAX_LEN_BYTES 256
/**
 * When we subscribe on a wild card topic vs when we subscribe over a non-wildcard topic the
 * indices for data and channel names and total response length are different.
 */
#define PATTERN_RESPONSE_LEN 4
#define PATTERN_CHANNEL_INDEX 2
#define PATTERN_MESSAGE_INDEX 3

#define NON_PATTERN_RESPONSE_LEN 3
#define NON_PATTERN_CHANNEL_INDEX 1
#define NON_PATTERN_MESSAGE_INDEX 2

#define DSTSTRING_LEN 256

// In the redis reply message, first index represents type of message.
#define REDIS_MESSAGE_TYPE_INDEX 0
// message type is "message" if it was subscribed with SUBSCRIBE command.
#define MESSAGE_TYPE_WITH_SUBSCRIBE_COMMAND "message"
// message type is "pmessage" if it was subscribed with PSUBSCRIBE command.
#define MESSAGE_TYPE_WITH_PSUBSCRIBE_COMMAND "pmessage"

static int g_kafka_connection_status[REDIS_MAX_CONSUMER];

pthread_t g_threads[REDIS_MAX_CONSUMER];
int g_thread_index[REDIS_MAX_CONSUMER];
bool g_init_redis_communicator_done = false;

/**
 * @brief It creates topic for kafka publisher.
 *
 * @param msg message structure which should be filled with the created topic name for kafka publisher.
 * @param channel_map It has all the configured channel names in form of array of source chnnel and destination channel.
 * @param type Type of source channel topic (pattern or normal topic).
 */
static void prepare_channel_map(cck_message_t *msg, cck_channel_mapping_t *channel_map, channel_map_type_t type)
{
    if (!msg)
    {
        LOGE("incorrect argument");
        return;
    }

    if (type == PATTERN)
    {

        for (int i = 0; i < channel_map->num_channels; i++)
        {
            int prefix_len = strlen(channel_map->channel_src[i]) - 1; // Exclude the '*'

            LOGD("Comparing received channel %s and template channel %s, prefix_len = %d\n", msg->channel_src, channel_map->channel_src[i], prefix_len);

            if (strncmp(msg->channel_src, channel_map->channel_src[i], prefix_len) == 0)
            {
                char *p_dststring = malloc(DSTSTRING_LEN);
                memset(p_dststring, 0, DSTSTRING_LEN);

                // Extract the suffix from srcstring
                const char *suffix = msg->channel_src + prefix_len;
                LOGD("suffix = %s\n", suffix);
                // Create a temporary buffer for dststring_template without '*'
                char dst_template[TEMP_DST_CHANNEL_BUFFER_SIZE_BYTES] = {0};
                strlcpy(dst_template, channel_map->channel_dst[i], strlen(channel_map->channel_dst[i]));
                dst_template[strlen(channel_map->channel_dst[i]) - 1] = '\0';

                // Construct the dststring
                snprintf(p_dststring, KAFKA_CHANNEL_MAX_LEN_BYTES, "%s%s", dst_template, suffix);
                LOGD("p_dststring = %s\n", p_dststring);
                msg->channel_dst = p_dststring;
            }
            else
            {
                msg->channel_dst = NULL;
            }
        }
    }
    else if (type == CHANNEL)
    {
        for (int i = 0; i < channel_map->num_channels; i++)
        {
            if (strcmp(channel_map->channel_src[i], msg->channel_src) == 0)
            {
                msg->channel_dst = strdup(channel_map->channel_dst[i]);
                break;
            }
        }
        LOGD("Destination channel name %s\n", msg->channel_dst);
    }
    else
    {
        LOGD("Incorrect channel map type");
    }
}

/**
 * @brief If the connection with redis is broken this function keeps attempting to connect with redis.
 *
 * @param c the pointer to redis context.
 * @param config cck_sys_config_t structure pointer
 *
 */
static void handle_reconnection(redisContext **c, cck_sys_config_t *config)
{
    LOGI("Attempting to reconnect...\n");
    while ((*c) == NULL || (*c)->err)
    {
        if (g_init_redis_communicator_done == false)
        {
            break;
        }  
        redisFree(*c);
        *c = redisConnect(config->redis_host, config->redis_port);
        if ((*c) == NULL || (*c)->err)
        {
            sleep(SLEEP_DURATION_ON_REDIS_CONN_FAIL); // Wait before retrying
        }
        else
        {
            LOGI("Reconnected to Redis server\n");
            break;
        }
    }
}

/**
 * @brief Function to establish connection with redis.
 *
 * @param config Microservice configuration.
 *
 */
static redisContext *connect_to_redis(cck_sys_config_t *config)
{
    redisContext *context = NULL;
    context = redisConnect(config->redis_host, config->redis_port);
    if (context == NULL || context->err)
    {
        if (context)
        {
            LOGE("Connection error: %s\n", context->errstr);
        }
        else
        {
            LOGE("Connection error: can't allocate redis context\n");
        }
        /* Attrempt re-connection */
        handle_reconnection(&context, config);
    }
    return context;
}

static int subscribe_to_redis(cck_channel_mapping_t *channel_map, redisContext *c)
{
    int status = CCK_STATUS_FAIL;
    redisReply *p_reply = NULL;
    for (int i = 0; i < channel_map->num_channels; i++)
    {
        if (strchr(channel_map->channel_src[i], '*') != NULL)
        {
            LOGD("PSubscribe to channel..%s", channel_map->channel_src[i]);
            /* Use PSUBSCRIBE for patterns */
            p_reply = redisCommand(c, "PSUBSCRIBE %s", channel_map->channel_src[i]);
        }
        else
        {
            LOGD("Subscribe to channel..%s", channel_map->channel_src[i]);
            /* Use SUBSCRIBE for specific topics */
            p_reply = redisCommand(c, "SUBSCRIBE %s", channel_map->channel_src[i]);
        }

        if (p_reply == NULL)
        {
            LOGE("Failed to subscribe with redisCommand()");
            return status;
        }
        else
        {
            freeReplyObject(p_reply);
        }
    }

    status = CCK_STATUS_SUCCESS;

    return status;
}

/**
 * @brief Function to publish data back to redis if msg error handeling is enabled.
 *
 * @param msg Message that needs to be published back.
 * @param config Microservice configuration.
 *
 */
static void publish_msg_to_redis(cck_message_t *msg, cck_sys_config_t *config)
{
    redisContext *context = connect_to_redis(config);
    if (context == NULL)
    {
        return;
    }

    // Publish a message to a topic
    redisReply *reply = (redisReply *)redisCommand(context,
                                                   "PUBLISH %s %s", msg->channel_src, msg->msg);
    if (reply == NULL)
    {
        LOGE("Publish error: %s\n", context->errstr);
        return;
    }

    LOGD("Published message to topic '%s':\n", msg->channel_src);

    // Clean up
    freeReplyObject(reply);
    redisFree(context);
}

/**
 * @brief Function to send redis msg to broker using msg communicator.
 *
 * @param type Defines a msg was received from pattern topic or non-pattern topic.
 * @param p_reply Redis reply msg
 * @param conn_id The connection id of broker
 * @param config Microservice configuration.
 *
 */
static int send_msg_to_msg_comm(int type, redisReply *p_reply, int conn_id, cck_sys_config_t *config)
{
    int status = CCK_STATUS_FAIL;
    cck_channel_mapping_t *channel_map = config->channel_mapping + conn_id;

    cck_message_t msg;
    msg.channel_dst = NULL;
    msg.connection_id = conn_id;
    if (type == PATTERN)
    {
        msg.channel_src = p_reply->element[PATTERN_CHANNEL_INDEX]->str;
        msg.msg_len = strlen(p_reply->element[PATTERN_MESSAGE_INDEX]->str);
        msg.msg = (uint8_t *)p_reply->element[PATTERN_MESSAGE_INDEX]->str;
    }
    else
    {
        msg.channel_src = p_reply->element[NON_PATTERN_CHANNEL_INDEX]->str;
        msg.msg_len = strlen(p_reply->element[NON_PATTERN_MESSAGE_INDEX]->str);
        msg.msg = (uint8_t *)p_reply->element[NON_PATTERN_MESSAGE_INDEX]->str;
    }
    LOGD("Received message from channel type %d: %s: %d\n", type, msg.channel_src, msg.msg_len);
    prepare_channel_map(&msg, channel_map, type);
    if (msg.channel_dst == NULL)
    {
        return CCK_STATUS_ERROR_NO_MAPPING;
    }

    /* send msg only if respective Kafka connection is established */
    if (g_kafka_connection_status[conn_id] == CCK_CONN_STATUS_SUCCESS)
    {
        status = cloud_connect_kafka_send_message(&msg);
        LOGD("Send status : %d and err handeling %d", status, config->msg_err_handling);
    }

    if (status != CCK_STATUS_SUCCESS && config->msg_err_handling)
    {
        /* publish msg back to redis */
        publish_msg_to_redis(&msg, config);
    }

    free(msg.channel_dst);
    return status;
}

/**
 * @brief A helper function to print periodic log while broker is disconnected and reading from redis is paused.
 *
 * @param conn_status_log_start_time_s start time refrence.
 * @param conn_status_log_end_time_s end time refrence
 * @param conn_id The connection id of broker
 *
 */
static void log_periodic_message(time_t *conn_status_log_start_time_s, time_t *conn_status_log_end_time_s, int conn_id)
{
    double conn_status_log_elapsed_time_s;
    time_t time_error_code = time(conn_status_log_end_time_s);
    if ((time_t)-1 == time_error_code)
    {
        LOGE("Failed to get the end time using time() function, error code = %d", time_error_code);
        return;
    }
    conn_status_log_elapsed_time_s = difftime(*conn_status_log_end_time_s,
                                              *conn_status_log_start_time_s);

    if (conn_status_log_elapsed_time_s >= KAFKA_CONNECTION_STATUS_LOG_INTERVAL_SECONDS)
    {
        LOGI("Kafka client %d is not connected to the Kafka server...", conn_id);

        // Reset the start time.
        time_error_code = time(conn_status_log_start_time_s);
        if ((time_t)-1 == time_error_code)
        {
            LOGE("Failed to get the start time using time() function, error code = %d", time_error_code);
            return;
        }
    }
}

static void handle_redis_message(redisReply *p_reply, cck_sys_config_t *p_config,
                                 int conn_id, int *msg_send_fail_count)
{
    CCKStatus status;
    if (p_reply->type == REDIS_REPLY_ARRAY && p_reply->elements == PATTERN_RESPONSE_LEN)
    {
        status = send_msg_to_msg_comm(PATTERN, p_reply, conn_id, p_config);
    }
    else if (p_reply->type == REDIS_REPLY_ARRAY && p_reply->elements == NON_PATTERN_RESPONSE_LEN)
    {
        status = send_msg_to_msg_comm(CHANNEL, p_reply, conn_id, p_config);
    }
    if (CCK_STATUS_SUCCESS != status)
    {
        *(msg_send_fail_count) = *(msg_send_fail_count) + 1;
        LOGE("ERROR: Failed to send kafka message : %d", *(msg_send_fail_count));
        /* Handle connection status. */
        if (*(msg_send_fail_count) > MAX_MSG_FAIL_COUNT && g_kafka_connection_status[conn_id] == CCK_CONN_STATUS_SUCCESS)
        {
            /* Reset count */
            *(msg_send_fail_count) = 0;

            /* Indicate connection failure */
            g_kafka_connection_status[conn_id] = CCK_CONN_STATUS_CLOSED;

            /* Try re-connection */
            cloud_connect_kafka_reconnect(conn_id, p_config);
        }
    }
    else
    {
        /* If send succes, reset the fail count */
        *(msg_send_fail_count) = 0;
    }

    freeReplyObject(p_reply);
}

void *redis_subscriber(void *arg)
{
    int *thread_index = (int *)arg;
    time_t conn_status_log_start_time_s, conn_status_log_end_time_s;
    CCKStatus status;
    // redisReply *p_reply;
    int msg_send_fail_count = 0;
    LOGD("Thread created %d", *thread_index);
    /* Get config */
    cck_sys_config_t *p_config = cloud_connect_kafka_get_config();
    cck_channel_mapping_t *channel_map = p_config->channel_mapping + *thread_index;

    /* connect to redis */
    redisContext *c = connect_to_redis(p_config);
    if (c == NULL || c->err)
    {
        return NULL;
    }

    status = subscribe_to_redis(channel_map, c);
    if (status != CCK_STATUS_SUCCESS)
    {
        LOGE("Failed to subscribe with redis");
        redisFree(c);
        return NULL;
    }
    // Message handling loop
    conn_status_log_start_time_s = 0;
    while (g_init_redis_communicator_done)
    {
        /* check if corresponsding connection with kafka is established */
        if (g_kafka_connection_status[*thread_index] != CCK_CONN_STATUS_SUCCESS)
        {
            log_periodic_message(&conn_status_log_start_time_s, &conn_status_log_end_time_s, *thread_index);
            continue;
        }
        /* Everytime before reading from redis, check if the connection is broken then attempt for reconnection first */
        if (c == NULL || c->err)
        {
            /*Attempt reconnection */
            handle_reconnection(&c, p_config);

            /* After reconnection you required a re-subscribe */
            subscribe_to_redis(channel_map, c);
        }
        redisReply *p_reply = NULL;
        if (redisGetReply(c, (void **)&p_reply) != REDIS_OK)
        {
            // LOGD("Message not received.....");
            continue;
        }
        if (p_reply == NULL)
        {
            continue;
        }
        handle_redis_message(p_reply, p_config, *thread_index, &msg_send_fail_count);
    }

    redisFree(c);
    LOGD("Exiting thread....");
    pthread_exit(NULL);
    return NULL;
}

static void kafka_conn_event_cb(int status, int conn_id)
{
    LOGD("Connection status of kafka: %d for conn_id: %d", status, conn_id);
    g_kafka_connection_status[conn_id] = status;
}

int cloud_connect_kafka_init_redis_communicator(cck_sys_config_t *config)
{
    if (config->channel_len > REDIS_MAX_CONSUMER)
    {
        return CCK_CONN_STATUS_ERROR;
    }

    g_init_redis_communicator_done = true;

    /* create redis consumer threads */
    for (int i = 0; i < config->channel_len; i++)
    {
        /* Set initial value of Kafka connections status as closed, it will be updated by Kafka connection callback */
        g_kafka_connection_status[i] = CCK_CONN_STATUS_CLOSED;

        g_thread_index[i] = i;
        int result = pthread_create(&g_threads[i], NULL, redis_subscriber, (void *)&g_thread_index[i]);
        if (result != 0)
        {
            LOGI("Error: Error creating thread: %d for for thread number %d", result, i);
            // Destroy created threads and it's resources.
            cloud_connect_kafka_deinit_redis_communicator(config);
            return CCK_STATUS_FAIL;
        }
    }

    /* register to message communicator for connection event */
    cloud_connect_kafka_register_conn_event(kafka_conn_event_cb);
    return CCK_STATUS_SUCCESS;
}

int cloud_connect_kafka_deinit_redis_communicator(cck_sys_config_t *config)
{
    /* remove redis consumer threads and it's resources */
    g_init_redis_communicator_done = false;
    for (int i = 0; i < config->channel_len; i++)
    {
        pthread_join(g_threads[i], NULL); // Wait for all threads to finish
    }

    LOGI("Dinit completed-----");
    return CCK_STATUS_SUCCESS;
}
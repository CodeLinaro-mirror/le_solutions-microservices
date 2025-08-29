/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    kafka_mock.c

DESCRIPTION
    Implementation file for the kafka mock client. It is currently mocking all the interfaces of kafka. By default all functions related to connection and publish data on kafka are returning
    success. To mock the error scenerios for them Unit Tests can use function void hook_rd_kafka_enable_error(kafka_mock_err_type_t option, bool enable).
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>
#include "kafka_mock.h"

#include "../include/cloud_connect_kafka_logging.h"

#define KAFKA_MOCK_ERR_MSG "Mock error msg"

static int g_published_msg_count = 0;

static int g_err_mock = ERR_TYPE_NONE;

static rd_kafka_t g_kafka_handle;

static rd_kafka_conf_t g_kafka_conf;

static rd_kafka_metadata_t g_metadata;

int hook_rd_kafka_get_msg_count(void)
{
    return g_published_msg_count;
}

void hook_rd_kafka_set_msg_count(int msg_count)
{
    g_published_msg_count = msg_count;
}

// Mock function for rd_kafka_conf_new
rd_kafka_conf_t *rd_kafka_conf_new(void)
{
    /* Return error if error mock is enabled */
    LOGD("Entering conf new");
    if (g_err_mock & ERR_TYPE_CONF)
    {
        LOGW("Mock error conf creation");
        return NULL;
    }
    // Allocate memory for the new configuration
    rd_kafka_conf_t *conf = &g_kafka_conf;
    return conf;
}

void hook_rd_kafka_error_callback(rd_kafka_t *rk, int err, const char *reason, void *opaque)
{
    if (rk == NULL || reason == NULL || opaque == NULL || (g_err_mock & ERR_TYPE_HANDLE_CB))
    {
        LOGE("Invalid arguments");
        return;
    }
    switch (err)
    {
        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
            LOGE("Disconnected from broker: %s\n", reason);
            break;
        default:
            LOGE("Error: %s\n", reason);
            break;
    }
}

// Mock function for rd_kafka_conf_set
int rd_kafka_conf_set(rd_kafka_conf_t *conf, const char *name, const char *value, char *errstr, size_t errstr_size)
{
    if (conf == NULL || name == NULL || value == NULL || (g_err_mock & ERR_TYPE_CONF_SET))
    {
        snprintf(errstr, errstr_size, "Invalid arguments");
        return -1;
    }

    return 0;
}

// Mock function to set the error callback
void rd_kafka_conf_set_error_cb(rd_kafka_conf_t *conf, error_cb_t error_callback)
{
    if (conf == NULL || error_callback == NULL)
    {
        LOGE("Invalid arguments");
        return;
    }
    conf->error_callback = error_callback;
}

// Mock function to set the opaque parameter
void rd_kafka_conf_set_opaque(rd_kafka_conf_t *conf, void *opaque)
{
    if (conf == NULL || opaque == NULL)
    {
        LOGE("Invalid arguments");
        return;
    }
    conf->opaque = opaque;
}

// Mock function for rd_kafka_conf_destroy
void rd_kafka_conf_destroy(rd_kafka_conf_t *conf)
{
}

// Mock function for rd_kafka_new
rd_kafka_t *rd_kafka_new(rd_kafka_type_t type, rd_kafka_conf_t *conf, char *errstr, size_t errstr_size)
{
    if (conf == NULL || (g_err_mock & ERR_TYPE_HANDLE))
    {
        snprintf(errstr, errstr_size, "Invalid configuration");
        return NULL;
    }

    // Allocate memory for the new Kafka instance
    rd_kafka_t *rk = &g_kafka_handle;

    return rk;
}

// Mock function for rd_kafka_topic_new
rd_kafka_topic_t *rd_kafka_topic_new(rd_kafka_t *rk, const char *topic, rd_kafka_conf_t *conf)
{
    if (rk == NULL || topic == NULL || (g_err_mock & ERR_TYPE_TOPIC))
    {
        LOGE("Invalid arguments\n");
        return NULL;
    }

    // Allocate memory for the new topic
    rd_kafka_topic_t *rkt = (rd_kafka_topic_t *)malloc(sizeof(rd_kafka_topic_t));
    if (rkt == NULL)
    {
        LOGE("Memory allocation failed\n");
        return NULL;
    }

    // Set the topic name and configuration
    rkt->name = strdup(topic);
    rkt->conf = conf;

    if (rkt->name == NULL)
    {
        LOGE("Memory allocation failed\n");
        free(rkt);
        return NULL;
    }

    return rkt;
}

// Mock function for rd_kafka_produce
int rd_kafka_produce(rd_kafka_topic_t *rkt, int partition, int msgflags, void *payload, size_t len, const char *key, size_t keylen, void *msg_opaque)
{
    if (rkt == NULL || payload == NULL || (g_err_mock & ERR_TYPE_PRODUCE))
    {
        LOGE("Invalid arguments\n");
        return -1;
    }
    g_published_msg_count++;

    // Simulate producing a message
    LOGD("Produced message to topic %s [partition %d]: %.*s\n", rkt->name, partition, (int)len, (char *)payload);

    return 0;
}

int rd_kafka_metadata(void *rk, int all_topics,
                      const void *only_rkt,
                      const struct rd_kafka_metadata **metadatap,
                      int timeout_ms)
{
    g_metadata.broker_cnt = 1;
    *metadatap = &g_metadata;
    return 0; // Return 0 to indicate success
}

// Mock implementation of rd_kafka_metadata_destroy
void rd_kafka_metadata_destroy(const struct rd_kafka_metadata *metadata)
{
}
// Mock function for rd_kafka_err2str
const char *rd_kafka_err2str(rd_kafka_resp_err_t err)
{
    return KAFKA_MOCK_ERR_MSG;
}

// Mock function for rd_kafka_last_error
rd_kafka_resp_err_t rd_kafka_last_error(void)
{
    return RD_KAFKA_RESP_ERR_NO_ERROR;
}

// Mock implementation of rd_kafka_flush
int rd_kafka_flush(rd_kafka_t *rk, int timeout_ms)
{
    if (rk == NULL)
    {
        return -1;
    }
    // Simulate flushing behavior
    LOGD("Flushing Kafka client with timeout: %d ms\n", timeout_ms);
    // Simulate successful flush
    return 0; // Return 0 to indicate success
}

// Mock implementation of rd_kafka_destroy
void rd_kafka_destroy(rd_kafka_t *rk)
{
}

// Mock function for rd_kafka_topic_destroy
void rd_kafka_topic_destroy(rd_kafka_topic_t *rkt)
{
    if (rkt == NULL)
    {
        return;
    }

    // Free the allocated memory for the topic name and configuration
    if (rkt->name)
    {
        free(rkt->name);
    }
    if (rkt->conf)
    {
        free(rkt->conf);
    }

    // Free the topic structure itself
    free(rkt);
}

// Mock function for rd_kafka_poll
int rd_kafka_poll(rd_kafka_t *rk, int timeout_ms)
{
    if (rk == NULL)
    {
        LOGE("Invalid Kafka instance\n");
        return -1;
    }

    // Simulate polling for events
    LOGD("Polling for events with timeout: %d ms\n", timeout_ms);

    // Simulate an event (for demonstration purposes)
    srand(time(NULL));
    int event_occurred = rand() % 2; // Randomly decide if an event occurred

    if (event_occurred)
    {
        LOGD("Event occurred!\n");
        return 1; // Indicate that an event occurred
    }
    else
    {
        LOGD("No event occurred\n");
        return 0; // Indicate that no event occurred
    }
}

void hook_rd_kafka_enable_error(kafka_mock_err_type_t option, bool enable)
{
    if (enable)
    {
        g_err_mock |= option;
    }
    else
    {
        g_err_mock &= ~option;
    }
}

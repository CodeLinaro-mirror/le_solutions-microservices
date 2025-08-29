/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    mosquitto_mock.c

DESCRIPTION
    Implementation file for the mosquitto mock client. It is currently mocking all the interfaces of mqtt. By default all functions related to connection and publish data on mqtt are returning
    success.
*/

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "mosquitto_mock.h"

#include "../include/cloud_connect_mqtt_logging.h"

mosquitto_mqtt_callbacks_t mosquitto_callbacks = {0};

static mosquitto_mock_err_type_t g_err_mock_type = MOSQ_ERR_TYPE_NONE;

static int g_send_msg_count = 0;

int hook_mosquitto_get_msg_count(void)
{
    return g_send_msg_count;
}

void hook_mosquitto_set_msg_count(int msg_count)
{
    g_send_msg_count = msg_count;
}

mosquitto_mqtt_callbacks_t hook_mosquitto_get_conn_callback(void)
{
    return mosquitto_callbacks;
}

void hook_mosquitto_enable_error(mosquitto_mock_err_type_t option, bool enable)
{
    if (enable)
    {
        g_err_mock_type |= option;
    }
    else
    {
        g_err_mock_type &= ~option;
    }
}

const char *mosquitto_reason_string(int reason_code)
{
    return "Mocked reason string";
}

const mosquitto_property *mosquitto_property_read_string(const mosquitto_property *proplist, int identifier, char **value, bool skip_first)
{
    if (identifier == MQTT_PROP_REASON_STRING && value && *value != NULL)
    {
        *value = strdup("Mocked reason string");
    }
    return NULL; // Return NULL as we do not care about return value here, only the output param
}

void mosquitto_connect_v5_callback_set(struct mosquitto *mosq, connect_callback_t connect_cb)
{
    if (mosq == NULL)
    {
        return;
    }
    mosquitto_callbacks.connect_cb = connect_cb;
}

void mosquitto_disconnect_v5_callback_set(struct mosquitto *mosq, disconnect_callback_t disconnect_cb)
{
    if (mosq == NULL)
    {
        return;
    }
    mosquitto_callbacks.disconnect_cb = disconnect_cb;
}

void mosquitto_log_callback_set(struct mosquitto *mosq, log_callback_t log_cb)
{
    if (mosq == NULL)
    {
        return;
    }
    mosquitto_callbacks.log_cb = log_cb;
}

void mosquitto_message_v5_callback_set(struct mosquitto *mosq, message_callback_t message_cb)
{
    if (mosq == NULL)
    {
        return;
    }
    mosquitto_callbacks.message_cb = message_cb;
}

void mosquitto_publish_v5_callback_set(struct mosquitto *mosq, publish_callback_t publish_cb)
{
    if (mosq == NULL)
    {
        return;
    }
    mosquitto_callbacks.publish_cb = publish_cb;
}

int mosquitto_int_option(struct mosquitto *mosq, enum mosq_opt_t option, int value)
{
    if (mosq == NULL)
    {
        return -1;
    }
    return 0; // Return success for this example
}

int mosquitto_lib_cleanup(void)
{
    return MOSQ_ERR_SUCCESS; // Return success for this example
}

int mosquitto_lib_init(void)
{
    return 0; // Return 0 to indicate success
}

struct mosquitto *mosquitto_new(const char *client_id, bool clean_session, void *obj)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_CLIENT_NEW)
    {
        return NULL; // mock error
    }
    struct mosquitto *mosq = (struct mosquitto *)malloc(sizeof(struct mosquitto));
    if (mosq == NULL)
    {
        return NULL;
    }

    if (client_id)
    {
        mosq->client_id = strdup(client_id);
    }
    else
    {
        mosq->client_id = NULL;
    }
    mosq->userdata = obj;

    return mosq;
}

void mosquitto_destroy(struct mosquitto *mosq)
{
    if (mosq != NULL)
    {
        if (mosq->client_id != NULL)
        {
            free(mosq->client_id);
            mosq->client_id = NULL;
        }
        free(mosq);
        mosq = NULL;
    }
}

int mosquitto_max_inflight_messages_set(struct mosquitto *mosq, unsigned int max_inflight_messages)
{
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    return 0; // Return 0 to indicate success
}

int mosquitto_connect(struct mosquitto *mosq, const char *host, int port, int keepalive)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_CLIENT_CONNECT)
    {
        return -1; // mock error
    }

    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    return 0; // Return 0 to indicate success
}

int mosquitto_loop_start(struct mosquitto *mosq)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_CLIENT_START)
    {
        return -1; // mock error
    }
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    return 0; // Return 0 to indicate success
}

int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic, int payloadlen, const void *payload, int qos, bool retain)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_SEND_MSG)
    {
        return -1; // Error
    }
    g_send_msg_count++;
    LOGD("Published message to topic: %s\n", topic);
    return 0; // Success
}

int mosquitto_disconnect(struct mosquitto *mosq)
{
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    LOGD("Disconnecting from mock host\n");
    return 0; // Return 0 to indicate success
}

// Mock function for mosquitto_loop_stop
int mosquitto_loop_stop(struct mosquitto *mosq, bool force)
{
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    return 0; // Return 0 to indicate success
}

int mosquitto_reconnect(struct mosquitto *mosq)
{
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }
    return 0; // Return 0 to indicate success
}

int mosquitto_reconnect_delay_set(struct mosquitto *mosq, unsigned int reconnect_delay, unsigned int reconnect_delay_max, bool reconnect_exponential_backoff)
{
    if (mosq == NULL)
    {
        return -1; // Return an error code
    }

    return 0; // Return 0 to indicate success
}

int mosquitto_tls_set(struct mosquitto *mosq, const char *cafile, const char *capath, const char *certfile, const char *keyfile, pw_callback_t pw_cb)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_TLS_SET)
    {
        return -1; // Error
    }
    if (mosq == NULL || cafile == NULL || certfile == NULL || keyfile == NULL)
    {
        return -1; // Return an error code for invalid parameters
    }
    mosquitto_callbacks.pw_cb = pw_cb;
    // Simulate successful TLS setup
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_tls_opts_set(struct mosquitto *mosq, int cert_reqs, const char *tls_version, const char *ciphers)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_TLS_OPTS_SET)
    {
        return -1; // Error
    }
    if (mosq == NULL)
    {
        return -1; // Return an error code for invalid parameters
    }
    // Simulate successful TLS options setup
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_tls_insecure_set(struct mosquitto *mosq, bool value)
{
    if (g_err_mock_type & MOSQ_ERR_TYPE_TLS_INSECURE_SET)
    {
        return -1; // Error
    }
    if (mosq == NULL)
    {
        return -1; // Return an error code for invalid parameters
    }

    return MOSQ_ERR_SUCCESS;
}

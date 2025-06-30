/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_mesage_config.c

DESCRIPTION
    Implementation file for the redis communicator module of Cloud Connect MQTT service.
*/
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef UNIT_TEST
#include "../tests/mosquitto_mock.h"
#else
#include <mosquitto.h>
#include <mqtt_protocol.h>
#endif

#include "cloud_connect_mqtt_logging.h"
#include "cloud_connect_mqtt_status.h"
#include "cloud_connect_mqtt_message_communicator.h"
#include "cloud_connect_mqtt_config.h"
#include "cloud_connect_utils.h"

/* Verify server certificate, should be true always  */
#define TLS_VERIFY_PEER 1

/* TLS Version - Using 1.3 has been inconsequential so far. So, we stick to 1.2 for now */
#define TLS_VERSION "tlsv1.2"

/* certificates files. */
#define TLS_ROOT_CA_FILE_PATH "./certificates/rootca.pem.crt"
#define TLS_CLIENT_CERTIFICATE_FILE_PATH "./certificates/certificate.pem.crt"
#define TLS_CLIENT_PVT_KEY_FILE_PATH "./certificates/private.pem.key"

/* PING request/response log identifier */
#define PING_LOG_IDENTIFIER "PING"

/* True if message retention is to be supported */
#define MQTT_RETENTION_ENABLED false

/* Represents the maximum number of outgoing QoS 1 and QoS 2 messages that this client will attempt to have “in flight” at once.
 * 20 is the default value from the library, we are staying at 20 as of now*/
#define MQTT_MAX_IN_FLIGHT_MESSAGES 20

/**
 * RECONNECT_DELAY: Use reconnect_delay parameter to change the delay between successive reconnection attempts
 * RECONNECT_DELAY_MAX: enable exponential backoff of the time between reconnections by setting reconnect_exponential_backoff to true and set an upper bound on the delay with reconnect_delay_max
 */
#define RECONNECT_DELAY 1
#define RECONNECT_DELAY_MAX 60

#define MAX_MQTT_CONNECTION 50
#define MAX_CLIENT_ID_LEN 40
#define MAX_BROKER_URL_LEN 100

/**
 * The Keep Alive interval is the maximum time between when a
 * client sends one control packet and the next
 */
#define MQTT_KEEP_ALIVE_INTERVAL_SECONDS 60

/**
 * An unsigned 8-bit integer that specifies the Quality of Service level
 * for PUBLISH message.
 * 0 - At most once (fire and forget)
 * 1 - At least once (guaranteed delivery with ACK)
 * 2 - Exactly once (highest level of reliability, with handshake).
 */
#define MQTT_QOS 1

/**
 * The size of the message body to be allocated.
 * This is often set to 0 for MQTT, as the message payload is typically
 * appended later using other functions like nng_msg_append.
 */
#define MQTT_MSG_OBJECT_INITIAL_SIZE_BYTES 0

/**
 * It modifies the behaviour of the send operation.
 *
 * 0 - Standard behaviour (blocking send).
 * NNG_FLAG_NONBLOCK - Non-blocking mode.
 */
#define MQTT_SENDMSG_FLAG 0

// TODO: Use this for managing connection specific data instead
// typedef struct ccm_connection_info_t {
//     struct mosquitto *instance;
//     ccm_mqtt_conn_cb_t cb;
//     int id = 0;
// } ccm_connection_info;


/* Global function pointer to notify mqtt connection status to redis communicator */
#ifndef UNIT_TEST
static struct mosquitto *gp_mosq[MAX_MQTT_CONNECTION] = {NULL};
#endif
static ccm_mqtt_conn_cb_t g_conn_callback = NULL;

// static struct ccm_connection_info[MAX_MQTT_CONNECTION] = { };

int g_connection_count = 0;

int (*pw_callback)(char *buf, int size, int rwflag, void *userdata) = NULL;

/**
 * @brief Callback function for handling connection events.
 *
 * This function is called when a new connection is established with the MQTT broker.
 *
 * @param p The pipe associated with the connection.
 * @param ev The event type, typically NNG_PIPE_EV_ADD_POST for connection events.
 * @param arg User-defined argument passed to the callback function.
 */
void on_connect(struct mosquitto *mosq, void *obj, int rc, int flags, const mosquitto_property *properties)
{
    int conn_id = (struct mosquitto **)(obj) - (&gp_mosq[0]);
    LOGI("mosquitto connect callback received for conn_id: %d, status: %d", conn_id, rc);
    if (rc == 0)
    {
        if (g_conn_callback)
        {
            g_conn_callback(CCM_CONN_STATUS_SUCCESS, conn_id);
        }
        g_connection_count++;
    }
    else
    {
        if (g_conn_callback)
        {
            g_conn_callback(rc, conn_id);
        }
    }
}

/**
 * @brief Callback function for handling disconnection events.
 *
 * This function is called when a connection with the MQTT broker is closed.
 *
 * @param p The pipe associated with the connection.
 * @param ev The event type, typically NNG_PIPE_EV_REM_POST for disconnection events.
 * @param arg User-defined argument passed to the callback function.
 */
void on_disconnect(struct mosquitto *mosq, void *obj, int rc, const mosquitto_property *properties)
{
    int conn_id = (struct mosquitto **)(obj) - (&gp_mosq[0]);
    LOGI("Disconnected from Mosquitto MQTT broker, conn_id: %d", conn_id);
    if (g_conn_callback)
    {
        g_conn_callback(CCM_CONN_STATUS_CLOSED, conn_id);
    }
    g_connection_count--;
}

void on_log_callback(struct mosquitto *mosq, void *obj, int level, const char *str)
{
    int conn_id = (struct mosquitto **)(obj) - (&gp_mosq[0]);
    switch (level)
    {
        case MOSQ_LOG_ERR:
            LOGE("MosquittoLibLog:[%d]%s", conn_id, str);
            break;
        case MOSQ_LOG_DEBUG:
            /* Here we are trying to identify PING request/response logs and exclude them from being printed to avoid unnecessary logging.*/
            if (strstr(str, PING_LOG_IDENTIFIER) == NULL)
            {
                LOGD("MosquittoLibLog:[%d]%s", conn_id, str);
            }
            break;
        case MOSQ_LOG_WARNING:
            LOGW("MosquittoLibLog:[%d]%s", conn_id, str);
            break;
        case MOSQ_LOG_INFO:
        default:
            LOGI("MosquittoLibLog:[%d]%s", conn_id, str);
            break;
    }
}

void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg, const mosquitto_property *properties)
{
    int conn_id = (struct mosquitto **)(obj) - (&gp_mosq[0]);
    LOGI("Received message on conn id %d: %s from topic: %s\n", conn_id, (char *)msg->payload, msg->topic);
}

void on_publish_callback(struct mosquitto *mosq, void *obj, int mid, int reason_code, const mosquitto_property *properties)
{
    int conn_id = (struct mosquitto **)(obj) - (&gp_mosq[0]);
    char *reason_string = NULL;
    if (reason_code != MOSQ_ERR_SUCCESS)
    {
        LOGE("Warning: Publish %d failed for connect id %d: %s.\n", conn_id, mid, mosquitto_reason_string(reason_code));
        mosquitto_property_read_string(properties, MQTT_PROP_REASON_STRING, &reason_string, false);
        if (reason_string)
        {
            LOGE("%s\n", reason_string);
            free(reason_string);
        }
    }
}

/**
 * @brief used to copy a password from a configuration structure to a provided buffer.
 * 
 * Callback function to provide the password
 * 
 * @param buf A pointer to the buffer where the password will be copied.
 * @param size The size of the buffer.
 * @param rwflag A flag indicating the read/write operation (not used in this function).
 * @param userdata User data (not used in this function).
 */
int password_callback(char *buf, int size, int rwflag, void *userdata) {
    ccm_sys_config_t *p_config = cloud_connect_mqtt_get_config();
    const char *password = (const char *)p_config->ssl_config.client_cert_password;
    if (size < strlen(password) + 1) {
        return 0; // Password is too long
    }
    LOGD("provided password: %s\n", password);
    strlcpy(buf, password, strlen(password));
    buf[strlen(password)] = '\0';
    return strlen(password);
}

/**
 * @brief Function to establish MQTT connection.
 *
 * This function will be called for connection and re-connection both purpose.
 *
 * @param conn_id The connection identifier.
 * @param config ccm_sys_config_t structure pointer
 */
static int cloud_connect_mqtt_connect(int conn_id, ccm_sys_config_t *config)
{
    int status = CCM_STATUS_FAIL;
    int rv = MOSQ_ERR_SUCCESS;
    struct mosquitto *p_mosq = NULL;
    char p_client_id[MAX_CLIENT_ID_LEN];
    char *p_client_id_prefix = NULL;

    if (conn_id < 0 || conn_id >= MAX_MQTT_CONNECTION)
    {
        LOGE("Invalid connection ID.");
        return status;
    }

    if (config->client_id_prefix)
    {
        snprintf(p_client_id, MAX_CLIENT_ID_LEN, "%.10s_%d", config->client_id_prefix, conn_id);
        p_client_id_prefix = p_client_id;
    }

    // sending address of conn_id doesn't make sense here. It is a stack variable doesn't last through callbacks.
    // lets send the address of gp_mosq[conn_id] here.
    // In callbacks, do a differential of (cbparam - gp_mosq) to figure out the conn id
    // Ownership of conn_id is poorly defined. To be addressed. Either it is owned by redic communicator, or mqtt communicator. Not both.

    p_mosq = mosquitto_new(p_client_id_prefix, true, &gp_mosq[conn_id]); // was &conn_id earlier

    gp_mosq[conn_id] = p_mosq;
    if (!gp_mosq[conn_id])
    {
        LOGE("Failed to create Mosquitto client.");
        goto end;
    }

    /*Set callbacks */
    mosquitto_log_callback_set(gp_mosq[conn_id], on_log_callback);
    mosquitto_connect_v5_callback_set(gp_mosq[conn_id], on_connect);
    mosquitto_disconnect_v5_callback_set(gp_mosq[conn_id], on_disconnect);
    mosquitto_message_v5_callback_set(gp_mosq[conn_id], on_message);
    mosquitto_publish_v5_callback_set(gp_mosq[conn_id], on_publish_callback);

    /*Set protocol version to V5*/
    mosquitto_int_option(gp_mosq[conn_id], MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    mosquitto_int_option(gp_mosq[conn_id], MOSQ_OPT_SEND_MAXIMUM, MQTT_MAX_IN_FLIGHT_MESSAGES);

    LOGI("Secure connection : %d", config->enable_ssl_connection);
    /*Set TLS parameters*/
    if (config->enable_ssl_connection != 0)
    {
        if(config->ssl_config.client_cert_password)
        {
            pw_callback = password_callback;
        }

        LOGD("file path = %s \n %s \n %s \n",config->ssl_config.root_ca_file,
            config->ssl_config.client_cert_file,
            config->ssl_config.client_private_key_file);

        rv = mosquitto_tls_set(gp_mosq[conn_id], config->ssl_config.root_ca_file, NULL, config->ssl_config.client_cert_file,
                                config->ssl_config.client_private_key_file, pw_callback); 
        if(MOSQ_ERR_SUCCESS != rv) 
        {
            LOGE("Failed to set TLS certs");
            goto end;
        }

        rv = mosquitto_tls_opts_set(gp_mosq[conn_id], TLS_VERIFY_PEER, TLS_VERSION, NULL);
        if (MOSQ_ERR_SUCCESS != rv)
        {
            LOGE("Failed to set TLS params");
            goto end;
        }

        rv = mosquitto_tls_insecure_set(gp_mosq[conn_id], (config->ssl_config.is_domain_validation_enabled == 0) ? true : false);
        if (MOSQ_ERR_SUCCESS != rv)
        {
            LOGE("Failed to set TLS insecure");
            goto end;
        }
    }

    LOGI("Mosquitto server host: %s, port: %d", config->mqtt_host, config->mqtt_port);
    if (mosquitto_connect(gp_mosq[conn_id], config->mqtt_host, config->mqtt_port, MQTT_KEEP_ALIVE_INTERVAL_SECONDS) != MOSQ_ERR_SUCCESS)
    {
        LOGE("Failed to connect to broker.");
        goto end;
    }
    else
    {
        LOGI("Mosquitto Connect returned success");
    }

    if (mosquitto_loop_start(gp_mosq[conn_id]) != MOSQ_ERR_SUCCESS)
    {
        LOGE("Failed to start Mosquitto loop.");
        goto end;
    }
    else
    {
        LOGI("Mosquitto loop start returned success");
        status = CCM_STATUS_SUCCESS;
    }

    return status;
end:
    if (gp_mosq[conn_id])
    {
        mosquitto_destroy(gp_mosq[conn_id]);
        gp_mosq[conn_id] = NULL;
    }
    return status;
}

int cloud_connect_mqtt_init_msg_communicator(ccm_sys_config_t *config)
{

    int rv = CCM_STATUS_FAIL;
    if (config->channel_len > MAX_CLIENT_ID_LEN)
    {
        return CCM_CONN_STATUS_ERROR;
    }
    rv = mosquitto_lib_init();

    if (rv != CCM_STATUS_SUCCESS)
    {
        LOGE("Failed to initialize mosquitto : %d", rv);
        goto end;
    }

    /* create and initialize the mqtt connection.*/
    for (int i = 0; i < config->channel_len; i++)
    {
        // g_arr_connection_id[i] = i;
        rv = cloud_connect_mqtt_connect(i, config);
        if (rv != CCM_STATUS_SUCCESS)
        {
            LOGE("Connection with conn_id : %d, failed", i);
            goto end;
        }
    }
    return CCM_STATUS_SUCCESS;
end:
    return CCM_STATUS_FAIL;
}

void cloud_connect_mqtt_reconnect(int conn_id, ccm_sys_config_t *config)
{
    if (conn_id < 0 || conn_id >= MAX_MQTT_CONNECTION || !gp_mosq[conn_id])
    {
        LOGE("Invalid connection ID or Mosquitto client not initialized.");
        return;
    }

    if (mosquitto_reconnect(gp_mosq[conn_id]) != MOSQ_ERR_SUCCESS)
    {
        LOGE("Failed to reconnect to broker for connection id : %d", conn_id);
    }
    else
    {
        LOGI("Reconnected successfully for connection_id %d", conn_id);
    }

    mosquitto_reconnect_delay_set(gp_mosq[conn_id], RECONNECT_DELAY, RECONNECT_DELAY_MAX, true);
}

int cloud_connect_mqtt_send_message(ccm_message_t *msg)
{
    if (msg->connection_id >= MAX_MQTT_CONNECTION || !gp_mosq[msg->connection_id])
    {
        LOGE("Invalid connection ID or Mosquitto client not initialized.");
        return CCM_STATUS_FAIL;
    }

    /*Second argument is NULL as we are not tracking individual messages for action on failure currently*/
    int ret = mosquitto_publish(gp_mosq[msg->connection_id], NULL, msg->channel_dst, msg->msg_len_bytes, msg->msg, MQTT_QOS, MQTT_RETENTION_ENABLED);
    if (ret != MOSQ_ERR_SUCCESS)
    {
        LOGE("Failed to publish message for connection id: %d", msg->connection_id);
        return CCM_STATUS_FAIL;
    }

    return CCM_STATUS_SUCCESS;
}

int cloud_connect_mqtt_register_conn_event(ccm_mqtt_conn_cb_t cb)
{
    g_conn_callback = cb;
    return CCM_STATUS_SUCCESS;
}

/**
 * @brief It releases all the mqtt client resources.
 *
 * @param config ccm_sys_config_t structure pointer
 */
static void free_mqtt_client_resource(ccm_sys_config_t *config)
{
    if (!config)
        return;

    /* close all mqtt connections and free resources*/
    for (int i = 0; i < config->channel_len; i++)
    {
        if (gp_mosq[i])
        {
            mosquitto_disconnect(gp_mosq[i]);
            mosquitto_loop_stop(gp_mosq[i], true);
            mosquitto_destroy(gp_mosq[i]);
            gp_mosq[i] = NULL;
        }
    }
    mosquitto_lib_cleanup();
}

int cloud_connect_mqtt_deinit_msg_communicator(ccm_sys_config_t *config)
{
    free_mqtt_client_resource(config);
    return CCM_STATUS_SUCCESS;
}

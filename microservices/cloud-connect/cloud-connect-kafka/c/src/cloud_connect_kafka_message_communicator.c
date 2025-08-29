/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_mesage_config.c

DESCRIPTION
    Implementation file for the redis communicator module of Cloud Connect kafka service.
*/
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#ifdef UNIT_TEST
#include "../tests/kafka_mock.h"
#else
#include <librdkafka/rdkafka.h>
#endif

#include "cloud_connect_kafka_status.h"
#include "cloud_connect_kafka_message_communicator.h"
#include "cloud_connect_kafka_config.h"
#include "cloud_connect_kafka_logging.h"

/* Global function pointer to notify kafka connection status to redis communicator */
cck_kafka_conn_cb_t g_conn_callback = NULL;
#define KAFKA_CLIENT_COUNT_MAX 10 // Max kafka clients to be created.
/* Size of error strings received from kafka client function calls. */
#define KAFKA_ERROR_STR_SIZE_BYTES 512
#define KAFKA_BROKER_NAME_SIZE_BYTES 500 // Length of kafka broker. (IP + Port)

#define KAFKA_CLIENT_ID_SIZE_BYTES 50

/* Kafka reconnection parameters*/
#define KAFKA_WAIT_TIME_FOR_PENDING_MSG_DELIVERY 1000
#define KAFKA_MIN_RECONNECT_TIME_MS "100"
#define KAFKA_MAX_RECONNECT_TIME_MS "10000"
#define KAFKA_KEEP_ALIVE_CONFIG "true"
#define KAFKA_RECONNECTION_STATUS_CHECK 1

/* certificates files. */
#define KAFKA_ROOT_CA_FILE_PATH "./certificates/rootca.pem.crt"
#define KAFKA_CLIENT_CERTIFICATE_FILE_PATH "./certificates/certificate.pem.crt"
#define KAFKA_CLIENT_PVT_KEY_FILE_PATH "./certificates/private.pem.key"

/* Global array representing kafka client instances. */
rd_kafka_t *gp_kafka_publish_client_arr[KAFKA_CLIENT_COUNT_MAX];
static int g_arr_connection_id[KAFKA_CLIENT_COUNT_MAX];

// Function to check broker state
int check_broker_state(rd_kafka_t *rk)
{
    const rd_kafka_metadata_t *metadata;
    rd_kafka_resp_err_t err;

    // Request metadata from the broker
    err = rd_kafka_metadata(rk, 1, NULL, &metadata, 5000);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        LOGE("%% Failed to acquire metadata: %s\n", rd_kafka_err2str(err));
        return CCK_STATUS_FAIL;
    }

    // Check the state of each broker
    for (int i = 0; i < metadata->broker_cnt; i++)
    {
        const rd_kafka_metadata_broker_t *broker = &metadata->brokers[i];
        LOGD("%% Broker %s:%d is %s\n", broker->host, broker->port,
             broker->id >= 0 ? "UP" : "DOWN");
        if (broker->id >= 0)
        {
            return CCK_STATUS_SUCCESS;
        }
    }

    // Free the metadata
    rd_kafka_metadata_destroy(metadata);
    return CCK_STATUS_FAIL;
}

// // Callback function for error events
void error_callback(rd_kafka_t *rk, int err, const char *reason, void *opaque)
{
    int *conn_id = (int *)opaque;
    switch (err)
    {
        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
            LOGE("Disconnected from broker: %s with conn_id: %d\n", reason, *conn_id);
            if (g_conn_callback)
            {
                /* Indicate redis communicator to pause reading */
                g_conn_callback(CCK_CONN_STATUS_CLOSED, *conn_id);
            }

            // check broker state
            while (1)
            {
                int status = CCK_STATUS_FAIL;
                // Check broker state
                status = check_broker_state(rk);
                if (status == CCK_STATUS_SUCCESS)
                {
                    if (g_conn_callback)
                    {
                        g_conn_callback(CCK_CONN_STATUS_SUCCESS, *conn_id);
                        status = CCK_STATUS_SUCCESS;
                    }
                    break;
                }

                // Sleep for a while before checking again
                sleep(KAFKA_RECONNECTION_STATUS_CHECK);
            }
            break;
        default:
            LOGE("Error: %s\n", reason);
            break;
    }
}

/**
 * @brief Function to establish Kafka connection.
 *
 * This function will be called for connection and re-connection both purpose.
 *
 * @param conn_id The connection identifier.
 * @param config cck_sys_config_t structure pointer
 */
static int cloud_connect_kafka_connect(int conn_id, cck_sys_config_t *config)
{
    int status = CCK_STATUS_FAIL;
    char errstr[KAFKA_ERROR_STR_SIZE_BYTES] = {0};
    int errstr_size = sizeof(errstr);
    char brokers[KAFKA_BROKER_NAME_SIZE_BYTES] = {0};
    char client_id[KAFKA_CLIENT_ID_SIZE_BYTES] = {0};
    char *p_client_id_prefix = NULL;

    snprintf(brokers, sizeof(brokers), "%s:%d", config->kafka_host, config->kafka_port);

    LOGD("Connecting to Kafka, brokers = %s\n", brokers);

    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    if (!conf)
    {
        LOGE("Failed to create kafka configuration object\n");
        goto end;
    }

    // Register the error callback
    rd_kafka_conf_set_error_cb(conf, error_callback);

    // Set the opaque parameter
    rd_kafka_conf_set_opaque(conf, &g_arr_connection_id[conn_id]);

    if (rd_kafka_conf_set(conf, "bootstrap.servers", brokers, errstr, errstr_size) != RD_KAFKA_CONF_OK)
    {
        LOGE("Failed to configure Kafka client: %s\n", errstr);
        rd_kafka_conf_destroy(conf);
        goto end;
    }

    if (config->client_id_prefix)
    {
        // Create client ID
        snprintf(client_id, KAFKA_CLIENT_ID_SIZE_BYTES, "%.10s_%d", config->client_id_prefix, conn_id);
        p_client_id_prefix = client_id;
    }

    // Set the client ID
    if (p_client_id_prefix)
    {
        if (rd_kafka_conf_set(conf, "client.id", p_client_id_prefix, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            LOGE("%% Failed to set client.id: %s\n", errstr);
            goto end;
        }
    }

    // Enable automatic reconnection
    if (rd_kafka_conf_set(conf, "reconnect.backoff.ms", KAFKA_MIN_RECONNECT_TIME_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
    {
        LOGE("%% Failed to set reconnect.backoff.ms: %s\n", errstr);
        goto end;
    }

    // defining max reconnect time by default its value is 10 min but we are configuring it to 10 seconds
    if (rd_kafka_conf_set(conf, "reconnect.backoff.max.ms", KAFKA_MAX_RECONNECT_TIME_MS, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
    {
        LOGE("%% Failed to set reconnect.backoff.max.ms: %s\n", errstr);
        goto end;
    }

    // enabling feature of periodic messages sent over a TCP connection to ensure that the connection is still active
    if (rd_kafka_conf_set(conf, "socket.keepalive.enable", KAFKA_KEEP_ALIVE_CONFIG, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
    {
        LOGE("%% Failed to set socket.keepalive.enable: %s\n", errstr);
        goto end;
    }

    // request message max bytes limit
    if (rd_kafka_conf_set(conf, "message.max.bytes", config->kafka_msg_max_bytes, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
    {
        LOGE("%% Failed to set message.max.bytes: %s\n", errstr);
        goto end;
    }

    LOGI("Secure connection : %d", config->enable_ssl_connection);
    if (config->enable_ssl_connection != 0)
    {
        // Set SSL configuration
        if (rd_kafka_conf_set(conf, "security.protocol", "SSL", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            LOGE("%% Failed to set security.protocol: %s\n", errstr);
            goto end;
        }
        LOGD("file path = %s \n %s \n %s \n",config->ssl_config.root_ca_file,
            config->ssl_config.client_cert_file,
            config->ssl_config.client_private_key_file);
        if (rd_kafka_conf_set(conf, "ssl.ca.location", config->ssl_config.root_ca_file, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            LOGE("%% Failed to set ssl.ca.location: %s\n", errstr);
            goto end;
        }
        if (rd_kafka_conf_set(conf, "ssl.certificate.location", config->ssl_config.client_cert_file, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            LOGE("%% Failed to set ssl.certificate.location: %s\n", errstr);
            goto end;
        }
        if (rd_kafka_conf_set(conf, "ssl.key.location", config->ssl_config.client_private_key_file, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            LOGE("%% Failed to set ssl.key.location: %s\n", errstr);
            goto end;
        }
        if(config->ssl_config.client_cert_password) 
        {
            if (rd_kafka_conf_set(conf, "ssl.key.password", config->ssl_config.client_cert_password, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
            {
                LOGE("%% Failed to set ssl.key.password: %s\n", errstr);
                goto end;
            }
        }
        if (!config->ssl_config.is_domain_validation_enabled)
        {
            // disable hostname validation
            if (rd_kafka_conf_set(conf, "ssl.endpoint.identification.algorithm", "none", errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
            {
                LOGE("%% Failed to disable hostname verification: %s\n", errstr);
                goto end;
            }
        }
    }

    rd_kafka_t *rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, errstr_size);
    if (!rk)
    {
        LOGE("Failed to create Kafka producer: %s\n", errstr);
        rd_kafka_conf_destroy(conf);
        goto end;
    }
    LOGI("Kafka producer created - %d\n", conn_id);

    // // Add brokers
    // if (rd_kafka_brokers_add(rk, brokers) == 0) {
    //     LOGE("No valid brokers specified\n");
    //     goto end;
    // }

    // // Poll to handle events
    // while (1) {
    //     rd_kafka_poll(rk, 1000);

    //     // Check metadata to infer connection success
    //     const struct rd_kafka_metadata *metadata;
    //     if (rd_kafka_metadata(rk, 0, NULL, &metadata, 5000) == RD_KAFKA_RESP_ERR_NO_ERROR) {
    //         printf("Connected to broker...................................................................................................................SUCCESS\n");
    //         rd_kafka_metadata_destroy(metadata);
    //         break;
    //     }
    // }

    // Save kafka publisher instance to global variable so that it can be accessed in other functions.
    gp_kafka_publish_client_arr[conn_id] = rk;

    if (g_conn_callback)
    {
        g_conn_callback(CCK_CONN_STATUS_SUCCESS, conn_id);
        status = CCK_STATUS_SUCCESS;
    }
end:
    return status;
}

int cloud_connect_kafka_init_msg_communicator(cck_sys_config_t *config)
{
    /* create and initialize the kafka connection.*/
    if (config->channel_len > KAFKA_CLIENT_COUNT_MAX)
    {
        return CCK_CONN_STATUS_ERROR;
    }

    /* create and initialize the Kafka connection.*/
    int rv = CCK_STATUS_FAIL;
    for (int i = 0; i < config->channel_len; i++)
    {
        g_arr_connection_id[i] = i;
        rv = cloud_connect_kafka_connect(i, config);
        if (rv == CCK_STATUS_FAIL)
        {
            LOGE("Connection with conn_id : %d, failed", i);
            goto end;
        }
    }
    return CCK_STATUS_SUCCESS;
end:
    return CCK_STATUS_FAIL;
}

int cloud_connect_kafka_send_message(cck_message_t *msg)
{
    /* publish message to kafka */

    rd_kafka_t *p_kafka_publish_client = gp_kafka_publish_client_arr[msg->connection_id];
    rd_kafka_topic_t *rkt = rd_kafka_topic_new(p_kafka_publish_client, msg->channel_dst, NULL);
    if (!rkt)
    {
        LOGE("Failed to create Kafka topic object\n");
        return CCK_STATUS_FAIL;
    }

    int err = rd_kafka_produce(
        rkt,
        RD_KAFKA_PARTITION_UA,
        RD_KAFKA_MSG_F_COPY,
        (void *)(msg->msg), msg->msg_len,
        NULL, 0,
        NULL);

    if (err == -1)
    {
        LOGE("Failed to produce message: %s\n", rd_kafka_err2str(rd_kafka_last_error()));
        rd_kafka_topic_destroy(rkt);
        return CCK_STATUS_FAIL;
    }

    rd_kafka_poll(p_kafka_publish_client, 0); // Poll to handle delivery reports
    rd_kafka_topic_destroy(rkt);
    LOGD("Send 1 message to the kafka broker on topic %s\n", msg->channel_dst);

    return CCK_STATUS_SUCCESS;
}

void cloud_connect_kafka_reconnect(int conn_id, cck_sys_config_t *config)
{
    // Wait for all messages to be delivered
    rd_kafka_flush(gp_kafka_publish_client_arr[conn_id], 10000); // Wait for up to 10 seconds
    // Destroy the Kafka producer instance
    rd_kafka_destroy(gp_kafka_publish_client_arr[conn_id]);
    if (g_conn_callback)
    {
        g_conn_callback(CCK_CONN_STATUS_CLOSED, conn_id);
    }
    cloud_connect_kafka_connect(conn_id, config);
}

int cloud_connect_kafka_register_conn_event(cck_kafka_conn_cb_t cb)
{
    if (cb != NULL)
    {
        g_conn_callback = cb;
        return CCK_STATUS_SUCCESS;
    }

    return CCK_STATUS_FAIL;
}

/**
 * @brief It releases all the kafka client resources.
 *
 * @param config cck_sys_config_t structure pointer
 */
static void free_kafka_client_resource(cck_sys_config_t *config)
{
    if (!config)
        return;

    /* close all kafka connections and free resources*/
    for (int i = 0; i < config->channel_len; i++)
    {
        rd_kafka_t *p_kafka_publish_client = gp_kafka_publish_client_arr[i];

        // Wait for all messages to be delivered
        rd_kafka_flush(p_kafka_publish_client, KAFKA_WAIT_TIME_FOR_PENDING_MSG_DELIVERY); // Wait for up to 10 seconds

        // Destroy the Kafka producer instance
        rd_kafka_destroy(p_kafka_publish_client);
        if (g_conn_callback)
        {
            g_conn_callback(CCK_CONN_STATUS_CLOSED, i);
        }
    }
}

int cloud_connect_kafka_deinit_msg_communicator(cck_sys_config_t *config)
{
    /* close all kafka connections and free resources*/
    free_kafka_client_resource(config);
    return CCK_STATUS_SUCCESS;
}
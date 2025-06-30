/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    mosquitto_mock.h

DESCRIPTION
    Header file for the nanomq mock client.
*/

#ifndef MOSQUITTO_MOCK_H
#define MOSQUITTO_MOCK_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* Log types */
#define MOSQ_LOG_NONE 0x00
#define MOSQ_LOG_INFO 0x01
#define MOSQ_LOG_NOTICE 0x02
#define MOSQ_LOG_WARNING 0x04
#define MOSQ_LOG_ERR 0x08
#define MOSQ_LOG_DEBUG 0x10

#define MOSQ_ERR_SUCCESS 0

#define MQTT_PROTOCOL_V31 3
#define MQTT_PROTOCOL_V311 4
#define MQTT_PROTOCOL_V5 5

#define MAX_MQTT_CONNECTION 50

enum MQTTPropertyCodes
{
    MQTT_PROP_REASON_STRING = 31,
};

typedef enum mosquitto_mock_err_type
{
    MOSQ_ERR_TYPE_NONE = 0,
    MOSQ_ERR_TYPE_CLIENT_NEW = 1 << 0,
    MOSQ_ERR_TYPE_CLIENT_CONNECT = 1 << 1,
    MOSQ_ERR_TYPE_CLIENT_START = 1 << 2,
    MOSQ_ERR_TYPE_SEND_MSG = 1 << 3,
    MOSQ_ERR_TYPE_TLS_SET = 1 << 4,
    MOSQ_ERR_TYPE_TLS_OPTS_SET = 1 << 5,
    MOSQ_ERR_TYPE_TLS_INSECURE_SET = 1 << 6,
} mosquitto_mock_err_type_t;

/* Enum: mosq_opt_t
 *
 * Client options.
 *
 * See <mosquitto_int_option>, <mosquitto_string_option>, and <mosquitto_void_option>.
 */
enum mosq_opt_t
{
    MOSQ_OPT_PROTOCOL_VERSION = 1,
    MOSQ_OPT_SEND_MAXIMUM = 5,

};

struct mosquitto
{
    char *client_id;
    void *userdata;
};

// Define a mock structure for mosquitto_property
typedef struct mosquitto_property
{
    int identifier;
    union
    {
        int int_value;
        char *str_value;
    } value;
    struct mosquitto_property *next;
} mosquitto_property;

struct mosquitto_message
{
    int mid;        // Message ID
    char *topic;    // Topic name
    void *payload;  // Message payload
    int payloadlen; // Length of the payload
    int qos;        // Quality of Service level
    bool retain;    // Retain flag
};

typedef void (*connect_callback_t)(struct mosquitto *, void *, int, int, const mosquitto_property *);
typedef void (*disconnect_callback_t)(struct mosquitto *, void *, int, const mosquitto_property *);
typedef void (*log_callback_t)(struct mosquitto *, void *, int, const char *);
typedef void (*message_callback_t)(struct mosquitto *, void *, const struct mosquitto_message *, const mosquitto_property *);
typedef void (*publish_callback_t)(struct mosquitto *, void *, int, int, const mosquitto_property *);
typedef int (*pw_callback_t)(char *buf, int size, int rwflag, void *userdata);

/**
 * @brief Struct to hold callback references.
 */
typedef struct
{
    connect_callback_t connect_cb;
    disconnect_callback_t disconnect_cb;
    log_callback_t log_cb;
    message_callback_t message_cb;
    publish_callback_t publish_cb;
    pw_callback_t pw_cb;
} mosquitto_mqtt_callbacks_t;

extern struct mosquitto *gp_mosq[MAX_MQTT_CONNECTION];

/**
 * @brief Get connection callback.
 * @return Connection callback structure.
 */
extern mosquitto_mqtt_callbacks_t mosquitto_callbacks;

/**
 * @brief Get connection callback.
 * @return Connection callback structure.
 */
mosquitto_mqtt_callbacks_t hook_mosquitto_get_conn_callback(void);

/**
 * @brief Enable or disable error simulation.
 * @param option Error type to enable or disable.
 * @param enable True to enable, false to disable.
 */
void hook_mosquitto_enable_error(mosquitto_mock_err_type_t option, bool enable);

/**
 * @brief Set message count.
 * @param msg_count Number of messages.
 */
void hook_mosquitto_set_msg_count(int msg_count);

/**
 * @brief Get message count.
 * @return Number of messages.
 */
int hook_mosquitto_get_msg_count(void);

/**
 * @brief Publish a message.
 * @param mosq Mosquitto client instance.
 * @param mid Message ID.
 * @param topic Topic name.
 * @param payloadlen Length of the payload.
 * @param payload Message payload.
 * @param qos Quality of Service level.
 * @param retain Retain flag.
 * @return Result code.
 */
int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic, int payloadlen, const void *payload, int qos, bool retain);

/**
 * @brief Create a new mosquitto client instance.
 * @param id Client ID.
 * @param clean_session Clean session flag.
 * @param obj User data.
 * @return Mosquitto client instance.
 */
struct mosquitto *mosquitto_new(const char *id, bool clean_session, void *obj);

/**
 * @brief Set the logging callback.
 * @param mosq Mosquitto client instance.
 * @param on_log Logging callback function.
 */
void mosquitto_log_callback_set(struct mosquitto *mosq, void (*on_log)(struct mosquitto *, void *, int, const char *));

/**
 * @brief Set the connect callback for MQTT v5.
 * @param mosq Mosquitto client instance.
 * @param on_connect Connect callback function.
 */
void mosquitto_connect_v5_callback_set(struct mosquitto *mosq, void (*on_connect)(struct mosquitto *, void *, int, int, const mosquitto_property *props));

/**
 * @brief Set the disconnect callback for MQTT v5.
 * @param mosq Mosquitto client instance.
 * @param on_disconnect Disconnect callback function.
 */
void mosquitto_disconnect_v5_callback_set(struct mosquitto *mosq, void (*on_disconnect)(struct mosquitto *, void *, int, const mosquitto_property *props));

/**
 * @brief Set the message callback for MQTT v5.
 * @param mosq Mosquitto client instance.
 * @param on_message Message callback function.
 */
void mosquitto_message_v5_callback_set(struct mosquitto *mosq, void (*on_message)(struct mosquitto *, void *, const struct mosquitto_message *, const mosquitto_property *props));

/**
 * @brief Set the publish callback for MQTT v5.
 * @param mosq Mosquitto client instance.
 * @param on_publish Publish callback function.
 */
void mosquitto_publish_v5_callback_set(struct mosquitto *mosq, void (*on_publish)(struct mosquitto *, void *, int, int, const mosquitto_property *props));

/**
 * @brief Set integer options for the client.
 * @param mosq Mosquitto client instance.
 * @param option Option to set.
 * @param value Value to set.
 * @return Result code.
 */
int mosquitto_int_option(struct mosquitto *mosq, enum mosq_opt_t option, int value);

/**
 * @brief Configure the client for certificate based SSL/TLS support.
 * @param mosq Mosquitto client instance.
 * @param cafile Path to the CA file.
 * @param capath Path to the CA directory.
 * @param certfile Path to the certificate file.
 * @param keyfile Path to the key file.
 * @param pw_callback Password callback function.
 * @return Result code.
 */
int mosquitto_tls_set(struct mosquitto *mosq, const char *cafile, const char *capath, const char *certfile, const char *keyfile, int (*pw_callback)(char *buf, int size, int rwflag, void *userdata));

/**
 * @brief Set advanced SSL/TLS options.
 * @param mosq Mosquitto client instance.
 * @param cert_reqs Certificate requirements.
 * @param tls_version TLS version.
 * @param ciphers Cipher suite.
 * @return Result code.
 */
int mosquitto_tls_opts_set(struct mosquitto *mosq, int cert_reqs, const char *tls_version, const char *ciphers);

/**
 * @brief Configure verification of the server hostname in the server certificate.
 * @param mosq Mosquitto client instance.
 * @param value True to enable, false to disable.
 * @return Result code.
 */
int mosquitto_tls_insecure_set(struct mosquitto *mosq, bool value);

/**
 * @brief Connect to an MQTT broker.
 * @param mosq Mosquitto client instance.
 * @param host Hostname.
 * @param port Port number.
 * @param keepalive Keepalive interval.
 * @return Result code.
 */
int mosquitto_connect(struct mosquitto *mosq, const char *host, int port, int keepalive);

/**
 * @brief Free memory associated with a mosquitto client instance.
 * @param mosq Mosquitto client instance.
 */
void mosquitto_destroy(struct mosquitto *mosq);

/**
 * @brief Start a new thread to process network traffic.
 * @param mosq Mosquitto client instance.
 * @return Result code.
 */
int mosquitto_loop_start(struct mosquitto *mosq);

/**
 * @brief Initialize the mosquitto library.
 * @return Result code.
 */
int mosquitto_lib_init(void);

/**
 * @brief Reconnect to a broker.
 * @param mosq Mosquitto client instance.
 * @return Result code.
 */
int mosquitto_reconnect(struct mosquitto *mosq);

/**
 * @brief Control the behaviour of the client when it has unexpectedly disconnected.
 * @param mosq Mosquitto client instance.
 * @param reconnect_delay Reconnect delay.
 * @param reconnect_delay_max Maximum reconnect delay.
 * @param reconnect_exponential_backoff True to enable exponential backoff, false to disable.
 * @return Result code.
 */
int mosquitto_reconnect_delay_set(struct mosquitto *mosq, unsigned int reconnect_delay, unsigned int reconnect_delay_max, bool reconnect_exponential_backoff);

/**
 * @brief Publish a message on a given topic.
 * @param mosq Mosquitto client instance.
 * @param mid Message ID.
 * @param topic Topic name.
 * @param payloadlen Length of the payload.
 * @param payload Message payload.
 * @param qos Quality of Service level.
 * @param retain Retain flag.
 * @return Result code.
 */
int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic, int payloadlen, const void *payload, int qos, bool retain);

/**
 * @brief Disconnect from the broker.
 * @param mosq Mosquitto client instance.
 * @return Result code.
 */
int mosquitto_disconnect(struct mosquitto *mosq);

/**
 * @brief Stop the network thread previously created with mosquitto_loop_start.
 * @param mosq Mosquitto client instance.
 * @param force True to force stop, false to stop gracefully.
 * @return Result code.
 */
int mosquitto_loop_stop(struct mosquitto *mosq, bool force);

/**
 * @brief Free resources associated with the library.
 * @return Result code.
 */
int mosquitto_lib_cleanup(void);

/**
 * @brief Read a string property value from a property.
 * @param proplist Property list.
 * @param identifier Property identifier.
 * @param value Pointer to store the string value.
 * @param skip_first True to skip the first property, false otherwise.
 * @return Property list.
 */
const mosquitto_property *mosquitto_property_read_string(const mosquitto_property *proplist, int identifier, char **value, bool skip_first);

/**
 * @brief Obtain a const string description of an MQTT reason code.
 * @param reason_code Reason code.
 * @return Reason string. Releasing memory for the returned string is not required.
 */
const char *mosquitto_reason_string(int reason_code);

/**
 * @brief Used to configure TLS settings for mosquitto client.
 * @param mosq Mosquitto client instance.
 * @param cafile Path to the file containing  PEM encoded CA certificate.
 * @param capath Path to a directory containing PEM encoded CA certificates.
 * @param certfile Path to the file containing the PEM encoded client certificate.
 * @param keyfile Path to the file containing the PEM encoded private key corresponding to the client certificate.
 * @param pw_cb A callback function for providing the password for the encrypted private key.
 * @return Result code.
 */
int mosquitto_tls_set(struct mosquitto *mosq, const char *cafile, const char *capath, const char *certfile, const char *keyfile, pw_callback_t pw_cb);

/**
 * @brief used to configure various TLS options for a Mosquitto client.
 * @param mosq Mosquitto client instance.
 * @param cert_reqs An integer specifying the certificate verification requirements. Common values include:
 *        - MOSQ_SSL_VERIFY_NONE: No verification of the server certificate.
 *        - MOSQ_SSL_VERIFY_PEER: Verify the server certificate.
 * @param tls_version string specifying the TLS version to use.
 * @param ciphers string specifying the ciphers to be used for the connection. If NULL, the default ciphers are used.
 * @return Result code.
 */
int mosquitto_tls_opts_set(struct mosquitto *mosq, int cert_reqs, const char *tls_version, const char *ciphers);

/**
 * @brief used to configure whether the client should verify the server's certificate hostname.
 * @param mosq Mosquitto client instance.
 * @param value boolean value indicating whether to disable hostname verification:
 *           - true: Disable hostname verification (insecure).
 *           - false: Enable hostname verification (secure).
 * @return Result code.
 */
int mosquitto_tls_insecure_set(struct mosquitto *mosq, bool value);

#endif // MOSQUITTO_MOCK_H

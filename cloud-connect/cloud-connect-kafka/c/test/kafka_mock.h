/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    kafka_mock.h

DESCRIPTION
    Header file for the kafka mock client.
*/

#ifndef TEST_KAFKA_MOCK_H_
#define TEST_KAFKA_MOCK_H_

#include <stdbool.h>

#define RD_KAFKA_CONF_OK 0
#define RD_KAFKA_PARTITION_UA ((int32_t)-1)
#define RD_KAFKA_MSG_F_COPY 0x2 /**< rdkafka will make a copy of the payload. */

typedef struct rd_kafka_s rd_kafka_t;

// Error callback function type
typedef void (*error_cb_t)(rd_kafka_t *rk, int err, const char *reason, void *opaque);

// Enum to mock error for the different functions
typedef enum kafka_mock_err_type
{
    ERR_TYPE_NONE = 0,
    ERR_TYPE_TOPIC = 1 << 0,
    ERR_TYPE_PRODUCE = 1 << 1,
    ERR_TYPE_CONF = 1 << 2,
    ERR_TYPE_CONF_SET = 1 << 3,
    ERR_TYPE_HANDLE = 1 << 4,
    ERR_TYPE_HANDLE_CB = 1 << 5
} kafka_mock_err_type_t;

// Mock enumeration for Kafka types
typedef enum
{
    RD_KAFKA_PRODUCER,
    RD_KAFKA_CONSUMER
} rd_kafka_type_t;

// Mock enumeration for Kafka error codes
typedef enum
{
    RD_KAFKA_RESP_ERR_NO_ERROR,
    RD_KAFKA_RESP_ERR_UNKNOWN,
    RD_KAFKA_RESP_ERR__TRANSPORT,
    RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN,
    RD_KAFKA_RESP_ERR_OFFSET_OUT_OF_RANGE,
    // Add other error codes as needed
} rd_kafka_resp_err_t;

// Mock structure for rd_kafka_conf_t
typedef struct
{
    // Add any necessary fields here
    char *key;
    char *value;
    error_cb_t error_callback;
    void *opaque;
} rd_kafka_conf_t;

// Define the metadata structures
typedef struct rd_kafka_metadata_broker
{
    int id;
    char *host;
    int port;
} rd_kafka_metadata_broker_t;

struct rd_kafka_metadata_partition
{
    int id;
    int leader;
};

struct rd_kafka_metadata_topic
{
    char *topic;
    int partition_cnt;
    struct rd_kafka_metadata_partition *partitions;
};

typedef struct rd_kafka_metadata
{
    int broker_cnt;
    struct rd_kafka_metadata_broker *brokers;
    int topic_cnt;
    struct rd_kafka_metadata_topic *topics;
    int orig_broker_id;
    char *orig_broker_name;
} rd_kafka_metadata_t;

// Mock structure for rd_kafka_t
typedef struct rd_kafka_s
{
    char *type;
    rd_kafka_conf_t *conf;
} rd_kafka_t;

typedef struct
{
    char *name;
    rd_kafka_conf_t *conf;
} rd_kafka_topic_t;

/**
 * @brief Create a new Kafka configuration object.
 *
 * @return A pointer to a new Kafka configuration object.
 */
rd_kafka_conf_t *rd_kafka_conf_new(void);

int rd_kafka_metadata(void *rk, int all_topics,
                      const void *only_rkt,
                      const struct rd_kafka_metadata **metadatap,
                      int timeout_ms);

void rd_kafka_metadata_destroy(const struct rd_kafka_metadata *metadata);

/**
 * @brief Destroy a Kafka configuration object.
 *
 * @param conf A pointer to the Kafka configuration object to be destroyed.
 */
void rd_kafka_conf_destroy(rd_kafka_conf_t *conf);

/**
 * @brief Create a new Kafka handle.
 *
 * @param type The type of Kafka handle to create (producer or consumer).
 * @param conf A pointer to the Kafka configuration object.
 * @param errstr A buffer to hold any error message.
 * @param errstr_size The size of the error message buffer.
 * @return A pointer to the new Kafka handle.
 */
rd_kafka_t *rd_kafka_new(rd_kafka_type_t type, rd_kafka_conf_t *conf, char *errstr, size_t errstr_size);

/**
 * @brief Create a new Kafka topic handle.
 *
 * @param rk A pointer to the Kafka handle.
 * @param topic The name of the topic.
 * @param conf A pointer to the Kafka topic configuration object.
 * @return A pointer to the new Kafka topic handle.
 */
rd_kafka_topic_t *rd_kafka_topic_new(rd_kafka_t *rk, const char *topic, rd_kafka_conf_t *conf);

/**
 * @brief Set a configuration property.
 *
 * @param conf A pointer to the Kafka configuration object.
 * @param name The name of the configuration property.
 * @param value The value of the configuration property.
 * @param errstr A buffer to hold any error message.
 * @param errstr_size The size of the error message buffer.
 * @return 0 on success, or a non-zero error code on failure.
 */
int rd_kafka_conf_set(rd_kafka_conf_t *conf, const char *name, const char *value, char *errstr, size_t errstr_size);

/**
 * @brief Produce and send a message to a Kafka topic.
 *
 * @param rkt A pointer to the Kafka topic handle.
 * @param partition The partition to produce to.
 * @param msgflags Message flags.
 * @param payload The message payload.
 * @param len The length of the message payload.
 * @param key The message key.
 * @param keylen The length of the message key.
 * @param msg_opaque An opaque pointer passed to the delivery report callback.
 * @return 0 on success, or a non-zero error code on failure.
 */
int rd_kafka_produce(rd_kafka_topic_t *rkt, int partition, int msgflags, void *payload, size_t len, const char *key, size_t keylen, void *msg_opaque);

/**
 * @brief Destroy a Kafka topic handle.
 *
 * @param rkt A pointer to the Kafka topic handle to be destroyed.
 */
void rd_kafka_topic_destroy(rd_kafka_topic_t *rkt);

/**
 * @brief Poll the Kafka handle for events.
 *
 * @param rk A pointer to the Kafka handle.
 * @param timeout_ms The maximum time to wait for events, in milliseconds.
 * @return The number of events handled.
 */
int rd_kafka_poll(rd_kafka_t *rk, int timeout_ms);

/**
 * @brief Convert a Kafka error code to a human-readable string.
 *
 * @param err The Kafka error code.
 * @return A human-readable string describing the error.
 */
const char *rd_kafka_err2str(rd_kafka_resp_err_t err);

/**
 * @brief Get the last error code.
 *
 * @return The last Kafka error code.
 */
rd_kafka_resp_err_t rd_kafka_last_error(void);

/**
 * @brief Get the current message count.
 *
 * @return The current message count.
 */
int hook_rd_kafka_get_msg_count(void);

/**
 * @brief Set the current message count.
 *
 * @param msg_count The new message count.
 */
void hook_rd_kafka_set_msg_count(int msg_count);

/**
 * @brief Enables the error mock for different functions
 *
 * @param option It will indicate for which function error should be mocked.
 * @param enable a bool to enable or disable the error mock
 */
void hook_rd_kafka_enable_error(kafka_mock_err_type_t option, bool enable);

/**
 * @brief This function serves as an error callback for handling Kafka-related errors. It is invoked by the Kafka library when an error occurs.
 *
 * @param rk A pointer to the Kafka handle, representing the Kafka instance where the error occurred.
 * @param err The error code indicating the type of error that occurred.
 * @param reason A string providing a human-readable description of the error.
 * @param opaque  A user-defined pointer that can be used to pass additional context or data to the callback.
 */
void hook_rd_kafka_error_callback(rd_kafka_t *rk, int err, const char *reason, void *opaque);

/**
 * @brief function to set the error callback
 *
 * @param conf A pointer to the Kafka configuration object.
 * @param error_callback A function pointer to the error callback function.
 */
void rd_kafka_conf_set_error_cb(rd_kafka_conf_t *conf, error_cb_t error_callback);

/**
 * @brief function to set the opaque parameter
 *
 * @param conf A pointer to the Kafka configuration object.
 * @param opaque A user-defined pointer that can hold any type of data. This pointer will be passed to callback functions set in the Kafka configuration
 */
void rd_kafka_conf_set_opaque(rd_kafka_conf_t *conf, void *opaque);

/**
 * @brief function is used to wait for all outstanding produce requests to be completed.
 *
 * @param rk A pointer to the Kafka handle.
 * @param timeout_ms The maximum time to wait for flush operation, in milliseconds.
 * @return return 0 menas all msg flushed successfully else it will return some error code.
 */
int rd_kafka_flush(rd_kafka_t *rk, int timeout_ms);

/**
 * @brief To clean up and release resources allocated for a Kafka handle.
 *
 * @param rk A pointer to the Kafka handle.
 */
void rd_kafka_destroy(rd_kafka_t *rk);

#endif
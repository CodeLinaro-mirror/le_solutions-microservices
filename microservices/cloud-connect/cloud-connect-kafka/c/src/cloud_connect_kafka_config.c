/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_config.c

DESCRIPTION
    Implementation file for the configuration of Cloud Connect KAFKA service.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>

#include "cloud_connect_kafka_config.h"
#include "cloud_connect_kafka_env_utils.h"
#include "cloud_connect_kafka_logging.h"
#include "cloud_connect_kafka_status.h"

#define CC_CONFIG_FILENAME "config/config.json"

#define KEY_REDIS_CONSUMERS "RedisConsumers"
#define KEY_SOURCE "source"
#define KEY_DEST "dest"
#define KEY_LOG_LEVEL "log_level"
#define KEY_KAFKA_MSG_ERR_HANDLING "msg_err_handling_bool"
#define KEY_KAFKA_CLIENT_ID_PREFIX "client_id_prefix"
#define KEY_ENABLE_SSL_CONNECTION "enable_ssl_connection"
#define KEY_SSL_CONFIG "ssl_config"
#define KEY_ROOT_CA_FILE "root_ca_file"
#define KEY_CLIENT_CERT_FILE "client_cert_file"
#define KEY_PRIVATE_KEY_FILE "client_private_key_file"
#define KEY_CLIENT_CERT_PASSWORD "client_cert_password"
#define KEY_KAFKA_DOMAIN_VALIDATION "domain_validation_enabled_bool"

#define DEFAULT_LOG_LEVEL "DEBUG"
#define DEFAULT_HEART_BEAT_SEC 10
#define DEFAULT_ERR_MSG_HANDELING 1
#define DEFAULT_KAFKA_DOMAIN_VALIDATION 0

/* Define the Keys for all the environment variables */
// Kafka env Keys
#define CCK_KEY_KAFKA_MSG_MAX_BYTES "KAFKA_MESSAGE_MAX_BYTES"
#define KEY_REDIS_HOST "REDIS_HOST"
#define KEY_REDIS_PORT "REDIS_PORT"
#define KEY_KAFKA_HOST "BROKER_HOST"
#define KEY_KAFKA_PORT "BROKER_PORT"

/* Define the default values for all the environment variables */
// Kafka env default values
#define CCK_VAL_KAFKA_MSG_MAX_DEFAULT "10485760" // Setting default max size of kafka message equal to 10MB.
#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_KAFKA_HOST "127.0.0.1"
#define DEFAULT_KAFKA_PORT 9094

cck_sys_config_t g_config;

/**
 * @fn free_config()
 * @brief Function to free the configuration.
 *
 * @return void
 */
static void free_config()
{
    for (int i = 0; i < g_config.channel_len; i++)
    {
        for (int j = 0; j < g_config.channel_mapping[i].num_channels; j++)
        {
            free(g_config.channel_mapping[i].channel_src[j]);
        }
        free(g_config.channel_mapping[i].channel_src);

        for (int j = 0; j < g_config.channel_mapping[i].num_channels; j++)
        {
            free(g_config.channel_mapping[i].channel_dst[j]);
        }
        free(g_config.channel_mapping[i].channel_dst);
    }

    g_config.channel_len = 0;

    if (g_config.channel_mapping)
    {
        free(g_config.channel_mapping);
        g_config.channel_mapping = NULL;
    }

    if (g_config.redis_host)
    {
        free(g_config.redis_host);
        g_config.redis_host = NULL;
    }

    if (g_config.kafka_host)
    {
        free(g_config.kafka_host);
        g_config.kafka_host = NULL;
    }

    if (g_config.log_level)
    {
        free(g_config.log_level);
        g_config.log_level = NULL;
    }

    if (g_config.client_id_prefix)
    {
        free(g_config.client_id_prefix);
        g_config.client_id_prefix = NULL;
    }

    if (g_config.kafka_msg_max_bytes)
    {
        free(g_config.kafka_msg_max_bytes);
        g_config.kafka_msg_max_bytes = NULL;
    }

    if(g_config.ssl_config.client_cert_password)
    {
        free(g_config.ssl_config.client_cert_password);
        g_config.ssl_config.client_cert_password = NULL;
    }

    if(g_config.ssl_config.root_ca_file)
    {
        free(g_config.ssl_config.root_ca_file);
        g_config.ssl_config.root_ca_file = NULL;
    }

    if(g_config.ssl_config.client_cert_file)
    {
        free(g_config.ssl_config.client_cert_file);
        g_config.ssl_config.client_cert_file = NULL;
    }

    if(g_config.ssl_config.client_private_key_file)
    {
        free(g_config.ssl_config.client_private_key_file);
        g_config.ssl_config.client_private_key_file = NULL;
    }
}

/**
 * @fn cloud_connect_read_file()
 * @brief Function to read config file content.
 *
 * @return json-string
 */
static char *cloud_connect_read_file(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (!file)
    {
        LOGE("File opening failed.\n");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *content = malloc(length + 1);
    if (content)
    {
        int total_read = 0;
        int remaining_length = length;
        while (total_read < length)
        {
            int read_size = fread(content + total_read, 1, remaining_length, file);
            if (read_size == 0)
            {
                if (ferror(file))
                {
                    LOGE("Error reading file");
                }
                else
                {
                    LOGE("Could not read complete file of size - %d", length);
                }
                free(content);
                fclose(file);
                return NULL;
            }
            total_read += read_size;
            remaining_length -= read_size;
        }
        content[length] = '\0';
    }

    fclose(file);
    return content;
}

int cloud_connect_kafka_init_config()
{
    int status = CCK_STATUS_FAIL;

    g_config.channel_len = 0;
    g_config.channel_mapping = NULL;
    g_config.redis_host = NULL;
    g_config.kafka_host = NULL;
    g_config.msg_err_handling = 0;
    g_config.client_id_prefix = NULL;
    g_config.ssl_config.root_ca_file = NULL;
    g_config.ssl_config.client_cert_file = NULL;
    g_config.ssl_config.client_private_key_file = NULL;
    g_config.ssl_config.client_cert_password = NULL;
    
    char *data = cloud_connect_read_file(CC_CONFIG_FILENAME);
    if (NULL == data)
    {
        LOGE("Failed to read content from configuration file.\n");
        return CCK_STATUS_FAIL;
    }

    cJSON *json = cJSON_Parse(data);
    if (!json)
    {
        LOGE("Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        free(data);
        return CCK_STATUS_FAIL;
    }

    cJSON *redisConsumers = cJSON_GetObjectItem(json, KEY_REDIS_CONSUMERS);
    if (cJSON_IsNull(redisConsumers) || !cJSON_IsArray(redisConsumers))
    {
        LOGE("Either item '%s' not found in configuration file or not an array.\n", KEY_REDIS_CONSUMERS);
        status = CCK_STATUS_FAIL;
        goto free_resources;
    }

    g_config.channel_len = cJSON_GetArraySize(redisConsumers);
    g_config.channel_mapping = malloc(g_config.channel_len * sizeof(cck_channel_mapping_t));
    memset(g_config.channel_mapping, 0x00, g_config.channel_len * sizeof(cck_channel_mapping_t));

    for (int i = 0; i < g_config.channel_len; i++)
    {
        cJSON *consumer = cJSON_GetArrayItem(redisConsumers, i);
        if (cJSON_IsNull(consumer))
        {
            LOGE("Item '%s' is empty in configuration file.\n", KEY_REDIS_CONSUMERS);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }

        cJSON *source = cJSON_GetObjectItem(consumer, KEY_SOURCE);
        if (cJSON_IsNull(source))
        {
            LOGE("Item '%s' not found under '%s' in configuration file.\n", KEY_SOURCE, KEY_REDIS_CONSUMERS);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }
        cJSON *dest = cJSON_GetObjectItem(consumer, KEY_DEST);
        if (cJSON_IsNull(dest))
        {
            LOGE("Item '%s' not found under '%s' in configuration file.\n", KEY_DEST, KEY_REDIS_CONSUMERS);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }

        g_config.channel_mapping[i].num_channels = cJSON_GetArraySize(source);
        g_config.channel_mapping[i].channel_src = malloc(g_config.channel_mapping[i].num_channels * sizeof(char *));
        memset(g_config.channel_mapping[i].channel_src, 0x00, g_config.channel_mapping[i].num_channels * sizeof(char *));
        for (int j = 0; j < g_config.channel_mapping[i].num_channels; j++)
        {
            if (cJSON_IsString(cJSON_GetArrayItem(source, j)))
            {
                g_config.channel_mapping[i].channel_src[j] = strdup(cJSON_GetArrayItem(source, j)->valuestring);
            }
        }

        g_config.channel_mapping[i].num_channels = cJSON_GetArraySize(dest);
        g_config.channel_mapping[i].channel_dst = malloc(g_config.channel_mapping[i].num_channels * sizeof(char *));
        memset(g_config.channel_mapping[i].channel_dst, 0x00, g_config.channel_mapping[i].num_channels * sizeof(char *));
        for (int j = 0; j < g_config.channel_mapping[i].num_channels; j++)
        {
            if (cJSON_IsString(cJSON_GetArrayItem(dest, j)))
            {
                g_config.channel_mapping[i].channel_dst[j] = strdup(cJSON_GetArrayItem(dest, j)->valuestring);
            }
        }
    }

    if (cJSON_HasObjectItem(json, KEY_LOG_LEVEL) &&
        cJSON_IsString(cJSON_GetObjectItem(json, KEY_LOG_LEVEL)))
    {
        g_config.log_level = strdup(cJSON_GetObjectItem(json, KEY_LOG_LEVEL)->valuestring);
    }
    else
    {
        g_config.log_level = strdup(DEFAULT_LOG_LEVEL);
    }


    if (cJSON_HasObjectItem(json, KEY_KAFKA_MSG_ERR_HANDLING) &&
        cJSON_IsNumber(cJSON_GetObjectItem(json, KEY_KAFKA_MSG_ERR_HANDLING)))
    {
        g_config.msg_err_handling = cJSON_GetObjectItem(json, KEY_KAFKA_MSG_ERR_HANDLING)->valueint;
    }
    else
    {
        g_config.msg_err_handling = DEFAULT_ERR_MSG_HANDELING;
    }

    

    
    if (cJSON_HasObjectItem(json, KEY_KAFKA_CLIENT_ID_PREFIX) && 
        cJSON_IsString(cJSON_GetObjectItem(json, KEY_KAFKA_CLIENT_ID_PREFIX)))
    {
        g_config.client_id_prefix = strdup(cJSON_GetObjectItem(json, KEY_KAFKA_CLIENT_ID_PREFIX)->valuestring);
    }
    else
    {
        LOGI("Item '%s' not found in configuration file.\n", KEY_KAFKA_CLIENT_ID_PREFIX);
        g_config.client_id_prefix = NULL;
    }

    // SSL configuration
    if (cJSON_HasObjectItem(json, KEY_ENABLE_SSL_CONNECTION) &&
        cJSON_IsNumber(cJSON_GetObjectItem(json, KEY_ENABLE_SSL_CONNECTION)))
    {
        g_config.enable_ssl_connection = cJSON_GetObjectItem(json, KEY_ENABLE_SSL_CONNECTION)->valueint;
    }
    else
    {
        g_config.enable_ssl_connection = 0;
    }

    if(g_config.enable_ssl_connection)
    {
        cJSON *ssl_config = cJSON_GetObjectItem(json, KEY_SSL_CONFIG);
        if (cJSON_IsNull(ssl_config) || !cJSON_IsObject(ssl_config))
        {
            LOGE("Either item '%s' not found in configuration file or not an object.\n", KEY_SSL_CONFIG);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }


        if (cJSON_HasObjectItem(ssl_config, KEY_ROOT_CA_FILE) && 
            cJSON_IsString(cJSON_GetObjectItem(ssl_config, KEY_ROOT_CA_FILE)))
        {
            g_config.ssl_config.root_ca_file = strdup(cJSON_GetObjectItem(ssl_config, KEY_ROOT_CA_FILE)->valuestring);
        }
        else
        {
            LOGI("Item '%s' not found in configuration file.\n", KEY_ROOT_CA_FILE);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }

        if (cJSON_HasObjectItem(ssl_config, KEY_CLIENT_CERT_FILE) && 
            cJSON_IsString(cJSON_GetObjectItem(ssl_config, KEY_CLIENT_CERT_FILE)))
        {
            g_config.ssl_config.client_cert_file = strdup(cJSON_GetObjectItem(ssl_config, KEY_CLIENT_CERT_FILE)->valuestring);
        }
        else
        {
            LOGI("Item '%s' not found in configuration file.\n", KEY_CLIENT_CERT_FILE);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }

        if (cJSON_HasObjectItem(ssl_config, KEY_PRIVATE_KEY_FILE) && 
            cJSON_IsString(cJSON_GetObjectItem(ssl_config, KEY_PRIVATE_KEY_FILE)))
        {
            g_config.ssl_config.client_private_key_file = strdup(cJSON_GetObjectItem(ssl_config, KEY_PRIVATE_KEY_FILE)->valuestring);
        }
        else
        {
            LOGI("Item '%s' not found in configuration file.\n", KEY_PRIVATE_KEY_FILE);
            status = CCK_STATUS_FAIL;
            goto free_resources;
        }

        if (cJSON_HasObjectItem(ssl_config, KEY_KAFKA_DOMAIN_VALIDATION) &&
            cJSON_IsNumber(cJSON_GetObjectItem(ssl_config, KEY_KAFKA_DOMAIN_VALIDATION)))
        {
            g_config.ssl_config.is_domain_validation_enabled = cJSON_GetObjectItem(ssl_config, KEY_KAFKA_DOMAIN_VALIDATION)->valueint;
        }
        else
        {
            g_config.ssl_config.is_domain_validation_enabled = DEFAULT_KAFKA_DOMAIN_VALIDATION;
        }

        if (cJSON_HasObjectItem(ssl_config, KEY_CLIENT_CERT_PASSWORD) && 
            cJSON_IsString(cJSON_GetObjectItem(ssl_config, KEY_CLIENT_CERT_PASSWORD)) &&
            strlen(cJSON_GetObjectItem(ssl_config, KEY_CLIENT_CERT_PASSWORD)->valuestring) > 0)
        {
            g_config.ssl_config.client_cert_password = strdup(cJSON_GetObjectItem(ssl_config, KEY_CLIENT_CERT_PASSWORD)->valuestring);
        }
        else
        {
            LOGI("Item '%s' not found or empty in configuration file.\n",KEY_CLIENT_CERT_PASSWORD);
            g_config.ssl_config.client_cert_password = NULL;
        }

    }

    g_config.kafka_msg_max_bytes = strdup(cloud_connect_kafka_get_env_value_str(CCK_KEY_KAFKA_MSG_MAX_BYTES,
                                                                                CCK_VAL_KAFKA_MSG_MAX_DEFAULT));

    g_config.redis_host = strdup(cloud_connect_kafka_get_env_value_str(KEY_REDIS_HOST,
                                                                                DEFAULT_REDIS_HOST));

    g_config.redis_port = cloud_connect_kafka_get_env_value_int(KEY_REDIS_PORT,
                                                                                DEFAULT_REDIS_PORT);

    g_config.kafka_host = strdup(cloud_connect_kafka_get_env_value_str(KEY_KAFKA_HOST,
                                                                                DEFAULT_KAFKA_HOST));

    g_config.kafka_port = cloud_connect_kafka_get_env_value_int(KEY_KAFKA_PORT,
                                                                                DEFAULT_KAFKA_PORT);

    status = CCK_STATUS_SUCCESS;

free_resources:

    cJSON_Delete(json);
    free(data);
    if (status != CCK_STATUS_SUCCESS)
    {
        free_config();
    }

    return status;
}

cck_sys_config_t *cloud_connect_kafka_get_config()
{
    return &g_config;
}

void cloud_connect_kafka_deinit_config()
{
    free_config();
}
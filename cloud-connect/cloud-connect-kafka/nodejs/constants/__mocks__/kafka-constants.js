/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis-constants.js
 * Description :- The file stores the mock constants related to Kafka based interaction for UTs.
 */
'use strict';

export const KafkaConstants = Object.freeze({
  CONFIG_HOST_KEY: 'kafka_host',
  CONFIG_PORT_KEY: 'kafka_port',
  CONNECTION_ID_PREFIX_KEY: 'client_id_prefix', // Prefix to be appened to client id
  CONNECTION_ENABLE_SSL_KEY: 'enable_ssl_connection', // Key to decide whether SSL Connection is enabled or disabled
  CONNECTION_SSL_CONFIG_KEY: 'ssl_config', // SSL Configuration object key.  
  CONNECTION_ROOT_CA_FILE_KEY: 'root_ca_file', // Key containing the path of the CA file generated in the self-signed certificate. It is necessary only when the server uses a self-signed certificate
  CONNECTION_CLIENT_CERT_FILE_KEY: 'client_cert_file', // Key containing the path of the Client certificate. It is necessary only when the server requires client certificate authentication (two-way authentication)
  CONNECTION_CLIENT_KEY_FILE_KEY: 'client_private_key_file', // Key containing the path of the Client key. It is necessary only when the server requires client certificate authentication (two-way authentication)
  CONNECTION_CHECK_HOST_NAME_KEY: 'domain_validation_enabled_bool', // Flag to check hostname
  CONNECTION_CLIENT_CERT_PWD_KEY: 'client_cert_password', // Client Certificate password
  CONNECTION_CLIENT_ID: 'IoTSolutions',
  CONNECTION_HEALTH_CHECK_INTERVAL: 2000, // Interval in milliseconds for sending connection health check's ping message.
  CONNECTION_INITIAL_RETRY_TIME: 100,
  CONNECTION_BACKOFF_MULTIPLIER: 1,
  CONNECTION_RANDOMIZATION_FACTOR: 0.5,
  CONNECTION_MAXIMUM_RETRY_DELAY: 5000, // Maximum 5 seconds delay between connection retrial
  CONNECTION_TIMEOUT_INTERVAL: 10000 // 10 Seconds timeout for connection
})
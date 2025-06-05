/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis-constants.js
 * Description :- The file stores the mock constants related to MQTT based interaction for UTs.
 */
'use strict';

export const MQTTConstants = Object.freeze({
  CONFIG_HOST_KEY: 'mqtt_host',
  CONFIG_PORT_KEY: 'mqtt_port',
  CONNECTION_ID_PREFIX_KEY: 'client_id_prefix', // Prefix to be appened to client id
  CONNECTION_ENABLE_SSL_KEY: 'enable_ssl_connection', // Key to decide whether SSL Connection is enabled or disabled
  CONNECTION_SSL_CONFIG_KEY: 'ssl_config', // SSL Configuration object key.
  CONNECTION_ROOT_CA_FILE_KEY: 'root_ca_file', // Key containing the path of the CA file generated in the self-signed certificate. It is necessary only when the server uses a self-signed certificate
  CONNECTION_CLIENT_CERT_FILE_KEY: 'client_cert_file', // Key containing the path of the Client certificate. It is necessary only when the server requires client certificate authentication (two-way authentication)
  CONNECTION_CLIENT_KEY_FILE_KEY: 'client_private_key_file', // Key containing the path of the Client key. It is necessary only when the server requires client certificate authentication (two-way authentication)
  CONNECTION_CHECK_HOST_NAME_KEY: 'domain_validation_enabled_bool', // Flag to check hostname
  CONNECTION_CLIENT_CERT_PWD_KEY: 'client_cert_password', // Client Certificate password
  CONNECTION_SSL_PROTOCOL: 'mqtts', // SSL Protocol
  CONNECTION_NON_SSL_PROTOCOL: 'mqtt', // Non-SSL Protocol 
  CONNECTION_KEEPALIVE: 30, // Keep alive Interval in seconds at which PINGREQ is sent in case of idle connection.
  CONNECTION_PROTCOL_VERSION: 4, // Can be set to 3 (v3.1), 4 (v3.1.1) and 5 (v5.0)
  CONNECTION_TIMEOUT: 15000, // 15 seconds timeout declared in milliseconds
  CONNECTION_RECONNECT_PERIOD: 5000, // 5 seconds interval before retrying connection.
  CONNECTION_RECONNECT_ON_CONNACK_ERROR: true, // Whether to try reconnect in case error is received with CONNACK
})
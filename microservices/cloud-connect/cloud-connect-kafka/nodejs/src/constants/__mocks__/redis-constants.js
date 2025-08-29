/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis-constants.js
 * Description :- The file stores the mock constants related to Redis based interaction for UTs.
 */
'use strict';

export const RedisConstants = Object.freeze({
  CONFIG_CONSUMERS_KEY: 'RedisConsumers', // JSON Key for reading list of Redis and respective MQTT channels
  CONFIG_SOURCE_TOPIC_KEY: 'source',
  CONFIG_DESTINATION_TOPIC_KEY: 'dest',
  CONFIG_HOST_KEY: 'redis_host',
  CONFIG_PORT_KEY: 'redis_port',
  CONFIG_PASSWORD_KEY: 'redis_password',
  CONNECTION_RETRY_INTERVAL: 5000, // 5 seconds delay between connection retry
  CONNECTION_TIMEOUT: 15000, // 15 seconds connection timeout declared in milliseconds
})
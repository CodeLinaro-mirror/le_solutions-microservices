/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- kafka-constants.test.js
 * Description :- The test file to validate the Defined Kafka Constants.
 */
'use strict';

import { KafkaConstants } from "../../../src/constants/kafka-constants.js";

// Test Suite for all Kafka Constants Related Test Cases
describe('Kafka Constants', () => {
    // Validate the defined Kafka Constants
    it("Validates all Kafka constants are defined properly", async () => {
        expect(KafkaConstants.CONFIG_HOST_KEY).toBe('kafka_host');
        expect(KafkaConstants.CONFIG_PORT_KEY).toBe('kafka_port');
        expect(KafkaConstants.CONNECTION_ID_PREFIX_KEY).toBe('client_id_prefix');
        expect(KafkaConstants.CONNECTION_ENABLE_SSL_KEY).toBe('enable_ssl_connection');
        expect(KafkaConstants.CONNECTION_SSL_CONFIG_KEY).toBe('ssl_config');
        expect(KafkaConstants.CONNECTION_ROOT_CA_FILE_KEY).toBe('root_ca_file');
        expect(KafkaConstants.CONNECTION_CLIENT_CERT_FILE_KEY).toBe('client_cert_file');
        expect(KafkaConstants.CONNECTION_CLIENT_KEY_FILE_KEY).toBe('client_private_key_file');
        expect(KafkaConstants.CONNECTION_CHECK_HOST_NAME_KEY).toBe('domain_validation_enabled_bool');
        expect(KafkaConstants.CONNECTION_CLIENT_CERT_PWD_KEY).toBe('client_cert_password');
        expect(KafkaConstants.CONNECTION_CLIENT_ID).toBe('IoTSolutions');
        expect(KafkaConstants.CONNECTION_HEALTH_CHECK_INTERVAL).toBeDefined();
        expect(KafkaConstants.CONNECTION_INITIAL_RETRY_TIME).toBeDefined();
        expect(KafkaConstants.CONNECTION_BACKOFF_MULTIPLIER).toBeDefined();
        expect(KafkaConstants.CONNECTION_RANDOMIZATION_FACTOR).toBeDefined();
        expect(KafkaConstants.CONNECTION_MAXIMUM_RETRY_DELAY).toBeDefined();
        expect(KafkaConstants.CONNECTION_TIMEOUT_INTERVAL).toBeDefined();
    });
});
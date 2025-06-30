/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- mqtt-constants.test.js
 * Description :- The test file to validate the Defined MQTT Constants.
 */
'use strict';

import { MQTTConstants } from "../../../src/constants/mqtt-constants.js";

// Test Suite for all MQTT Constants Related Test Cases
describe('MQTT Constants', () => {
    // Validate the defined MQTT Constants
    it("Validates all MQTT constants are defined properly", async () => {
        expect(MQTTConstants.CONFIG_HOST_KEY).toBe('mqtt_host');
        expect(MQTTConstants.CONFIG_PORT_KEY).toBe('mqtt_port');
        expect(MQTTConstants.CONNECTION_ID_PREFIX_KEY).toBe('client_id_prefix');
        expect(MQTTConstants.CONNECTION_ENABLE_SSL_KEY).toBe('enable_ssl_connection');
        expect(MQTTConstants.CONNECTION_SSL_CONFIG_KEY).toBe('ssl_config');
        expect(MQTTConstants.CONNECTION_ROOT_CA_FILE_KEY).toBe('root_ca_file');
        expect(MQTTConstants.CONNECTION_CLIENT_CERT_FILE_KEY).toBe('client_cert_file');
        expect(MQTTConstants.CONNECTION_CLIENT_KEY_FILE_KEY).toBe('client_private_key_file');
        expect(MQTTConstants.CONNECTION_CHECK_HOST_NAME_KEY).toBe('domain_validation_enabled_bool');
        expect(MQTTConstants.CONNECTION_CLIENT_CERT_PWD_KEY).toBe('client_cert_password');
        expect(MQTTConstants.CONNECTION_SSL_PROTOCOL).toBeDefined();
        expect(MQTTConstants.CONNECTION_NON_SSL_PROTOCOL).toBeDefined();
        expect(MQTTConstants.CONNECTION_KEEPALIVE).toBe(60);
        expect(MQTTConstants.CONNECTION_PROTCOL_VERSION).toBeDefined();
        expect(MQTTConstants.CONNECTION_TIMEOUT).toBeDefined();
    });
});
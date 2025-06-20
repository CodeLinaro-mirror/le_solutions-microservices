/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis-constants.test.js
 * Description :- The test file to validate the Defined Redis Constants.
 */
'use strict';

import { RedisConstants } from "../../constants/redis-constants.js";

// Test Suite for all Redis Constants Related Test Cases
describe('Redis Constants', () => {
    // Validate the defined Redis Constants
    it("Validates all Redis constants are defined properly", async () => {
        expect(RedisConstants.CONFIG_CONSUMERS_KEY).toBe('RedisConsumers');
        expect(RedisConstants.CONFIG_SOURCE_TOPIC_KEY).toBe('source');
        expect(RedisConstants.CONFIG_DESTINATION_TOPIC_KEY).toBe('dest');
        expect(RedisConstants.CONFIG_HOST_KEY).toBe('redis_host');
        expect(RedisConstants.CONFIG_PORT_KEY).toBe('redis_port');
        expect(RedisConstants.CONFIG_PASSWORD_KEY).toBe('redis_password');
        expect(RedisConstants.CONNECTION_RETRY_INTERVAL).toBeDefined();
        expect(RedisConstants.CONNECTION_TIMEOUT).toBeDefined();
    });
});
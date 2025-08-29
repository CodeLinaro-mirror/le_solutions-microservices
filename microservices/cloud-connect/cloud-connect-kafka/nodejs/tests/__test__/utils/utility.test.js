/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- utility.test.js
 * Description :- The test file to validate Utility related functionality.
 */
'use strict';

import { parseJsonFile, retrieveTopicNameToPublish, ValidationError } from "../../../src/utils/utility.js";

// Test Suite for all Utility Related Test Cases
describe('Utility Functions', () => {
    // Validate the custom Validation Error
    it("Validates an instance of Validation Error", async () => {
        var customValidationError = new ValidationError('Test custom validation error.');
        expect(customValidationError).toBeInstanceOf(Error);
        expect(customValidationError).toBeInstanceOf(ValidationError);
        expect(customValidationError.message).toBe('Test custom validation error.');
        expect(customValidationError.name).toBe('ValidationError');
    });

    // Validate the Utility's parseJSON Function
    it("Validates parseJsonFile function", async () => {
        var readJSONObject = parseJsonFile('tests/__test__/sample_file.json');
        expect(readJSONObject['Key1'].length).toBe(2);
        expect(readJSONObject['Key1'][0]['Key1SubKey1'][0]).toBe('Value1');
        expect(readJSONObject['Key1'][0]['Key1SubKey2'][0]).toBe('Value2');
        expect(readJSONObject['Key1'][1]['Key1SubKey1'][0]).toBe('Value3');
        expect(readJSONObject['Key1'][1]['Key1SubKey1'][1]).toBe('Value4');
        expect(readJSONObject['Key1'][1]['Key1SubKey2'][0]).toBe('Value5');
        expect(readJSONObject['Key1'][1]['Key1SubKey2'][1]).toBe('Value6');
        expect(readJSONObject['Key2']).toBe(0);
        expect(readJSONObject['Key3']).toBe('Value7');
    });

    // Validate the Utility's retrieveTopicNameToPublish function for topic name without *
    it("Validates retrieveTopicNameToPublish function for topic name without *", async () => {
        var retrievedTopicName =
            retrieveTopicNameToPublish('detection:channel:1', 'detection.channel.kafka.1', 'detection:channel:1');
        expect(retrievedTopicName).toBe('detection.channel.kafka.1');
    });

    // Validate the Utility's retrieveTopicNameToPublish function for topic name with *
    it("Validates retrieveTopicNameToPublish function for topic name with *", async () => {
        var retrievedTopicName =
            retrieveTopicNameToPublish('detection:ppe:1', 'detection.ppe.kafka.*', 'detection:ppe:*');
        expect(retrievedTopicName).toBe('detection.ppe.kafka.1');
    });
});
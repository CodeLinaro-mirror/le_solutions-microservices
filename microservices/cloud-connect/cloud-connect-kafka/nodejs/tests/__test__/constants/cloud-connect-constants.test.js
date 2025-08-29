/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect-constants.test.js
 * Description :- The test file to validate the Defined Cloud Connect Service Constants.
 */
'use strict';

import { CloudConnectConstants } from "../../../src/constants/cloud-connect-constants.js";

// Test Suite for all Cloud Connect Service Constants Related Test Cases
describe('Cloud Connect Service Constants', () => {
    // Validate the defined Cloud Connect Service Constants
    it("Validates all Cloud Connect constants are defined properly", async () => {
        expect(CloudConnectConstants.CONFIG_FILE_NAME).toBe('config/config.json');
        expect(CloudConnectConstants.MESSAGE_ERROR_HANDLE_FLAG_KEY).toBe('msg_err_handling_bool');
        expect(CloudConnectConstants.CLIENT_PREFIX_MAX_ALLOWED_LENGTH).toBe(10);
    });
});
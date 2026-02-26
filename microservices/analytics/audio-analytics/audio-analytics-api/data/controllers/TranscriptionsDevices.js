/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.listTranscriptionDevices = async function listTranscriptionDevices (req, res, next) {
    try {
        console.log('ASR devices request received');

        const body = {
            message_type: 'asr_devices'
        };

        messages.publishAndListenOnce(config.asrDevices, config.asrDevices, body, (err, data) => {
            if (err) {
                res.status(400).json({
                    error: {
                        message: data.message,
                        type: 'server_error',
                        param: null,
                        code: null
                    }
                });
            } else {
                console.log('Received ASR devices from server');
                res.status(200).json(data.result || data);
            }
        });
    } catch (e) {
        console.error('ASR devices error:', e);
        res.status(500).json({
            error: {
                message: e.message,
                type: 'server_error',
                param: null,
                code: null
            }
        });
    }
};

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.listTTSDevices = async function listTTSDevices (req, res, next) {
    try {
        console.log('TTS devices request received');

        const body = {
            message_type: 'tts_devices'
        };

        messages.publishAndListenOnce(config.ttsDevices, config.ttsDevices, body, (err, data) => {
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
                console.log('Received TTS devices from server');
                res.status(200).json(data.result || data);
            }
        });
    } catch (e) {
        console.error('TTS devices error:', e);
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

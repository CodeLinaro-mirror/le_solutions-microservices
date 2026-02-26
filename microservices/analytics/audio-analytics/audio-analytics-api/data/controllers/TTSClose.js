/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.closeTTS = async function closeTTS (req, res, next, body) {
    try {
        console.log('Close tts request received');

        messages.publish(config.ttsTextIn, {message_type: 'tts_close'}, ()=>{});
        res.status(200).send('Text-to-Speech session successfully closed.');
    } catch (e) {
        console.error('Close tts error:', e);
        res.status(500).json({
            error: {
                message: e.message,
                type: "server_error",
                param: null,
                code: null
            }
        });
    }
};

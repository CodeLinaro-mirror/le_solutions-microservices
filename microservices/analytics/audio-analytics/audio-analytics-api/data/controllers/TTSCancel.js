/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.cancelTTS = async function cancelTTS (req, res, next, body) {
    try {
        console.log('Cancel TTS request received');

        messages.publish(config.ttsTextIn, {message_type: 'tts_cancel'}, ()=>{});
        res.status(200).send('Text-to-Speech session successfully cancelled.');
    } catch (e) {
        console.error('Cancel tts error:', e);
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

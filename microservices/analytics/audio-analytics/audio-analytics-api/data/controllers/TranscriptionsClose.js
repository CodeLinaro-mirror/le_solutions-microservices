/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.closeTranscription = async function closeTranscription (req, res, next, body) {
    try {
        console.log('Close transcription request received');
        const sessionId = (body && body.session_id) || null;

        messages.publishAndListenOnce(config.asrTranscriptionIn, config.asrTranscriptionOut,
            {message_type: 'transcriptions_close', session_id: sessionId},
            (err, data) => {
                console.log(`[close] callback received — err=${err}, data=${JSON.stringify(data)}`);
                if (res.headersSent) return;
                if (err) {
                    return res.status(500).json({
                        error: { message: (data && data.message) || 'Close failed', type: 'server_error' }
                    });
                }
                return res.status(200).json({
                    session_id: sessionId,
                    state: (data && data.state) || 'asr_closed'
                });
            }
        );
    } catch (e) {
        console.error('Close transcription error:', e);
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

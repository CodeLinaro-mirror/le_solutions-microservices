/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.flushTranscription = async function flushTranscription (req, res, next) {
    try {
        const body = req.body || {};
        const session_id = body.session_id || null;

        console.log('Flush transcription request received, session_id:', session_id);

        const payload = {
            message_type: 'transcriptions_flush',
            session_id: session_id
        };

        messages.publish(config.asrTranscriptionIn, payload, (err) => {
            if (err) {
                return res.status(500).json({
                    error: { message: 'Failed to send flush request', type: 'server_error' }
                });
            }
            return res.status(200).json({ status: 'flushed' });
        });
    } catch (e) {
        console.error('Flush transcription error:', e);
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

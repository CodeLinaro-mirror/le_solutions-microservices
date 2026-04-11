/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');
const utils = require('../utils/utils');
const fs = require('fs');

module.exports.synthesizeSpeech = async function synthesizeSpeech (req, res, next, body) {
    try {
        const MAX_TTS_CHAR_SIZE = 1024;

        console.log('TTS synthesis request received');
        console.log(body);
        
        // Extract parameters from body
        const text = body.text;
        const model = body.model || 'tts-model-1';
        const language = (body.language || 'en').toLowerCase();
        const voice = body.voice || 'default';
        const speaker = body.output_speaker || false;
        const override_max_chars_check = body.override_max_chars_check || false;
        const gender = body.gender;
        const style = body.style;
        const sample_rate = body.sample_rate;
        const parameters = body.parameters || {};
        
        // Validate required fields
        if (!text) {
            return res.status(400).json({
                error: {
                    message: "Text is required",
                    type: "invalid_request_error",
                    param: "text",
                    code: null
                }
            });
        }

        const text_len = text.length;
        if (!override_max_chars_check && text_len >= MAX_TTS_CHAR_SIZE) {
            return res.status(400).json({
                error: {
                    message: `Text exceeds maximum length of ${MAX_TTS_CHAR_SIZE} characters. Current length: ${text_len}. Use 'override_max_chars_check: true' to bypass this limit.`,
                    type: "invalid_request_error",
                    param: "text",
                    code: "text_too_long"
                }
            });
        }

        // Generate a unique sync_id for this request so the handler can ignore
        // audio chunks and done-messages that belong to other requests.
        const { randomUUID } = require('crypto');
        const syncId = randomUUID();
        body.sync_id = syncId;
        body.message_type = 'tts_synthesize';

        console.log('   Text:', text.substring(0, 50) + (text.length > 50 ? '...' : ''));
        console.log('   Text length:', text_len);
        console.log('   Model:', model);
        console.log('   Language:', language);
        console.log('   Voice:', voice);
        console.log('   Speaker:', speaker);
        console.log('   syncId:', syncId);

        // Define the handler so we can unsubscribe it by reference when done
        const audioHandler = (err, data, msg) => {
            // If the response is already finished, unsubscribe and bail out
            if (res.writableEnded) {
                messages.unsubscribeFromChannel(config.ttsAudioOut, audioHandler);
                return;
            }

            // All messages from the server are JSON objects:
            //   Audio chunk: { type: 'audio', data: '<base64>', sync_id: '...' }
            //   Done:        { status: 'done', sync_id: '...' }
            //   Error:       { error: {...}, sync_id: '...' }
            try {
                const parsed = JSON.parse(msg);

                // Ignore messages that belong to a different request
                if (parsed.sync_id && parsed.sync_id !== syncId) {
                    return;
                }

                if (parsed.status === 'done') {
                    console.log('TTS synthesis complete, closing response');
                    messages.unsubscribeFromChannel(config.ttsAudioOut, audioHandler);
                    return res.end();
                } else if (parsed.type === 'audio' && parsed.data) {
                    //console.info("new envelope");
                    // New envelope format: { type: 'audio', data: '<base64>', sync_id }
                    const buf = Buffer.from(parsed.data, 'base64');
                    res.write(buf);
                } else if (typeof parsed === 'string') {
                    //console.info("legacy")
                    // Legacy fallback: bare base64 string (should not occur with updated server)
                    const buf = Buffer.from(parsed, 'base64');
                    res.write(buf);
                }
                // Any other object (e.g. error without matching sync_id already filtered) — ignore
            } catch (e) {
                // JSON.parse failed — treat as raw base64 (legacy redis mode fallback)
                const buf = Buffer.from(msg, 'base64');
                res.write(buf);
            }
        };

        // Unsubscribe if the client disconnects before synthesis completes
        res.on('close', () => {
            messages.unsubscribeFromChannel(config.ttsAudioOut, audioHandler);
        });

        // Subscribe BEFORE publishing the request — prevents the race condition
        // where the server publishes the first audio chunk before the handler
        // is registered, causing the first chunk to be missed (heard as static).
        messages.listenToChannel(config.ttsAudioOut, {res}, audioHandler);

        // Now publish the TTS request to start server-side synthesis
        messages.publish(config.ttsTextIn, body, () => {});



    } catch (e) {
        console.error('TTS synthesis error:', e);
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

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.translateText = async function translateText (req, res, next, body) {
    try {
        console.log('Translation request received');

        // Extract parameters from body
        const text = body.text;
        const model = body.model || 'translation-model-1';
        const source_language = body.source_language?.toLowerCase();
        const target_language = body.target_language?.toLowerCase();
        const parameters = body.parameters || {};
        
        // Validate required fields
        if (!text || !Array.isArray(text) || text.length === 0) {
            return res.status(400).json({
                error: {
                    message: "Text array is required and must not be empty",
                    type: "invalid_request_error",
                    param: "text",
                    code: null
                }
        });
        }
        
        if (!source_language || !target_language) {
            return res.status(400).json({
                error: {
                    message: "Source and target languages are required",
                    type: "invalid_request_error",
                    param: "language",
                    code: null
                }
            });
        }
        
        console.log('   Text count:', text.length);
        console.log('   Model:', model);
        console.log('   Source:', source_language, '-> Target:', target_language);

        body.message_type = 'translation_request';

        // publish the JSON to redis
        messages.publishAndListenOnce(config.t2tTranslationIn, config.t2tTranslationOut, body, (err, data) => {
            if (err) {
                // Return the server's error payload so clients can see the reason
                return res.status(400).json({ error: data });
            } else {
                // Output Response
                return res.status(200).json(data);
            }
        });

    } catch (e) {
        console.error('Translation error:', e);
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
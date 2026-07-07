/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');
const crypto = require('crypto');

module.exports.listTTSModels = async function listTTSModels (req, res, next, body) {
    try {
        console.log('🔊 TTS models request received');
        
        // Get language filter from query
        const language = req.query.language;
        
        // Create request message
        let body = {
            message_type: 'models_list',
            language: language
        };
        
         // handle message
        messages.publishAndListenOnce(config.ttsModels, config.ttsModels, body, (err, data) => {
            if (err) {
                res.status(400).json({
                    error: {
                        message: data.message,
                        type: "server_error",
                        param: null,
                        code: null
                    }
                });
            } else {
                console.log('Received models from server');
                // Respond with the models
                res.status(200).json(data);
            }
        });
    } catch (e) {
        console.error('TTS models error:', e);
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

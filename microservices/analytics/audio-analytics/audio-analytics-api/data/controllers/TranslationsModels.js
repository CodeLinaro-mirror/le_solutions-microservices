/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');
const crypto = require('crypto');

module.exports.listTranslationModels = async function listTranslationModels (req, res, next, body) {
    try {
        console.log('T2T models request received');
        
        // Create request message
        let body = {
            message_type: 'models_list'
        };
        
        // handle message
        messages.publishAndListenOnce(config.t2tModels, config.t2tModels, body, (err, data) => {
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
        console.error('T2T models error:', e);
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

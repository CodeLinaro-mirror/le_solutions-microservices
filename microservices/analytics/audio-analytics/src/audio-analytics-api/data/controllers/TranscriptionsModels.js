/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';
// TODO: Models should return the expected results json from the server.
// TODO: If sockets should use the .sock file
// TODO: Error Messages for all messages should be handled. // Logic should be in the utils/messages or utils/redis
const config = require('../config/config');
const messages = require('../utils/messages');


module.exports.listTranscriptionModels = async function listTranscriptionModels (req, res, next, body) {
    try {
        console.log('ASR models request received');
        
        let body = {
            "message_type": 'models_list'
        };

        // handle message
        messages.publishAndListenOnce(config.asrModels, config.asrModels, body, (err, data) => {
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
                // Respond with the models (data.result contains the array)
                res.status(200).json(data.result || data);
            }
        });
    } catch (e) {
        console.error('ASR models error:', e);
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

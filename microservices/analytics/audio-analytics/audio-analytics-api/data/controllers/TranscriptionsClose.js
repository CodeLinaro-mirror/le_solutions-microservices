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


        // // Extract session ID from body if provided
        // const session_id = body.session_id || crypto.randomUUID();
        
        // // Create close message
        // const request = {
        //     message_type: 'transcriptions_close',
        //     session_id: session_id
        // };
        
        // if (config.blackboxContainer) {
        //     // Use Unix socket
        //     console.log('Sending close via Unix socket');
        //     const indexModule = require('../index');
        //     const socketClient = indexModule.getSocketClient();
            
        //     if (!socketClient) {
        //         throw new Error('Socket client not initialized');
        //     }
            
        //     const response = await socketClient.sendRequest(config.asrTranscriptionIn, request);
        //     console.log('Session closed');
        //     res.status(200).json({ status: "closed" });
        // } else {
        //     // Use Redis
        //     console.log('Publishing close to Redis channel:', config.asrTranscriptionIn);
        //     await redis.publish(config.asrTranscriptionIn, request);
            
        //     res.status(200).json({ status: "closed" });
        // }




        messages.publish(config.asrTranscriptionIn, {message_type: 'transcriptions_close', session_id: body.session_id}, ()=>{});
        res.status(200).send('Transcription session successfully closed.');
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

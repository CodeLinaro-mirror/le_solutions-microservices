/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const { asrTranscriptionIn } = require('../config/config');
const config = require('../config/config');
const messages = require('../utils/messages');
const utils = require('../utils/utils');

module.exports.createTranscription = async function createTranscription (req, res, next, body) {
    try {
        console.log('Transcription request received');
        
        // Extract parameters from body
        const model = body.model || 'whisper-1';
        const language = body.language || null;
        const stream = body.stream === 'true' || body.stream === true || false;
        const parameters = body.parameters
            ? (typeof body.parameters === 'string' ? JSON.parse(body.parameters) : body.parameters)
            : [];

        console.log('   Model:', model);
        console.log('   Language:', language);
        console.log('   Stream:', stream);

        // Clear the previous transcription messages if any
        messages.clearQueue();

        // Check to see if a file was uploaded
        if (req.files && req.files.length != 0) {
            let file = req.files[0];
            body.stream = body.stream == 'true' ? true : false;
            body.message_type = 'transcriptions_create';

            // Add filename to the request
            body.filename = file.originalname || file.name || 'audio_file';
            console.log('   Filename:', body.filename);

            // Write File to temp storage
            await utils.writeLocalFile(file);

            // Convert Audio to base64Encoded stream
            body.file = await utils.readFileAsBase64(file);

            // Only Publish if on stream is enabled
            if (body.stream || body.stream == 'true') {
                // Send Encoded Stream as message to message broker
                // If model takes a long time to initialize, then do not respond to USER until an ACK is received
                messages.publishAndListenOnce(config.asrTranscriptionIn, config.asrTranscriptionOut, body, (err, data) => {
                    if (err) {return res.status(data && data.code === 'conflict' ? 409 : 400).json({error: {message: (data && data.message) || 'Transcription failed', type: (data && data.type) || 'server_error', code: (data && data.code) || null, sessions: (data && data.sessions) || undefined}});}
                    else {
                        return res.status(200).json({
                            session_id: data.session_id || "no_session_id",
                            text: "File Uploaded. Listen on WebSocket to get transcription output.",
                            language: "en",
                            type: "transcript.text.delta"
                        });
                    }
                });
            }
            // Blocking synchronous call. Waiting for the specific message to be sent back
            else {
                // Send Encoded Stream as message to message broker
                // Listen for Response from message broker
                messages.publishAndListenOnce(config.asrTranscriptionIn, config.asrTranscriptionOut, body, (err, data) => {
                    if (err) {res.status(400).json({error: {message: (data && data.message) || 'Transcription failed', type: 'server_error'}});}
                    else {
                        // Output Response in WebSocket if stream is enabled
                        res.status(200).json(data);

                        // Send message to Server to CLOSE Session/Deinitialize???
                        messages.publish(config.asrTranscriptionIn, {message_type: 'transcriptions_close'}, ()=>{});
                    }
                });
            }
        }
        // Special case using online microphone
        // TODO: Determine if this should stay in actual release. If not remove before pushing to main for publishing
        else if (parameters != null && parameters.some(obj => {return obj.key == 'on-device-microphone'})) {
            // Mock Data
            // Add message to message queue
            // messages.addMsgToQueue({
            //     result: {
            //         session_id: "session123",
            //         text: "The initial text. ",
            //         language: "en",
            //         type: "transcript.text.delta"
            //     }
            // });
            // messages.addMsgToQueue({
            //     result: {
            //         session_id: "session123",
            //         text: "The final text.",
            //         language: "en",
            //         type: "transcript.text.done"
            //     }
            // });

            // res.status(200).json({
            //     session_id: "session123",               // TODO: Fake session_id, Use the real one 
            //     text: "Successfully started Transcription Engine. Please connect to the WebSocket to send audio data & receive transcription output.",
            //     language: "en",
            //     type: "transcript.text.delta"
            // });
            // TODO: Connect to server for real data
            body.file = null;
            body.message_type = 'transcriptions_create';
            // If model takes a long time to initialize, then do not respond to USER until an ACK is received
            messages.publishAndListenOnce(config.asrTranscriptionIn, config.asrTranscriptionOut, body, (err, data) => {
                if (err) {return res.status(data && data.code === 'conflict' ? 409 : 400).json({error: {message: (data && data.message) || 'Transcription failed', type: (data && data.type) || 'server_error', code: (data && data.code) || null, sessions: (data && data.sessions) || undefined}});}
                else {
                    return res.status(200).json({
                        session_id: data.session_id || "session123",
                        state: data.state || undefined,
                        text: "Successfully started Transcription Engine. Please connect to the WebSocket to send audio data & receive transcription output.",
                        language: "en",
                        type: "transcript.text.delta"
                    });
                }
            });

        } else {
            // Streaming input and output case
            // Send message to initialize model
            body.file = null;
            body.message_type = 'transcriptions_create';
            // If model takes a long time to initialize, then do not respond to USER until an ACK is received
            messages.publishAndListenOnce(config.asrTranscriptionIn, config.asrTranscriptionOut, body, (err, data) => {
                if (err) {return res.status(data && data.code === 'conflict' ? 409 : 400).json({error: {message: (data && data.message) || 'Transcription failed', type: (data && data.type) || 'server_error', code: (data && data.code) || null, sessions: (data && data.sessions) || undefined}});}
                else {
                    return res.status(200).json({
                        session_id: data.session_id || "session123",
                        state: data.state || undefined,
                        text: "Successfully started Transcription Engine. Please connect to the WebSocket to send audio data & receive transcription output.",
                        language: "en",
                        type: "transcript.text.delta"
                    });
                }
            });
        }
    } catch (e) {
        console.error('Transcription error:', e);
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

messages.listenToChannel(config.asrTranscriptionOut, null, (err, inData, jsonData) => {
    try {
        const indexModule = require('../index');
        const ws = indexModule.getWebSocketServer();

        if (err) {
            let data = JSON.parse(jsonData);
            console.error(data);
            // Send Error message to all clients listening
            ws.clients.forEach(client => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify(data.result))
                }
            })
        } else {
            // Data received on message broker. Stream out data to WebSocket as it comes in
            let data = JSON.parse(jsonData);

            // Ignore messages sent by myself to the server
            if (data.message_source != 'audio_analytics_api') {
                // Only forward actual transcription messages to WebSocket clients
                const result = data.result || {};
                const isTranscriptionMessage = 
                    result.type === 'transcript.text.delta' 
                    || result.type === 'transcript.text.done'
                    || result.type === 'transcript.event';
                
                if (isTranscriptionMessage) {
                    console.log(`Forwarding transcription message: ${result.type}, text: "${(result.text || '').substring(0, 50)}..."`);
                    
                    // IF NO CLIENTS, THEN QUEUE THE DATA UNTIL A CLIENT IS CONNECTED. THEN SEND IT ALL AT ONCE
                    if(ws.clients.size == 0 && data.stream) {
                        messages.addMsgToQueue(data);
                    } else {
                        // Send message to all clients listening
                        ws.clients.forEach(client => {
                            if (client.readyState === WebSocket.OPEN) {
                                client.send(JSON.stringify(data.result));
                                indexModule.resetInactivityTimer(client);
                            }
                        })
                    }
                } else {
                    console.log(`Ignoring non-transcription message: ${result.type || 'unknown'}, text: "${(result.text || '').substring(0, 50)}..."`);
                }
            }
        }
    } catch (e) {
        console.error(e.message);
    }
});

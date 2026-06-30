/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('./redis');
const SocketClient = require('./socket-client');

const msgQueue = [];
let globalSocketClient = null;

async function initializeCommunication() {
    if (config.blackboxContainer) {
        console.log('🔌 Initializing BLACKBOX mode (Unix Socket)');
        console.log('   Socket path:', config.socketPath);
        globalSocketClient = new SocketClient(config.socketPath);
        try {
            await globalSocketClient.connect();
            console.log('✅ Socket client connected');
            return globalSocketClient;
        } catch (err) {
            console.error('❌ Failed to connect to socket:', err.message);
            console.log('   Server may not be ready yet, will retry on first request');
            return globalSocketClient;
        }
    } else {
        console.log('📡 Initializing STANDARD mode (Redis)');
        console.log('   Redis host:', config.redisHost);
        console.log('   Redis port:', config.redisPort);
        return null;
    }
}

async function ensureSocketConnected() {
    if (!globalSocketClient) return false;
    if (globalSocketClient.connected) return true;

    // If the client has exhausted its reconnect attempts, reset and try again
    if (globalSocketClient.reconnectAttempts >= globalSocketClient.maxReconnectAttempts) {
        console.log('🔄 Socket client exhausted retries, resetting connection...');
        globalSocketClient.reconnectAttempts = 0;
        globalSocketClient.reconnectTimer = null;
    }

    try {
        await globalSocketClient.connect();
        return globalSocketClient.connected;
    } catch (err) {
        console.error('❌ Socket reconnect failed:', err.message);
        return false;
    }
}

async function publishAndListenOnce (channelIn, channelOut, data, cb) {
    // Insert a message_source to signal that the message is from here
    data.message_source = 'audio_analytics_api';

    if (!config.blackboxContainer) {
        redis.publishAndListenOnce(channelIn, channelOut, data, cb);
    } else {
        // In blackbox mode, implement request-response pattern via Unix socket
        console.log('Publishing via Unix socket to channel:', channelIn);
        
        if (!globalSocketClient) {
            return cb(true, {message: 'Socket client not initialized'});
        }
        
        if (!(await ensureSocketConnected())) {
            return cb(true, {message: 'Socket server not available'});
        }

        try {
            const { randomUUID } = require('crypto');
            const sync_id = randomUUID();
            data.sync_id = sync_id;
            
            // Set up a one-time listener for the response
            const responseHandler = (err, _, message) => {
                try {
                    console.log(`[${sync_id}] Received message on ${channelOut}:`, message.substring(0, 200) + '...');
                    const retData = JSON.parse(message);

                    console.log(`[${sync_id}] Parsed response sync_id: ${retData.sync_id}, expected: ${sync_id}`);
                    console.log(`[${sync_id}] Response keys:`, Object.keys(retData));

                    // Check if this is the response we're waiting for
                    if (retData.sync_id === sync_id) {
                        console.log(`[${sync_id}] ✅ Sync ID matches, processing response`);

                        // Clear the timeout
                        if (responseHandler.timeout) {
                            clearTimeout(responseHandler.timeout);
                        }

                        // Unsubscribe this specific handler after receiving the response
                        globalSocketClient.unsubscribe(channelOut, responseHandler);

                        // Call the callback with the result — do NOT wrap in try/catch so
                        // errors in cb don't re-invoke cb via the outer catch block
                        if (retData.error) {
                            console.log(`[${sync_id}] ❌ Response has error:`, retData.error);
                            responseHandler.cbCalled = true;
                            cb(true, retData.result);
                        } else if (retData.result !== undefined) {
                            console.log(`[${sync_id}] ✅ Response has result, calling callback`);
                            responseHandler.cbCalled = true;
                            cb(false, retData.result);
                        } else {
                            console.log(`[${sync_id}] ⚠️ Response has no result field:`, retData);
                            responseHandler.cbCalled = true;
                            cb(false, retData);
                        }
                    } else {
                        console.log(`[${sync_id}] ⏭️ Sync ID mismatch, ignoring message`);
                    }
                } catch (e) {
                    console.error(`[${sync_id}] Error processing socket response:`, e.message);
                    // Only call cb on parse/internal errors, not on errors thrown by cb itself
                    if (!responseHandler.cbCalled) {
                        cb(true, {message: e.message});
                    }
                }
            };
            
            // Subscribe to the output channel before publishing
            globalSocketClient.subscribe(channelOut, responseHandler);
            
            // Publish the request
            await globalSocketClient.publish(channelIn, data);
            
            console.log(`[${sync_id}] Published request to ${channelIn}, waiting for response on ${channelOut}`);
            
            // Add a timeout to prevent hanging indefinitely
            const timeout = setTimeout(() => {
                console.log(`[${sync_id}] ⏰ Request timeout after 60 seconds`);
                globalSocketClient.unsubscribe(channelOut, responseHandler);
                cb(true, {message: 'Request timeout'});
            }, 300000); // 300 second timeout
            
            // Store timeout reference so we can clear it when response arrives
            responseHandler.timeout = timeout;
        } catch (err) {
            console.error('Socket publish error:', err.message);
            cb(true, {message: err.message});
        }
    }
}

async function publish (channel, data, cb) {
    // Insert a message_source to signal that the message is from here
    data.message_source = 'audio_analytics_api';

    if (!config.blackboxContainer) {
        redis.publish(channel, data, cb);
    } else {
        // Use Unix socket - pure pub/sub (fire and forget)
        console.log('Publishing via Unix socket to channel:', channel);
        
        if (!globalSocketClient) {
            return cb(true, {message: 'Socket client not initialized'});
        }
        
        if (!(await ensureSocketConnected())) {
            return cb(true, {message: 'Socket server not available'});
        }

        try {
            await globalSocketClient.publish(channel, data);
            cb(false, data);
        } catch (err) {
            console.error('Socket publish error:', err.message);
            cb(true, {message: err.message});
        }
    }
}

function listenToChannel (channel, data, cb) {
    if (!config.blackboxContainer) {
        redis.listenToChannel(channel, data, cb);
    } else {
        // In blackbox mode, subscribe to the channel via Unix socket
        console.log('Subscribing to channel in blackbox mode:', channel);
        
        if (!globalSocketClient) {
            console.warn('Socket client not initialized yet, will retry...');
            // Retry after a short delay
            setTimeout(() => listenToChannel(channel, data, cb), 100);
            return;
        }
        
        globalSocketClient.subscribe(channel, cb);
    }
}

function unsubscribeFromChannel (channel, cb) {
    if (!config.blackboxContainer) {
        // Redis mode: no-op (redis subscriptions are per-connection and cleaned up automatically)
    } else {
        if (globalSocketClient) {
            globalSocketClient.unsubscribe(channel, cb);
        }
    }
}

function addMsgToQueue (data) {
    msgQueue.push(data.result);
}

function sendQueue (wsc) {
    // Return All messages in queue
    while(msgQueue.length !== 0) {
        wsc.send(JSON.stringify(msgQueue.shift()));
    }
}

function clearQueue () {
    msgQueue.splice(0, msgQueue.length);
}
module.exports = {
    initializeCommunication,
    publishAndListenOnce,
    publish,
    listenToChannel,
    unsubscribeFromChannel,
    addMsgToQueue,
    sendQueue,
    clearQueue
};

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const { randomUUID } = require('crypto');

const { createClient } = require('redis');

// Only create Redis client if not in blackbox mode
const client = !config.blackboxContainer ? createClient({
    socket: {
        port: config.redisPort,
        host: config.redisHost,
        reconnectStrategy: (retries) => {
            if (retries > 10) {
                console.error('Redis: Max reconnection attempts reached');
                return new Error('Max reconnection attempts reached');
            }
            const delay = Math.min(retries * 100, 3000);
            console.log(`Redis: Reconnecting in ${delay}ms (attempt ${retries})`);
            return delay;
        }
    }
}) : null;

let isConnecting = false;
let isConnected = false;

async function ensureConnection() {
    // Skip Redis connection in blackbox mode
    if (config.blackboxContainer) {
        console.log('Redis: Connection skipped in blackbox mode');
        return;
    }
    
    if (isConnected) return;
    if (isConnecting) {
        // Wait for ongoing connection attempt
        while (isConnecting) {
            await new Promise(resolve => setTimeout(resolve, 100));
        }
        return;
    }
    
    isConnecting = true;
    try {
        console.log(`Connecting to Redis at ${config.redisHost}:${config.redisPort}...`);
        await client.connect();
        isConnected = true;
        console.log('✅ Redis connected successfully');
    } catch (err) {
        console.error('❌ Redis connection failed:', err.message);
        throw err;
    } finally {
        isConnecting = false;
    }
}

// Start connection attempt but don't block module loading
(async () => {
    // Only attempt to connect if not in blackbox mode
    if (!config.blackboxContainer) {
        try {
            await ensureConnection();
        } catch (err) {
            console.error('Initial Redis connection failed, will retry on first use');
        }
    } else {
        console.log('Redis: Skipping connection in blackbox mode');
    }
})();

// Only attach event listeners if client exists (not in blackbox mode)
if (client) {
    client.on('connect', () => {
        isConnected = true;
        console.log('Redis: Connected');
    });

    client.on('ready', () => {
        isConnected = true;
        console.log('Redis: Ready');
    });

    client.on('error', (err) => {
        console.error('Redis error:', err.message);
    });

    client.on('reconnecting', () => {
        isConnected = false;
        console.log('Redis: Reconnecting...');
    });

    client.on('end', () => {
        isConnected = false;
        console.log('Redis: Connection ended');
    });
}

async function listenToChannel(channel, data, cb) {
    // Skip in blackbox mode
    if (config.blackboxContainer) {
        console.log('Redis: listenToChannel skipped in blackbox mode');
        return;
    }
    
    try {
        await ensureConnection();
        const subscriber = client.duplicate();
        await subscriber.connect();
        console.log('Listening to channel:', channel);
        await subscriber.subscribe(channel, (message) => {
            cb(false, data, message);
        });
    } catch (e) {
        console.error('Error subscribing to channel:', e);
    }
}

async function publish(channel, data, cb) {
    // Skip in blackbox mode
    if (config.blackboxContainer) {
        console.log('Redis: publish skipped in blackbox mode');
        cb(false, data);
        return;
    }
    
    try {
        await ensureConnection();
        let publisher = client.duplicate();
        await publisher.connect();
        let sync_id = randomUUID();
        if (config.TESTING_ON == 'true') sync_id = 'RANDOM_UUID';

        // Insert unique ID to ensure a synchronous callback
        data.sync_id = sync_id;

        // publish on given channel
        await publisher.publish(channel, JSON.stringify(data));
        await publisher.quit();

        cb(false, data);
    } catch (e) {
        console.error('Error publishing to channel:', e);
        cb(true, e)
    }
}

async function publishAndListenOnce(channelIn, channelOut, data, cb) {
    // Skip in blackbox mode
    if (config.blackboxContainer) {
        console.log('Redis: publishAndListenOnce skipped in blackbox mode');
        cb(false, data);
        return;
    }
    
    try {
        await ensureConnection();
        let redisClient = client.duplicate();
        await redisClient.connect();
        let sync_id = randomUUID();
        if (config.TESTING_ON == 'true') sync_id = 'RANDOM_UUID';

        // Insert unique ID to ensure a synchronous callback
        data.sync_id = sync_id;

        // publish on given channelIn
        await redisClient.publish(channelIn, JSON.stringify(data));

        // Listen to the channelOut until a message is received with same ID
        await redisClient.subscribe(channelOut, (message) => {
            try {
                // Check to see if message matches unique ID from earlier
                let retData = JSON.parse(message);

                // If message matches the unique ID run Callback
                if (retData.error) {
                    cb(true, retData.result);
                }
                else if (retData.sync_id == sync_id && retData.result !== undefined) {
                    cb(false, retData.result);

                    redisClient.unsubscribe();
                    redisClient.quit();
                }
            } catch (e) {
                console.error(e.message);
                cb(true, e)
            }
        });
    } catch (e) {
        console.error(e);
        cb(true, e.message);
    }
}

module.exports = {
    listenToChannel,
    publish,
    publishAndListenOnce
};
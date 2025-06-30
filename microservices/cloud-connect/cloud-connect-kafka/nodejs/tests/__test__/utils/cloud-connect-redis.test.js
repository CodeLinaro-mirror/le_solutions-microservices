/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect-redis.test.js
 * Description :- Description :- The test file to validate Cloud Client Redis related functionality.
 */
'use strict';

import { createClient } from 'redis';
import { CloudConnectRedis } from "../../../src/utils/cloud-connect-redis.js";
import { RedisConstants } from "../../../src/constants/redis-constants.js";
import { RedisMemoryServer } from 'redis-memory-server';

jest.mock('../../../src/constants/redis-constants.js');

// In Memory Redis Server
let redisServer;
// Spawned In Memory Redis Server Host
let redisHost;
// Spawned In Memory Redis Server Port
let redisPort;
const data = {
    "object_detection": [
        {
            "label": "person",
            "rectangle": {
                "x": 0.06875,
                "y": 0,
                "width": 0.16197916666666667,
                "height": 0.9879629629629629
            },
            "object_detection": [
                {
                    "label": "safety vest",
                    "rectangle": {
                        "x": 0.0921875,
                        "y": 0.13796296296296295,
                        "width": 0.09166666666666666,
                        "height": 0.387037037037037
                    }
                },
                {
                    "label": "helmet",
                    "rectangle": {
                        "x": 0.13229166666666667,
                        "y": 0,
                        "width": 0.06770833333333333,
                        "height": 0.1398148148148148
                    }
                }
            ]
        },
        {
            "label": "person",
            "rectangle": {
                "x": 0.4125,
                "y": 0.06666666666666667,
                "width": 0.18697916666666667,
                "height": 0.9324074074074075
            },
            "object_detection": [
                {
                    "label": "safety vest",
                    "rectangle": {
                        "x": 0.43072916666666666,
                        "y": 0.25925925925925924,
                        "width": 0.16041666666666668,
                        "height": 0.4009259259259259
                    }
                }
            ]
        },
        {
            "label": "person",
            "confidence": 90.56021118164062,
            "color": 16711935,
            "rectangle": {
                "x": 0.6125,
                "y": 0,
                "width": 0.26197916666666665,
                "height": 1
            },
            "object_detection": [
                {
                    "label": "safety vest",
                    "rectangle": {
                        "x": 0.653125,
                        "y": 0.14166666666666666,
                        "width": 0.1484375,
                        "height": 0.3962962962962963
                    }
                },
                {
                    "label": "helmet",
                    "rectangle": {
                        "x": 0.6755208333333333,
                        "y": 0,
                        "width": 0.07916666666666666,
                        "height": 0.13333333333333333
                    }
                }
            ]
        }
    ],
    "parameters": {
        "timestamp": "87651879582"
    }
}

// The callback method to be called on receipt of messages from subscribed channel
let listOfExpectedPublishedTopics = ['detection:ppe:1', 'detection:ppe:2'];
async function subscriptionCallback(message, channel) {
    console.log(`Channel ${channel} sent message: \n ${message}`);
    expect(listOfExpectedPublishedTopics).toContain(channel);
    expect(message).toBe(JSON.stringify(data));
}

// Before all tests are executed across suites setup Redis Container
beforeAll(async () => {
    // Start the In memory Redis server
    redisServer = new RedisMemoryServer();
    redisHost = await redisServer.getHost();
    redisPort = await redisServer.getPort();
}, 300000);

// Clean up after all the tests are executed across test suites.
afterAll(async () => {
    // Stop the created in memory redis server
    await redisServer.stop();
});

// Test Suite for all Cloud Connect Redis Client related Test cases
describe('Cloud Connect Redis Client', () => {
    // Redis Client to be used for publishing message
    let redisClient;
    // Cloud Connect Redis Client
    let cloudConnectRedisClient = undefined;
    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(async () => {
        // Create the Redis Client to be used for publishing the message
        redisClient = createClient({
            url: `redis://${redisHost}:${redisPort}`,
            socket: {
                connectTimeout: RedisConstants.CONNECTION_TIMEOUT, // Setting Timeout in milliseconds
                reconnectStrategy: function (retries) {
                    Logger.log(Logger.DEBUG, `Attempting to reconnect to Cloud Connect Test Redis Client. Retry count: ${retries}`);
                    return RedisConstants.CONNECTION_RETRY_INTERVAL;
                }
            }
        });
        // Listener for successful connection of test Redis Client
        redisClient.on('connect', async () => {
            console.log('Successfully connected the Cloud Connect Test Redis Client.');
        });
        // Listener for errors in the test Redis client
        redisClient.on('error', error => {
            console.log('Error in Cloud Connect Test Redis Client.');
        });
        // Listener for reconnection attempts of test Redis Client
        redisClient.on('reconnecting', () => {
            console.log('Cloud Connect Test Redis Client attempting to reconnect.');
        });
        // Connect the Client
        await redisClient.connect();
        // Put in a delay so that connection is complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Subscribe to the topic
        await redisClient.pSubscribe('detection:ppe:*', subscriptionCallback);
    });

    // Clean up after all the tests of suite are executed.
    afterAll(async () => {
        // Disconnect the Test Redis Client
        await redisClient.quit();
    });

    // Task to be done before each Test of suite is executed
    beforeEach(() => {
        cloudConnectRedisClient = undefined;
    });

    // Task to be done after each Test of Suite is executed
    afterEach(async () => {
        if (cloudConnectRedisClient != undefined) {
            await cloudConnectRedisClient.deInitialize();
        }
    });

    // Validate message is successfully published by Cloud Connect Redis Client
    it("Validate message is successfully published by Cloud Connect Redis Client", async () => {
        // Create an instance of Cloud Connect Redis Client
        cloudConnectRedisClient = new CloudConnectRedis(redisHost, redisPort, undefined);
        // Initialize the Client
        await cloudConnectRedisClient.initialize();
        // Put in a delay so that initialization can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Invoke the Client Publish Method
        await cloudConnectRedisClient.publish('detection:ppe:1', JSON.stringify(data));
        // Put in a delay so that publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
    }, 10000);

    // Validate reconnection is handled by Cloud Connect Redis Client
    it("Validate reconnection is handled by Cloud Connect Redis Client", async () => {
        var consoleErrorSpy = jest.spyOn(console, 'error');
        // Create an instance of Cloud Connect Redis Client
        cloudConnectRedisClient = new CloudConnectRedis(redisHost, redisPort, undefined);
        // Initialize the Client
        await cloudConnectRedisClient.initialize();
        // Put in a delay so that initialization can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Stop the created in memory redis server
        await redisServer.stop();
        // Wait for 1.5 seconds for an reconnect attempt to happen
        await new Promise(resolve => setTimeout(resolve, 1500));
        // Start the created in memory redis server
        await redisServer.start();
        // Wait for 2 seconds for reconnection to be successful
        await new Promise(resolve => setTimeout(resolve, 2000));
        expect(consoleErrorSpy).toHaveBeenCalled();

        // Re-Subscribe to the topic post restart 
        await redisClient.pSubscribe('detection:ppe:*', subscriptionCallback);
        // Put in a delay so that subscription can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Invoke the Client Publish Method
        await cloudConnectRedisClient.publish('detection:ppe:2', JSON.stringify(data));
        // Put in a delay so that publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));

        // Access the Redis Client
        const mockRedisClient = cloudConnectRedisClient.getCloudConnectRedisClient();
        // Save the original publish method
        const originalPublish = mockRedisClient.publish;
        // Mock the publish method to throw an error
        mockRedisClient.publish = jest.fn((channel, message) => {
            return Promise.reject(new Error('Simulated Cloud Connect Redis Client Publish error.'));
        });
        try {
            await cloudConnectRedisClient.publish('detection:ppe:2', JSON.stringify(data));
        } catch (error) {
            expect(error.message).toEqual('Simulated Cloud Connect Redis Client Publish error.');
        }
        // Restore the original publish method
        mockRedisClient.publish = originalPublish;
        consoleErrorSpy.mockRestore();
    }, 20000);
});
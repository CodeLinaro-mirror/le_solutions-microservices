/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis.test.js
 * Description :- The test file to validate Redis related functionality.
 */
'use strict';

import { createClient } from 'redis';
import { RedisSubscriber } from "../../utils/redis.js";
import { RedisConstants } from "../../constants/redis-constants.js";
import { RedisMemoryServer } from 'redis-memory-server';

jest.mock('../../constants/redis-constants.js');

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

// The Mock Cloud Client Class to be used by Redis Subscriber as Cloud Client
class MockCloudClient {
    // The Dummy Async Publish Method to be invoked by Redis Subscriber
    async publish(sourceTopic, topicToPublish, msgToPublish) {
        var listOfSourceTopics = ['detection:ppe:1', 'detection:ppe:2', 'detection:rz:1', 'detection:rz:2',
            'detection:rz:3'];
        var listOfExpectedTopics = ['detection/ppe/mqtt/1', 'detection/ppe/mqtt/2', 'detection/ppe/mqtt/rz/1',
            'detection/ppe/mqtt/rz/2', 'detection/ppe/mqtt/rz/3'];
            expect(listOfSourceTopics).toContain(sourceTopic);
        expect(listOfExpectedTopics).toContain(topicToPublish);
        expect(msgToPublish).toBe(JSON.stringify(data));
        console.info(`Successfully validated message ${msgToPublish} received from ${topicToPublish}.`);
    }
    // The Dummy shutdown Method to be invoked by Redis Subscriber
    shutdown() {
        console.info('Mock Cloud Client Shutdown invoked.')
    }
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

// Test Suite for all Redis Subscriber Related Test Cases
describe('Redis Subscriber', () => {
    // Redis Client to be used for publishing message
    let redisClient;
    // Redis Subscriber
    let redisSubcriber = undefined;

    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(async () => {
        // Create the Redis Client to be used for publishing the message
        redisClient = createClient({
            url: `redis://${redisHost}:${redisPort}`,
            socket: {
                connectTimeout: RedisConstants.CONNECTION_TIMEOUT, // Setting Timeout in milliseconds
                reconnectStrategy: function (retries) {
                    Logger.log(Logger.DEBUG, `Attempting to reconnect to test Redis Client. Retry count: ${retries}`);
                    return RedisConstants.CONNECTION_RETRY_INTERVAL;
                }
            }
        });
        // Listener for successful connection of test Redis Client
        redisClient.on('connect', async () => {
            console.log('Successfully connected the Test Redis Client.');
        });
        // Listener for errors in the test Redis client
        redisClient.on('error', error => {
            console.log('Error in Test Redis Client.');
        });
        // Listener for reconnection attempts of test Redis Client
        redisClient.on('reconnecting', () => {
            console.log('Test Redis Client attempting to reconnect.');
        });
        // Connect the Client
        await redisClient.connect();
    });

    // Clean up after all the tests of suite are executed.
    afterAll(async () => {
        // Disconnect the Test Redis Client
        await redisClient.quit();
    });

    // Task to be done before each Test of suite is executed
    beforeEach(() => {
        redisSubcriber = undefined;
    });

    // Task to be done after each Test of Suite is executed
    afterEach(async () => {
        await redisSubcriber.disconnectSubscriber();
    });

    // Validate message is received by subscriber on publishing to correct topic
    it("Validated message is successfully received by the subscriber", async () => {
        // Create an instance of Redis Subscriber and set the required topics and cloud client
        redisSubcriber = new RedisSubscriber(redisHost, redisPort, undefined);
        redisSubcriber.setTopicToSubscribe('detection:ppe:*');
        redisSubcriber.setTopicToPublish('detection/ppe/mqtt/*');
        var cloudClient = new MockCloudClient();
        redisSubcriber.setCloudClient(cloudClient);
        // Subscribe to the topic
        await redisSubcriber.subscribe();
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        // Put in a delay so that subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
    }, 10000);

    // Validate if subscriber is able to pause and resume as expected
    it("Validate if subscriber is able to pause and resume as expected", async () => {
        redisSubcriber = new RedisSubscriber(redisHost, redisPort, undefined);
        redisSubcriber.setTopicToSubscribe('detection:rz:*');
        redisSubcriber.setTopicToPublish('detection/ppe/mqtt/rz/*');
        var cloudClient = new MockCloudClient();
        redisSubcriber.setCloudClient(cloudClient);
        // Subscribe to the topic
        await redisSubcriber.subscribe();
        redisSubcriber.pauseSubscriber();
        await new Promise(resolve => setTimeout(resolve, 100));
        // Publish the data to the topic when it is paused
        await redisClient.publish('detection:rz:1', JSON.stringify(data));
        await redisClient.publish('detection:rz:2', JSON.stringify(data));
        await redisSubcriber.resumeClient();
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the data to the topic when it has been resumed
        await redisClient.publish('detection:rz:3', JSON.stringify(data));
        // Put in a delay so that subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
    }, 10000);

    // Validate reconnection is handled in case Redis Server is down
    it("Validate reconnection is handled in case Redis Server is down", async () => {
        var consoleErrorSpy = jest.spyOn(console, 'error');
        // Create an instance of Redis Subscriber and set the required topics and cloud client
        redisSubcriber = new RedisSubscriber(redisHost, redisPort, undefined);
        redisSubcriber.setTopicToSubscribe('detection:ppe:*');
        redisSubcriber.setTopicToPublish('detection/ppe/mqtt/*');
        var cloudClient = new MockCloudClient();
        redisSubcriber.setCloudClient(cloudClient);
        // Subscribe to the topic
        await redisSubcriber.subscribe();
        // Wait for few milliseconds for connection and subscription to happen
        await new Promise(resolve => setTimeout(resolve, 100));
        // Stop the created in memory redis server
        await redisServer.stop();
        // Wait for 1.5 seconds for an reconnect attempt to happen
        await new Promise(resolve => setTimeout(resolve, 1500));
        // Start the created in memory redis server
        await redisServer.start();
        // Wait for 4 seconds for reconnection to be successful
        await new Promise(resolve => setTimeout(resolve, 4000));
        expect(consoleErrorSpy).toHaveBeenCalled();
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:2', JSON.stringify(data));
        // Put in a delay so that subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        consoleErrorSpy.mockRestore();
    }, 15000);
});
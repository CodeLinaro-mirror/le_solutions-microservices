/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect.test.js
 * Description :- The test file to validate the cloud-connect.js by invoking 
 *                its functions as it is done from index.js.
 */
'use strict';

import fs from 'fs';
import { connect } from "mqtt";
import { createClient } from 'redis';
import { RedisMemoryServer } from 'redis-memory-server';
import { CloudConnect } from "../../src/cloud-connect.js";
import { CloudConnectConstants } from "../../src/constants/cloud-connect-constants.js";
import { MQTTConstants } from "../../src/constants/mqtt-constants.js";
import { AedesBroker } from '../__mocks__/aedesBroker.js';

jest.mock('../../src/constants/cloud-connect-constants.js');
jest.mock('../../src/constants/mqtt-constants.js');
jest.mock('../../src/constants/redis-constants.js');

// In Memory Redis Server
let redisServer;
// Spawned In Memory Redis Server Host
let redisHost;
// Spawned In Memory Redis Server Port
let redisPort;
// MQTT In Memory Broker
let mqttBroker;
// MQTT In Memory Broker Port
let mqttBrokerPort = 8884;
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

// Before all tests are executed across suites setup all the required Container
beforeAll(async () => {
    // Create the instance of MQTT In Memory Broker
    mqttBroker = new AedesBroker(mqttBrokerPort, true,
        'tests/__mocks__/server_certificates/rootca.pem.crt',
        'tests/__mocks__/server_certificates/server.pem.crt',
        'tests/__mocks__/server_certificates/server.private.pem.key');
    // Start the MQTT In Memory Broker
    mqttBroker.start();
    // Start the In memory Redis server
    redisServer = new RedisMemoryServer();
    redisHost = await redisServer.getHost();
    redisPort = await redisServer.getPort();

    // Set Redis Host
    process.env.REDIS_HOST = redisHost;
    // Set Redis Port
    process.env.REDIS_PORT = redisPort;
    // Set MQTT Host
    process.env.BROKER_HOST = 'localhost';
    // Set MQTT Port
    process.env.BROKER_PORT = mqttBrokerPort;
}, 30000);

// Clean up after all the tests are executed across test suites.
afterAll(async () => {
    // Stop the created in memory redis server
    await redisServer.stop();
    // Stop the MQTT In Memory Broker
    await mqttBroker.stop();
}, 10000);

// Test Suite for all Cloud Connect Service Related Test Cases
describe('Cloud Connect Service', () => {
    // Redis Client to be used for publishing message
    let redisClient;
    // MQTT Subscriber Client
    let mqttSubscriber = 'undefined';
    // Message received by Consumer
    let mqttConsumerReceivedMessage = '';
    // Topic on which Message is received
    let mqttConsumerReceivedMessageTopic = '';

    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(async () => {
        // Create MQTT Subscriber Option
        var mqttOptions = {
            keepalive: MQTTConstants.CONNECTION_KEEPALIVE,
            protocolVersion: MQTTConstants.CONNECTION_PROTCOL_VERSION,
            connectTimeout: MQTTConstants.CONNECTION_TIMEOUT,
            reconnectPeriod: MQTTConstants.CONNECTION_RECONNECT_PERIOD,
            reconnectOnConnackError: MQTTConstants.CONNECTION_RECONNECT_ON_CONNACK_ERROR,
            ca: fs.readFileSync('tests/__test__/certificates/rootca.pem.crt', 'utf-8'),
            cert: fs.readFileSync('tests/__test__/certificates/certificate.pem.crt', 'utf-8'),
            key: fs.readFileSync('tests/__test__/certificates/private.pem.key', 'utf-8'),
            rejectUnauthorized: false // Disable enable Host Name based on Flag
        };
        // Create the MQTT Client
        mqttSubscriber = connect(`mqtts://localhost:${mqttBrokerPort}`, mqttOptions);

        // Subscribe to the topic on connection  
        mqttSubscriber.on("connect", () => {
            mqttSubscriber.subscribe(['detection/ppe/mqtt/1'], (error) => {
                expect(error).toBeNull();
            });
        });

        // Callback for Received Message over subscribed topic
        mqttSubscriber.on("message", (topic, message) => {
            console.log(`Message received by MQTT Subscriber over topic ${topic} is ${message}`);
            mqttConsumerReceivedMessage = message.toString();
            mqttConsumerReceivedMessageTopic = topic;
        });

        // Create the Redis Client to be used for publishing the message
        redisClient = createClient({
            url: `redis://${redisHost}:${redisPort}`
        });
        // Connect the Client
        await redisClient.connect();
    }, 10000);

    // Clean up after all the tests of suite are executed.
    afterAll(async () => {
        // Disconnect the MQTT Subscriber
        mqttSubscriber.end();
        // Disconnect the Redis Client
        await redisClient.quit();
    }, 30000);

    // Task to be done before each Test of suite is executed
    beforeEach(() => {
        mqttConsumerReceivedMessage = '';
        mqttConsumerReceivedMessageTopic = '';
    });

    // Task to be done after each Test of Suite is executed
    afterEach(() => {
        mqttConsumerReceivedMessage = '';
        mqttConsumerReceivedMessageTopic = '';
    });

    // Validate the Cloud Connect service initialization as it is done by index.js
    it('Validate the cloud connect service initialization', async () => {
        // Initialize the Cloud Connect Service with Configuration JSON
        var cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME);

        // Initialize the Subscriber of the Cloud Connect Service
        cloudClient.initialiseSubscribers();
        var initializedSubscriberArray = cloudClient.getInitializedSubscriber();
        // Validate the number of Subscriber initialized based on config.json
        expect(initializedSubscriberArray.length).toBe(4);

        // Once Intialized, initiate the Subscription
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.initiateSubscription(subscriber);
        });
        // Initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.initialize();
        // Wait for 1 second for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 1000));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 100));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));

        // Disconnect the Subscriber
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.removeSubscription(subscriber);
        });

        // Clean up the Client Id to Initialized Subcriber Map
        CloudConnect.cloudClientToInitializedSubscriber.clear();
        // De-initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.deInitialize();
        CloudConnect.cloudConnectRedisClient = undefined;
    }, 30000);

    // Validate the Cloud Client Disconnection is handled by Cloud Connect Service
    it('Validate the Cloud Client Disconnection is handled', async () => {
        // Initialize the Cloud Connect Service with Configuration JSON
        var cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME);

        // Initialize the Subscriber of the Cloud Connect Service
        cloudClient.initialiseSubscribers();
        var initializedSubscriberArray = cloudClient.getInitializedSubscriber();
        // Validate the number of Subscriber initialized based on config.json
        expect(initializedSubscriberArray.length).toBe(4);

        // Once Intialized, initiate the Subscription
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.initiateSubscription(subscriber);
        });
        // Initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.initialize();
        // Wait for 1 second for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 1000));

        // Test the disconnection flow as the subscribers are initialized
        var clientIdToClose = CloudConnect.cloudClientToInitializedSubscriber.keys().next().value;
        mqttBroker.closeClientConnection(clientIdToClose);
        // Wait for 5.5 seconds for reconnection to happen
        await new Promise(resolve => setTimeout(resolve, 5500));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));

        // Disconnect the Subscriber
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.removeSubscription(subscriber);
        });

        // Clean up the Client Id to Initialized Subcriber Map
        CloudConnect.cloudClientToInitializedSubscriber.clear();
        // De-initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.deInitialize();
        CloudConnect.cloudConnectRedisClient = undefined;
    }, 30000);

    // Validate that Cloud Connect publishes back message in case of error
    it('Validate that Cloud Connect publishes back message in case of error', async () => {
        // Create a redisClient which will subscribe to the topic
        var testCaseRedisClient = redisClient.duplicate();
        await testCaseRedisClient.connect();
        await testCaseRedisClient.pSubscribe('detection:ppe:1', async (message, channel) => {
            console.log(`Published back message on Channel ${channel} : \n ${message}`);
            expect(message).toBe(JSON.stringify(data));
        });
        // Initialize the Cloud Connect Service with Configuration JSON
        var cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME);

        // Initialize the Subscriber of the Cloud Connect Service
        cloudClient.initialiseSubscribers();
        var initializedSubscriberArray = cloudClient.getInitializedSubscriber();
        // Validate the number of Subscriber initialized based on config.json
        expect(initializedSubscriberArray.length).toBe(4);

        // Once Intialized, initiate the Subscription
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.initiateSubscription(subscriber);
        });
        // Initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.initialize();
        // Wait for 1 second for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 1000));

        // Get the required Initialized Subscriber whose cloud client will be shutdown
        let cloudClientToDisconnect = initializedSubscriberArray.at(0).getCloudClient();
        cloudClientToDisconnect.shutdown();
        // Publish the message to the required topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        // Put in some delay to shutdown the pubslisher cleint
        await new Promise(resolve => setTimeout(resolve, 250));

        // Disconnect the Subscriber
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.removeSubscription(subscriber);
        });

        // Clean up the Client Id to Initialized Subcriber Map
        CloudConnect.cloudClientToInitializedSubscriber.clear();
        // De-initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.deInitialize();
        CloudConnect.cloudConnectRedisClient = undefined;
        // Disconnect the Redis Client
        await testCaseRedisClient.quit();
    }, 30000);

    // Validate the Cloud Connect service throws error in case of missing environment variables
    it('Validate the Cloud Connect service throws error in case of missing environment variables', async () => {
        // Unset the environment variables
        // Unset Redis Host
        delete process.env.REDIS_HOST;
        // Unset Redis Port
        delete process.env.REDIS_PORT;
        // Unset MQTT Host
        delete process.env.BROKER_HOST;
        // Unset MQTT Port
        delete process.env.BROKER_PORT;

        var cloudClient = undefined;

        expect(() => { cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME); }).toThrow('Environment variable REDIS_HOST is not set.');

        // Set Redis Host
        process.env.REDIS_HOST = redisHost;
        expect(() => { cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME); }).toThrow('Environment variable REDIS_PORT is not set.');

        // Set Redis Port
        process.env.REDIS_PORT = redisPort;
        expect(() => { cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME); }).toThrow('Environment variable BROKER_HOST is not set.');

        // Set MQTT Host
        process.env.BROKER_HOST = 'localhost';
        expect(() => { cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME); }).toThrow('Environment variable BROKER_PORT is not set.');

        // Set MQTT Port
        process.env.BROKER_PORT = mqttBrokerPort;

        // Initialize the Cloud Connect Service with Configuration JSON
        cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME);

        // Initialize the Subscriber of the Cloud Connect Service
        cloudClient.initialiseSubscribers();
        var initializedSubscriberArray = cloudClient.getInitializedSubscriber();
        // Validate the number of Subscriber initialized based on config.json
        expect(initializedSubscriberArray.length).toBe(4);

        // Once Intialized, initiate the Subscription
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.initiateSubscription(subscriber);
        });
        // Initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.initialize();
        // Wait for 1 second for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 1000));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 100));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));

        // Disconnect the Subscriber
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.removeSubscription(subscriber);
        });

        // Clean up the Client Id to Initialized Subcriber Map
        CloudConnect.cloudClientToInitializedSubscriber.clear();
        // De-initialize the Cloud Connect Redis Client
        await CloudConnect.cloudConnectRedisClient.deInitialize();
        CloudConnect.cloudConnectRedisClient = undefined;
    }, 30000);
});
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect.test.js
 * Description :- The test file to validate the cloud-connect.js by invoking 
 *                its functions as it is done from index.js.
 */
'use strict';

import fs from 'fs';
import { Kafka } from "kafkajs";
import { createClient } from 'redis';
import { RedisMemoryServer } from 'redis-memory-server';
import { CloudConnect } from "../cloud-connect.js";
import { CloudConnectConstants } from "../constants/cloud-connect-constants.js";
import { KafkaConstants } from "../constants/kafka-constants.js";
import { RedisConstants } from "../constants/redis-constants.js";

jest.mock('../constants/cloud-connect-constants.js');
jest.mock('../constants/kafka-constants.js');
jest.mock('../constants/redis-constants.js');

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

// Before all tests are executed across suites setup all the required Container
beforeAll(async () => {
    // Clean up the topics list previously stored
    Kafka.topics = {};
    // Start the In memory Redis server
    redisServer = new RedisMemoryServer();
    redisHost = await redisServer.getHost();
    redisPort = await redisServer.getPort();

    // Set Redis Host
    process.env.REDIS_HOST = redisHost;
    // Set Redis Port
    process.env.REDIS_PORT = redisPort;
    // Set Kafka Host
    process.env.BROKER_HOST = 'localhost';
    // Set Kafka Port
    process.env.BROKER_PORT = 9092;
}, 30000);

// Clean up after all the tests are executed across test suites.
afterAll(async () => {
    // Stop the created in memory redis server
    await redisServer.stop();
}, 10000);

// Test Suite for all Cloud Connect Service Related Test Cases
describe('Cloud Connect Service', () => {
    // Kafka Instance for creating Pub Sub Instances
    let kafka;
    // Kafka Consumer
    let kafkaConsumer = 'undefined';
    // Redis Client to be used for publishing message
    let redisClient;
    // Message received by Consumer
    let kafkaConsumerReceivedMessage = '';
    // Topic on which Message is received
    let kafkaConsumerReceivedMessageTopic = '';

    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(async () => {
        // Create the SSL Context Object
        let sslContext = {
            ca: [fs.readFileSync('__test__/certificates/rootca.pem.crt', 'utf-8')],
            key: fs.readFileSync('__test__/certificates/private.pem.key', 'utf-8'),
            cert: fs.readFileSync('__test__/certificates/certificate.pem.crt', 'utf-8'),
            passphrase: "password",
            checkServerIdentity: () => { return null; }
        };
        // The Kafka Class instance to be used for creating publishers
        kafka = new Kafka({
            clientId: `${KafkaConstants.CONNECTION_CLIENT_ID}-CLoudConnectTest`,
            brokers: ['localhost:9092'],
            ssl: sslContext
        });
        // Create the Redis Client to be used for publishing the message
        redisClient = createClient({
            url: `redis://${redisHost}:${redisPort}`
        });
        // Create the Kafka Consumer
        kafkaConsumer = kafka.consumer({ groupId: 'Consumer1' });
        // Connect the Consumer
        await kafkaConsumer.connect();
        // Subscribe to the topic
        await kafkaConsumer.subscribe({ topic: 'detection.ppe.kafka.1', fromBeginning: true });
        // Callback for Received Message over subscribed topic
        await kafkaConsumer.run({
            eachMessage: async ({ topic, partition, message }) => {
                kafkaConsumerReceivedMessage = JSON.stringify(data);
                kafkaConsumerReceivedMessageTopic = topic;
                console.log(`Received message from topic '${topic}': ${message.value.toString()}`);
            }
        });
        // Connect the Client
        await redisClient.connect();
    }, 30000);

    // Clean up after all the tests of suite are executed.
    afterAll(async () => {
        // Disconnect the Kafka Consumer
        await kafkaConsumer.disconnect();
        // Disconnect the Redis Client
        await redisClient.quit();
    }, 30000);

    // Task to be done before each Test of suite is executed
    beforeEach(() => {
        kafkaConsumerReceivedMessage = '';
        kafkaConsumerReceivedMessageTopic = '';
    });

    // Task to be done after each Test of Suite is executed
    afterEach(async () => {
        kafkaConsumerReceivedMessage = '';
        kafkaConsumerReceivedMessageTopic = '';
        Kafka.throwProducerError = false;
        Kafka.throwConnectionError = false;
        Kafka.throwProducerDisconnectionError = false;
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
        // Wait for few milliseconds for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));

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
        // Wait for few milliseconds for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 500));

        // Get the required Initialized Subscriber for testing disconnect
        let cloudClientToDisconnect = initializedSubscriberArray.at(0).getCloudClient();
        cloudClientToDisconnect.getProducer().simulateDisconnection(true);
        // Let the disconnection run for 4 seconds so that check connection message is invoked
        await new Promise(resolve => setTimeout(resolve, 4000));
        cloudClientToDisconnect.getProducer().simulateDisconnection(false);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 200));

        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));

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

    // Validate that Cloud Connect publishes back message in case of error.
    it('Validate that Cloud Connect publishes back message in case of error', async () => {
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
        // Wait for few milliseconds for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 500));

        // Test the flow of sending message back to Redis in case of failure
        Kafka.throwProducerError = true;
        var consoleErrorSpy = jest.spyOn(console, 'error');
        // Publish the message to the required topic and check for error
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 200));
        Kafka.throwProducerError = false;
        expect(consoleErrorSpy).toHaveBeenCalled();
        consoleErrorSpy.mockRestore();
        //Put in a delay of few milliseconds so as after republication message is received by Kafka Consumer.
        await new Promise(resolve => setTimeout(resolve, 100));

        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));

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
        process.env.BROKER_PORT = 9092;

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
        // Wait for few milliseconds for all Subscriber to be up and running.
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the data to the topic
        await redisClient.publish('detection:ppe:1', JSON.stringify(data));
        //Put in a delay of few milliseconds so as subscription and publication can complete.
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));

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
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- kafka.test.js
 * Description :- The test file to validate Kafka related functionality.
 */
'use strict';

import fs from 'fs';
import { Kafka } from "kafkajs";
import { KafkaPublisher } from "../../../src/utils/kafka.js";
import { KafkaConstants } from "../../../src/constants/kafka-constants.js";

jest.mock('../../../src/constants/kafka-constants.js');

jest.mock('../../../src/cloud-connect.js', () => {
    const originalModule = jest.requireActual('../../../src/cloud-connect.js');
    return {
        ...originalModule,
        CloudConnect: {
            cloudConnectRedisClient: {
                publish: jest.fn()
            },
            cloudClientToInitializedSubscriber: new Map(),
            publishMessageBackToRedis: jest.fn(),
            initiateSubscription: originalModule.CloudConnect.initiateSubscription,
            removeSubscription: originalModule.CloudConnect.removeSubscription,
            notifyCloudClientDisconnection: originalModule.CloudConnect.notifyCloudClientDisconnection,
            notifyCloudClientReconnection: originalModule.CloudConnect.notifyCloudClientReconnection,
            removeCloudClientSubcriberMapping: originalModule.CloudConnect.removeCloudClientSubcriberMapping
        }
    };
});

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

// Before all tests are executed across suites setup Kafka Container

beforeAll(() => {
    // Clean up the topics list previously stored
    Kafka.topics = {};
});

// Clean up after all the tests are executed across test suites.
afterAll(() => {

});

// Test Suite for all Kafka Publisher Related Test Cases
describe('Kafka Publisher', () => {
    // Mock for Process Exit
    let exitMock;
    // Kafka Instance for creating Pub Sub Instances
    let kafka;
    // Kafka Publisher
    let kafkaPublisher = 'undefined';
    // Kafka Consumer
    let kafkaConsumer = 'undefined';
    // Message received by Consumer
    let kafkaConsumerReceivedMessage = '';
    // Topic on which Message is received
    let kafkaConsumerReceivedMessageTopic = '';
    // Object to store SSL Config
    let sslConfig = {};

    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(async () => {
        // Create the SSL Config to be used for creating MQTT Client
        sslConfig[KafkaConstants.CONNECTION_ROOT_CA_FILE_KEY] = 'tests/__test__/certificates/rootca.pem.crt';
        sslConfig[KafkaConstants.CONNECTION_CLIENT_CERT_FILE_KEY] = 'tests/__test__/certificates/certificate.pem.crt';
        sslConfig[KafkaConstants.CONNECTION_CLIENT_KEY_FILE_KEY] = 'tests/__test__/certificates/private.pem.key';
        sslConfig[KafkaConstants.CONNECTION_CHECK_HOST_NAME_KEY] = 0;
        sslConfig[KafkaConstants.CONNECTION_CLIENT_CERT_PWD_KEY] = 'password';

        // Initialize Spy for Process exit invocation
        exitMock = jest.spyOn(process, 'exit').mockImplementation(() => { });
        // Create the SSL Context Object
        let sslContext = {
            ca: [fs.readFileSync('tests/__test__/certificates/rootca.pem.crt', 'utf-8')],
            key: fs.readFileSync('tests/__test__/certificates/private.pem.key', 'utf-8'),
            cert: fs.readFileSync('tests/__test__/certificates/certificate.pem.crt', 'utf-8'),
            passphrase: "password",
            checkServerIdentity: () => { return null; }
        };
        // The Kafka Class instance to be used for creating publishers        
        kafka = new Kafka({
            clientId: `${KafkaConstants.CONNECTION_CLIENT_ID}-KafkaTest`,
            brokers: ['localhost:9092'],
            ssl: sslContext
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
                kafkaConsumerReceivedMessage = message.value.toString();
                kafkaConsumerReceivedMessageTopic = topic;
                console.log(`Received message from topic '${topic}': ${message.value.toString()}`);
            }
        });
    }, 30000);

    // Clean up after all the tests of suite are executed.
    afterAll(async () => {
        // Disconnect the Kafka Consumer
        await kafkaConsumer.disconnect();
        // De-initialize the SSL Config
        sslConfig = {};
        // Cancel Process Exit Spy
        exitMock.mockRestore();
    });

    // Task to be done before each Test of suite is executed
    beforeEach(() => {
        // Clear all mock instances and calls to constructor and all methods:
        jest.clearAllMocks();
        kafkaConsumerReceivedMessage = '';
        kafkaConsumerReceivedMessageTopic = '';
    });

    // Task to be done after each Test of Suite is executed
    afterEach(async () => {
        kafkaConsumerReceivedMessage = '';
        kafkaConsumerReceivedMessageTopic = '';
        kafkaPublisher = 'undefined';
        Kafka.throwProducerError = false;
        Kafka.throwConnectionError = false;
        Kafka.throwProducerDisconnectionError = false;
    });

    // Validate message is published sccessfully by the Kafka Publisher to a subscribed topic
    it('Validate successful message publication by the Kafka Publisher to a subscribed topic', async () => {
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', false, 'default', true, sslConfig);
        kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic        
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));
        await kafkaPublisher.shutdown();
    }, 10000);

    // Validate message is not published by the Kafka Publisher to an unsubscribed topic
    it('Validate unsuccessful message publication by the Kafka Publisher to an unsubscribed topic', async () => {
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', false, 'default', true, sslConfig);
        kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic        
        await kafkaPublisher.publish('unsubscribed:topic', 'unsubscribed.topic.kafka', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('');
        expect(kafkaConsumerReceivedMessage).toBe('');
        await kafkaPublisher.shutdown();
    });

    // Validate error thrown by producer during message publishing is handled
    it('Validate error thrown by prodcuer during message publishing is handled', async () => {
        Kafka.throwProducerError = true;
        var consoleErrorSpy = jest.spyOn(console, 'error');
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', true, 'default', true, sslConfig);
        await kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic and check for error
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        expect(consoleErrorSpy).toHaveBeenCalled();
        consoleErrorSpy.mockRestore();
        await kafkaPublisher.shutdown();
    });

    // Validate retry happens in case initial connection attempts fails
    it('Validate retrial in initial connection attempt', async () => {
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', false, 'default', true, sslConfig);
        Kafka.throwConnectionError = true;
        var consoleErrorSpy = jest.spyOn(console, 'error');
        kafkaPublisher.run();
        Kafka.throwConnectionError = false;
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic and check for error
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(consoleErrorSpy).toHaveBeenCalled();
        consoleErrorSpy.mockRestore();
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));
        await kafkaPublisher.shutdown();
    });

    // Validate Producer Disconnection Error is logged before exit
    it('Validate Producer Disconnection Error is logged', async () => {
        Kafka.throwProducerDisconnectionError = true;
        var consoleErrorSpy = jest.spyOn(console, 'error');
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', true, undefined, true, sslConfig);
        kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic        
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));
        await kafkaPublisher.shutdown();
        expect(consoleErrorSpy).toHaveBeenCalled();
        expect(exitMock).toHaveBeenCalledWith(1);
        consoleErrorSpy.mockRestore();
    });

    // Validate reconnection happens post initial connection disconnection
    it('Validate successful reconnection post disconnection', async () => {
        // Check Hostname without password
        sslConfig[KafkaConstants.CONNECTION_CHECK_HOST_NAME_KEY] = 1;
        delete sslConfig[KafkaConstants.CONNECTION_CLIENT_CERT_PWD_KEY];

        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', true, undefined, true, sslConfig);
        kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        kafkaPublisher.getProducer().simulateDisconnection(true);
        // Let the disconnection run for 4 seconds so that check connection message is invoked
        await new Promise(resolve => setTimeout(resolve, 4000));
        kafkaPublisher.getProducer().simulateDisconnection(false);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));
        await kafkaPublisher.shutdown();
    }, 10000);

    // Validate message is published sccessfully by the Kafka Publisher for Non-SSL Connection
    it('Validate message is published sccessfully by the Kafka Publisher for Non-SSL Connections', async () => {
        kafkaPublisher = new KafkaPublisher('Consumer1', 'localhost', '9092', false, 'default', false, undefined);
        kafkaPublisher.run();
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 500));
        // Publish the message to the required topic        
        await kafkaPublisher.publish('detection:ppe:1', 'detection.ppe.kafka.1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 500));
        expect(kafkaConsumerReceivedMessageTopic).toBe('detection.ppe.kafka.1');
        expect(kafkaConsumerReceivedMessage).toBe(JSON.stringify(data));
        await kafkaPublisher.shutdown();
    }, 10000);
});

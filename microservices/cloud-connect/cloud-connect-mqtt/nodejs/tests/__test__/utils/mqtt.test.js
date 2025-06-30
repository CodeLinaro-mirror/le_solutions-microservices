/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- kafka.test.js
 * Description :- The test file to validate Kafka related functionality.
 */
'use strict';

import fs from 'fs';
import { connect } from "mqtt";
import { MQTTConstants } from "../../../src/constants/mqtt-constants.js";
import { MQTTPublisher } from "../../../src/utils/mqtt.js";
import { AedesBroker } from '../../__mocks__/aedesBroker.js';

jest.mock('../../../src/constants/mqtt-constants.js');

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

// MQTT In Memory SSL Broker
let mqttBroker;
// MQTT In Memory SSL Broker Port
let mqttBrokerPort = 8885;
// MQTT In Memory Non-SSL Broker
let nonSSLMQTTBroker;
// MQTT In Memory Non-SSL Broker Port
let nonSSLMQTTBroketPort = 1885;
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

// Before all tests are executed across suites setup required configurations
beforeAll(() => {
    // Create the instance of MQTT In Memory SSL Broker
    mqttBroker = new AedesBroker(mqttBrokerPort, true,
        'tests/__mocks__/server_certificates/rootca.pem.crt',
        'tests/__mocks__/server_certificates/server.pem.crt',
        'tests/__mocks__/server_certificates/server.private.pem.key');
    // Start the MQTT In Memory SSL Broker
    mqttBroker.start();

    // Create the instance of MQTT In Memory Non-SSL Broker
    nonSSLMQTTBroker = new AedesBroker(nonSSLMQTTBroketPort, false);
    // Start the MQTT In Memory Non-SSL Broker
    nonSSLMQTTBroker.start();
});

// Clean up after all the tests are executed across test suites.
afterAll(async () => {
    // Put in some delay so that MQTT Client can disconnect
    await new Promise(resolve => setTimeout(resolve, 500));
    // Stop the MQTT In Memory SSL Broker
    await mqttBroker.stop();
    // Stop the MQTT In Memory Non-SSL Broker
    await nonSSLMQTTBroker.stop();
});

// Test Suite for all MQTT Publisher Related Test Cases
describe('MQTT Publisher', () => {
    // MQTT Subscriber SSL Client
    let mqttSubscriber = 'undefined';
    // MQTT Subscriber Non-SSL Client
    let mqttNonSSLSubscriber = 'undefined';
    // Message received by Consumer
    let mqttConsumerReceivedMessage = '';
    // Topic on which Message is received
    let mqttConsumerReceivedMessageTopic = '';
    // Object to store SSL Config
    let sslConfig = {};

    // Before all tests of suite are executed do the required configurations
    // and initialize the required connections
    beforeAll(() => {
        // Create the SSL Config to be used for creating MQTT Client
        sslConfig[MQTTConstants.CONNECTION_ROOT_CA_FILE_KEY] = 'tests/__test__/certificates/rootca.pem.crt';
        sslConfig[MQTTConstants.CONNECTION_CLIENT_CERT_FILE_KEY] = 'tests/__test__/certificates/certificate.pem.crt';
        sslConfig[MQTTConstants.CONNECTION_CLIENT_KEY_FILE_KEY] = 'tests/__test__/certificates/private.pem.key';
        sslConfig[MQTTConstants.CONNECTION_CHECK_HOST_NAME_KEY] = 0;

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
        mqttSubscriber = connect(`mqtt://localhost:${mqttBrokerPort}`, mqttOptions);

        // Subscribe to the topic on connection  
        mqttSubscriber.on("connect", () => {
            mqttSubscriber.subscribe(['detection/ppe/mqtt/1', 'disconnected/topic',
                'errorneous/topic'], (error) => {
                    expect(error).toBeNull();
                });
        });

        // Callback for Received Message over subscribed topic
        mqttSubscriber.on("message", (topic, message) => {
            console.log(`Message received by MQTT Subscriber over topic ${topic} is ${message}`);
            mqttConsumerReceivedMessage = message.toString();
            mqttConsumerReceivedMessageTopic = topic;
        });

        // Create Non-SSL MQTT Subscriber Option
        var nonSSLMQTTOptions = {
            keepalive: MQTTConstants.CONNECTION_KEEPALIVE,
            protocolVersion: MQTTConstants.CONNECTION_PROTCOL_VERSION,
            connectTimeout: MQTTConstants.CONNECTION_TIMEOUT,
            reconnectPeriod: MQTTConstants.CONNECTION_RECONNECT_PERIOD,
            reconnectOnConnackError: MQTTConstants.CONNECTION_RECONNECT_ON_CONNACK_ERROR,
        };
        // Create the Non-SSL MQTT Client        
        mqttNonSSLSubscriber = connect(`mqtt://localhost:${nonSSLMQTTBroketPort}`, nonSSLMQTTOptions);

        // Subscribe to the topic on connection  
        mqttNonSSLSubscriber.on("connect", () => {
            mqttNonSSLSubscriber.subscribe(['detection/ppe/mqtt/1'], (error) => {
                expect(error).toBeNull();
            });
        });

        // Callback for Received Message over subscribed topic
        mqttNonSSLSubscriber.on("message", (topic, message) => {
            console.log(`Message received by Non-SSL MQTT Subscriber over topic ${topic} is ${message}`);
            mqttConsumerReceivedMessage = message.toString();
            mqttConsumerReceivedMessageTopic = topic;
        });
    });

    // Clean up after all the tests of suite are executed.
    afterAll(() => {
        // Disconnect the MQTT Subscriber
        mqttSubscriber.end();
        // De-initialize the SSL Config
        sslConfig = {};
        // Disconnect the Non-SSL MQTT Subscriber
        mqttNonSSLSubscriber.end();
    });

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

    // Validate message is published sccessfully by the MQTT Publisher to a subscribed topic
    it('Validate successful message publication by the MQTT Publisher to a subscribed topic', async () => {
        var mqttPublisherTest1 = new MQTTPublisher('localhost', mqttBrokerPort, false, 'default', true, sslConfig);
        var mqttClientId = mqttPublisherTest1.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest1 : ${mqttClientId}`);
        mqttPublisherTest1.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        // Publish the message to the required topic        
        mqttPublisherTest1.publish('detection:ppe:1', 'detection/ppe/mqtt/1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        mqttPublisherTest1.shutdown();
        await new Promise(resolve => setTimeout(resolve, 250));
    });

    // Validate message is not published by the MQTT Publisher to an unsubscribed topic
    it('Validate unsuccessful message publication by the MQTT Publisher to an unsubscribed topic', async () => {
        var mqttPublisherTest2 = new MQTTPublisher('localhost', mqttBrokerPort, false, 'default', true, sslConfig);
        var mqttClientId = mqttPublisherTest2.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest2 : ${mqttClientId}`);
        mqttPublisherTest2.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        // Publish the message to the required topic        
        mqttPublisherTest2.publish('unsubscribed:topic', 'unsubscribed/topic', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessageTopic).toBe('');
        expect(mqttConsumerReceivedMessage).toBe('');
        mqttPublisherTest2.shutdown();
        await new Promise(resolve => setTimeout(resolve, 250));
    });

    // Validate message is published by a reconnected MQTT Publisher
    it('Validate message is published by a reconnected MQTT Publisher', async () => {
        var mqttPublisherTest3 = new MQTTPublisher('localhost', mqttBrokerPort, true, undefined, true, sslConfig);
        var mqttClientId = mqttPublisherTest3.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest3 : ${mqttClientId}`);
        mqttPublisherTest3.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        mqttBroker.closeClientConnection(mqttClientId);
        // Wait for 5.5 seconds for reconnection to happen
        await new Promise(resolve => setTimeout(resolve, 5500));
        // Publish the message to the required topic        
        mqttPublisherTest3.publish('disconnected:topic', 'disconnected/topic', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));
        expect(mqttConsumerReceivedMessageTopic).toBe('disconnected/topic');
        mqttPublisherTest3.shutdown();
    }, 10000);

    // Validate error thrown by MQTT Publisher during message publication is logged
    it('Validate error thrown by MQTT Publisher during message publication is logged', async () => {
        var consoleErrorSpy = jest.spyOn(console, 'error');
        var mqttPublisherTest4 = new MQTTPublisher('localhost', mqttBrokerPort, true, 'default', true, sslConfig);
        var mqttClientId = mqttPublisherTest4.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest4 : ${mqttClientId}`);
        mqttPublisherTest4.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        mqttPublisherTest4.shutdown();
        // Publish the message to the required topic
        mqttPublisherTest4.publish('errorneous:topic', 'errorneous/topic', JSON.stringify(data));
        // Put in some delay to shutdown the pubslisher cleint
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(consoleErrorSpy).toHaveBeenCalled();
        consoleErrorSpy.mockRestore();
    });

    // Validate reconnection is handled in case MQTT Broker is down
    it('Validate reconnection is handled in case MQTT Broker is down', async () => {
        var consoleErrorSpy = jest.spyOn(console, 'error');
        var mqttPublisherTest5 = new MQTTPublisher('localhost', mqttBrokerPort, true, undefined, true, sslConfig);
        var mqttClientId = mqttPublisherTest5.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest5 : ${mqttClientId}`);
        mqttPublisherTest5.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        // Stop the MQTT In Memory Broker
        await mqttBroker.stop();
        // Wait for 5.5 seconds for an reconnect attempt to happen
        await new Promise(resolve => setTimeout(resolve, 5500));
        // Re-Start the server by Creating the instance of MQTT In Memory Broker
        mqttBroker = new AedesBroker(mqttBrokerPort, true,
            'tests/__mocks__/server_certificates/rootca.pem.crt',
            'tests/__mocks__/server_certificates/server.pem.crt',
            'tests/__mocks__/server_certificates/server.private.pem.key');
        // Start the recreated MQTT In Memory Broker
        mqttBroker.start();
        // Wait for 5 seconds for reconnection to be successful
        await new Promise(resolve => setTimeout(resolve, 5000));
        expect(consoleErrorSpy).toHaveBeenCalled();
        // Publish the message to the required topic        
        mqttPublisherTest5.publish('detection:ppe:1', 'detection/ppe/mqtt/1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        consoleErrorSpy.mockRestore();
        mqttPublisherTest5.shutdown();
    }, 15000);

    // Validate message is published sccessfully by the MQTT Publisher to a subscribed topic with encrypted client key
    it('Validate successful message publication by the MQTT Publisher to a subscribed topic with encrypted client key', async () => {
        // Create SSL Config with Client Certificate enabled
        var pwdSSLConfig = {};
        pwdSSLConfig[MQTTConstants.CONNECTION_ROOT_CA_FILE_KEY] = 'tests/__test__/certificates_with_password/rootca.pem.crt';
        pwdSSLConfig[MQTTConstants.CONNECTION_CLIENT_CERT_FILE_KEY] = 'tests/__test__/certificates_with_password/certificate.pem.crt';
        pwdSSLConfig[MQTTConstants.CONNECTION_CLIENT_KEY_FILE_KEY] = 'tests/__test__/certificates_with_password/private.pem.key';
        pwdSSLConfig[MQTTConstants.CONNECTION_CHECK_HOST_NAME_KEY] = 0;
        pwdSSLConfig[MQTTConstants.CONNECTION_CLIENT_CERT_PWD_KEY] = 'password';

        var mqttPublisherTest6 = new MQTTPublisher('localhost', mqttBrokerPort, false, 'default', true, pwdSSLConfig);
        var mqttClientId = mqttPublisherTest6.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest6 : ${mqttClientId}`);
        mqttPublisherTest6.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        // Publish the message to the required topic        
        mqttPublisherTest6.publish('detection:ppe:1', 'detection/ppe/mqtt/1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        mqttPublisherTest6.shutdown();
        await new Promise(resolve => setTimeout(resolve, 250));
    });

    // Validate successful message publication by the MQTT Publisher for Non-SSL subcription
    it('Validate successful message publication by the MQTT Publisher for Non-SSL subcription', async () => {
        var mqttPublisherTest7 = new MQTTPublisher('localhost', nonSSLMQTTBroketPort, false, 'default', false, undefined);
        var mqttClientId = mqttPublisherTest7.getClient().options.clientId;
        console.log(`Client Id of mqttPublisherTest7 : ${mqttClientId}`);
        mqttPublisherTest7.setClientId(mqttClientId);
        // Put in some delay to let Producer connect properly
        await new Promise(resolve => setTimeout(resolve, 250));
        // Publish the message to the required topic        
        mqttPublisherTest7.publish('detection:ppe:1', 'detection/ppe/mqtt/1', JSON.stringify(data));
        // Put in some delay to let Producer complete Publication
        await new Promise(resolve => setTimeout(resolve, 250));
        expect(mqttConsumerReceivedMessage).toBe(JSON.stringify(data));
        expect(mqttConsumerReceivedMessageTopic).toBe('detection/ppe/mqtt/1');
        mqttPublisherTest7.shutdown();
        await new Promise(resolve => setTimeout(resolve, 250));
    });
});
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- kafkajs.js
 * Description :- The utility file used for mocking the standard kafkajs.js library.
 */
'use strict';

import { logLevel } from "kafkajs";

// Mock the whole Kafkajs Module
const kafkajs = jest.genMockFromModule('kafkajs');

/**
 * Class encapsulating the Mock Producer of Kafka
 */
class Producer {
    constructor({ allowAutoTopicCreation, idempotent, sendCb, logCreator, ssl, clientId }) {
        this.allowAutoTopicCreation = allowAutoTopicCreation,
            this.idempotent = idempotent,
            this.sendCb = sendCb;
        this.connected = false;
        this.simulateDisconnectionFlag = false;
        this.logCreator = logCreator;
        this.ssl = ssl;
        this.clientId = clientId;
    }

    async connect() {
        if (kafkajs.Kafka.throwConnectionError) {
            if (this.logCreator) {
                await this.logCreator({
                    namespace: 'kafkajs',
                    level: logLevel.ERROR,
                    label: 'Connection',
                    log: { message: 'Mock Connection error: connect ECONNREFUSED', logger: 'kafkajs' }
                });
            }
            throw new Error('Mock Connection error: connect ECONNREFUSED while connection.');
        }
        if (this.ssl) {
            // Simulate SSL connection logic
            console.log(`SSL connection established for producer having Client ID: ${this.clientId}`);
        }
        this.connected = true;
        return Promise.resolve();
    }

    async send({ topic, messages }) {
        if (!this.connected) {
            if (this.logCreator) {
                await this.logCreator({
                    namespace: 'kafkajs',
                    level: logLevel.ERROR,
                    label: 'Producer',
                    log: { message: 'Producer is not connected.', logger: 'kafkajs' }
                });
            }
            throw new Error('Producer is not connected.');
        }
        if (this.simulateDisconnectionFlag == true) {
            if (this.logCreator) {
                await this.logCreator({
                    namespace: 'kafkajs',
                    level: logLevel.ERROR,
                    label: 'Connection',
                    log: { message: 'Connection timeout', logger: 'kafkajs' }
                });
            }
            throw new Error('Connection timeout');
        }
        if (kafkajs.Kafka.throwProducerError == true) {
            if (this.logCreator) {
                await this.logCreator({
                    namespace: 'kafkajs',
                    level: logLevel.ERROR,
                    label: 'Producer',
                    log: { message: `Failed to send messages to topic ${topic}: Mock Connection timeout.`, logger: 'kafkajs' }
                });
            }
            throw new Error(`Failed to send messages to topic ${topic}: Mock Connection timeout.`);
        } else {
            this.sendCb({ topic, messages });
            var response = {
                'topicName': topic,
                'partition': 0,
                'errorCode': 0,
                'logAppendTime': -1
            };
            return Promise.resolve(Array.of(response));
        }
    }

    async disconnect() {
        if (kafkajs.Kafka.throwProducerDisconnectionError == true) {
            if (this.logCreator) {
                await this.logCreator({
                    namespace: 'kafkajs',
                    level: logLevel.ERROR,
                    label: 'Producer',
                    log: { message: 'Mock Connection Error while disconnecting producer.', logger: 'kafkajs' }
                });
            }
            throw new Error('Mock Connection Error while disconnecting producer.');
        } else {
            this.connected = false;
            return Promise.resolve();
        }
    }

    simulateDisconnection(startDisconnection) {
        if (startDisconnection == true) {
            this.simulateDisconnectionFlag = true;
        } else {
            this.simulateDisconnectionFlag = false;
        }
    }

    get events() {
        return {
            CONNECT: 'producer.connect',
            DISCONNECT: 'producer.disconnect',
            REQUEST: 'producer.network.request',
            REQUEST_TIMEOUT: 'producer.network.request_timeout',
            REQUEST_QUEUE_SIZE: 'producer.network.request_queue_size',
        };
    }
}

/**
 * Class encapsulating the Mock Consumer of Kafka
 */
class Consumer {
    constructor({ groupId, subscribeCb, logCreator, ssl }) {
        this.groupId = groupId;
        this.subscribeCb = subscribeCb;
        this.connected = false;
        this.logCreator = logCreator;
        this.ssl = ssl;
    }

    getGroupId() {
        return this.groupId;
    }

    async connect() {
        if (this.ssl) {
            // Simulate SSL connection logic
            console.log('SSL connection established for Consumer');
        }
        this.connected = true;
        return Promise.resolve();
    }

    async subscribe({ topic }) {
        this.subscribeCb(topic, this);
    }

    async run({ eachMessage, topic }) {
        this.eachMessage = eachMessage;
        this.topic = topic
    }

    async disconnect() {
        this.connected = false;
        return Promise.resolve();
    }
}

/**
 * Mocking the Kafka Class
 */
kafkajs.Kafka = class Kafka {
    // Keep the list of subscribed topic as class level
    static topics = {};
    // Whether to throw error while executing producer send message
    static throwProducerError = false;
    // Whether to throw error while connecting producer/consumer
    static throwConnectionError = false;
    // Whether to throw error while disconnecting prodcuer
    static throwProducerDisconnectionError = false;

    constructor(config) {
        this.brokers = config.brokers;
        this.clientId = config.clientId;
        this.ssl = config.ssl;
        this.retry = config.retry;
        this.logCreator = config.logCreator;
    }

    // Mock subscribe method to be invoked by mock consumer
    _subscribeCb(topic, consumer) {
        Kafka.topics[topic] = Kafka.topics[topic] || {};
        const topicObj = Kafka.topics[topic];
        topicObj[consumer.getGroupId()] = topicObj[consumer.getGroupId()] || [];
        topicObj[consumer.getGroupId()].push(consumer);
    }

    // Mock send method to be invoked by mock producer
    _sendCb({ topic, messages }) {
        messages.forEach((message) => {
            if (typeof Kafka.topics[topic] !== 'undefined' && Kafka.topics[topic]) {
                Object.values(Kafka.topics[topic]).forEach((consumers) => {
                    const consumerToGetMessage = Math.floor(Math.random() * consumers.length);
                    consumers[consumerToGetMessage].eachMessage({
                        message, topic
                    });
                });
            }
        });
    }

    producer({ allowAutoTopicCreation, idempotent }) {
        return new Producer({
            allowAutoTopicCreation,
            idempotent,
            sendCb: this._sendCb.bind(this),
            logCreator: this.logCreator, // Pass logCreator to Producer
            ssl: this.ssl, // Pass SSL to Producer
            clientId: this.clientId // Pass clientId to Producer
        });
    }

    consumer({ groupId }) {
        return new Consumer({
            groupId,
            subscribeCb: this._subscribeCb.bind(this),
            logCreator: this.logCreator, // Pass logCreator to Consumer
            ssl: this.ssl // Pass SSL to Consumer
        });
    }
};

(module).exports = kafkajs;
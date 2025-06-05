/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- kafka.js
 * Description :- The utility file handling all the operations related to Kafka.
 */
'use strict';

import fs from 'fs';
import { Kafka, logLevel } from "kafkajs";
import { KafkaConstants } from "../constants/kafka-constants.js";
import * as Logger from "./logging.js";
import { CloudConnect } from "../cloud-connect.js";

/**
 * Class encapsulating the Cloud Communicator Kafka Publisher
 */
export class KafkaPublisher {
    /** 
     * @private {Object} [Kafka Producer Object of required for publishing]
     */
    #producer = undefined;
    /**
     * @private {string} [The Consumer Id for setting as key for each published message.]
     */
    #consumerId = '';
    /**
     * @private {String} [Id of Kafka Producer to be used for publishing]
     */
    #clientId = '';
    /**
     * @private {Object} [The Kafka Instance to be used for creating Producer.]
     */
    #kafka = undefined;
    /**
     * @private {boolean} [Flag to identify whether reconnection is ongoing or not.]
     */
    #isReconnecting = false;
    /**
     * @private {number} [Unique id of interval set for checking connection]
     */
    #connectionCheckIntervalId = undefined;
    /**
     * @private {boolean} [Flag to determine whether message need to publish back in case publication failure]
     */
    #publishBackMessage = false;
    /**
     * Custom logger function to capture KafkaJS errors and act upon the
     * connection related error
     * @param {number} _logLevel 
     */
    customLogger = _logLevel => async ({ namespace, level, label, log }) => {
        if (log) {
            const { message, logger, ...extra } = log;
            if (level === logLevel.ERROR) {
                Logger.log(Logger.ERROR, `[${logger}] [${label}] [${namespace}] ${message} ${JSON.stringify(extra)}`);
                // Handle connection-related errors
                if (message.includes('Connection error') || message.includes('Connection timeout')) {
                    // Invoke the connection reconnection logic if not done already
                    if (!this.#isReconnecting) {
                        // Inform about cloud client disconnection
                        CloudConnect.notifyCloudClientDisconnection(this.#clientId);
                        this.#isReconnecting = true;
                        // Attempt to reconnect
                        await this.connect();
                    }
                }
            }
        }
    };

    /**
     * Initialises the Kafka producer which the communicator 
     * will use to publish to a topic.
     * 
     * @param {string} consumerId [Consumer Id to be send with each message the instance will publish.]
     * @param {string} kafkaBrokerHosts [Comma separated list DNS or IP of the Kafka Brokers for making connection.]
     * @param {string} kafkaBrokerPort [Port of the Kafka Broker for making connection.]
     * @param {boolean} publishBackMessage [Flag to decide whether to publish back message to Redis.]
     * @param {string} clientIdPrefix [The Prefix string to be used in client Id.]
     * @param {boolean} isSSLEnabled [Flag to decide whether SSL Conenction is enabled or not.]
     * @param {object} sslConfig [Object containing SSL related configurtaion values.]
     */
    constructor(consumerId, kafkaBrokerHosts, kafkaBrokerPort, publishBackMessage,
        clientIdPrefix, isSSLEnabled, sslConfig) {
        // Set the flag to decide whether message needs to be pushed back in Redis or not
        this.#publishBackMessage = publishBackMessage;
        // Set the Consumer Id the instance will use for publishing message
        this.#consumerId = consumerId;
        // Set the Producer Client Id if provided
        if (typeof clientIdPrefix != 'undefined' && clientIdPrefix.trim() != '') {
            this.#clientId = clientIdPrefix;
        } else {
            this.#clientId = `${KafkaConstants.CONNECTION_CLIENT_ID}_${Math.random().toString(16).slice(2, 10)}`;
        }
        // Prepare Kafka Broker Array
        var provideKafkaBrokerHostNames = kafkaBrokerHosts.split(',');
        var kafkaBrokers = [];
        provideKafkaBrokerHostNames.forEach((broketHost) => {
            kafkaBrokers.push(`${broketHost.trim()}:${kafkaBrokerPort}`);
        });
        // Initialising kafka instance requirements
        var kafkaConfig = {
            clientId: this.#clientId,
            brokers: kafkaBrokers,
            retry: {
                initialRetryTime: KafkaConstants.CONNECTION_INITIAL_RETRY_TIME, //The initial retry delay in milliseconds
                retries: Number.MAX_SAFE_INTEGER, // Maximum number of retries
                multiplier: KafkaConstants.CONNECTION_BACKOFF_MULTIPLIER, //Backoff multiplier
                factor: KafkaConstants.CONNECTION_RANDOMIZATION_FACTOR, // Randomization Factor
                maxRetryTime: KafkaConstants.CONNECTION_MAXIMUM_RETRY_DELAY // Maximum retry delay in milliseconds
            },
            logCreator: this.customLogger, // Use the custom logger
            connectionTimeout: KafkaConstants.CONNECTION_TIMEOUT_INTERVAL
        };
        if (isSSLEnabled) {
            // Create the SSL Context Object
            let sslContext = {
                ca: [fs.readFileSync(`${sslConfig[KafkaConstants.CONNECTION_ROOT_CA_FILE_KEY]}`, 'utf-8')],
                cert: fs.readFileSync(`${sslConfig[KafkaConstants.CONNECTION_CLIENT_CERT_FILE_KEY]}`, 'utf-8'),
                key: fs.readFileSync(`${sslConfig[KafkaConstants.CONNECTION_CLIENT_KEY_FILE_KEY]}`, 'utf-8')
            };
            // Set Client Key Password if provided
            var clientKeyPassword = sslConfig[KafkaConstants.CONNECTION_CLIENT_CERT_PWD_KEY];
            if (typeof clientKeyPassword != 'undefined' && clientKeyPassword.trim() != '') {
                sslContext.passphrase = clientKeyPassword.trim();
            }
            // Disable Host Name Check if flag is false
            if (!Boolean(Number(sslConfig[KafkaConstants.CONNECTION_CHECK_HOST_NAME_KEY]))) {
                sslContext.checkServerIdentity = () => { return null; };
            }
            kafkaConfig.ssl = sslContext;
        }
        // The Kafka Class instance to be used for creating publishers
        this.#kafka = new Kafka(kafkaConfig);
        // Create the Kafka Producer
        this.#producer = this.#kafka.producer({
            allowAutoTopicCreation: true,
            idempotent: true,
        });
    }

    /**
     * Getter method for Kafka Producer's Client Id.
     * 
     * @returns {string} [The id with which Kafka Producer is initialised.]
     */
    getClientId() {
        return this.#clientId;
    }

    /**
     * Getter method for created Kafka Producer.
     * 
     * @returns {Object} [The created Kafka Producer]
     */
    getProducer() {
        return this.#producer;
    }

    /**
     * Method for creating producer connection with retry logic.
     * 
     * @param {number} attempt [Number of attempt made to connect with initial value 1.]
     * @returns {boolean} [Flag signifying whether producer got connected successfully or not.]
     */
    async connect(attempt = 1) {
        // Try Producer connection
        try {
            await this.#producer.connect();
            if (this.#isReconnecting) {
                // Reconnect Connection was successful
                this.#isReconnecting = false;
                // Send the resume intimation request post connection.
                CloudConnect.notifyCloudClientReconnection(this.#clientId);
                Logger.log(Logger.INFO, `Kafka Producer ${this.#clientId} reconnected successfully.`);
            } else {
                // Fresh Connection was successful
                Logger.log(Logger.INFO, `Kafka Producer Connection successful for consumer ${this.#consumerId}.`);
            }
            return true;
        } catch (error) {
            Logger.log(Logger.ERROR, `Error while connecting the producer for consumer ${this.#consumerId}: ${error}-${error.stack}`);
        }

        // Calculate next time interval for retrying connection
        const nextRetry = KafkaConstants.CONNECTION_INITIAL_RETRY_TIME * Math.pow(KafkaConstants.CONNECTION_BACKOFF_MULTIPLIER, attempt);
        const retryDelay = Math.min(nextRetry, KafkaConstants.CONNECTION_MAXIMUM_RETRY_DELAY);
        Logger.log(Logger.DEBUG, `Retrying connection in ${retryDelay} ms...`);

        // Wait for calculated interval before retrying connection
        await new Promise(resolve => setTimeout(resolve, retryDelay));

        // Recursive call with incremented attempt count
        return this.connect(attempt + 1);
    }

    /**
     * The method which will be invoked to publish message to the topic.
     * The method will be invoked by cloud communicator subscriber.
     * 
     * @param {string} sourceTopic [The Topic name originally on which the message was received.]
     * @param {string} topicToPublish [Exact Name of the topic to publish the message]
     * @param {string} msgToPublish [Message to publish over topic]
     */
    async publish(sourceTopic, topicToPublish, msgToPublish) {
        try {
            await this.#producer.send({
                topic: topicToPublish,
                messages: [{
                    key: this.#consumerId,
                    value: msgToPublish
                }]
            }).then(response => {
                Logger.log(Logger.DEBUG, `Response on successful publishing of message to topic ${topicToPublish} :`);
                for (const key in response[0]) {
                    Logger.log(Logger.DEBUG, `${key} : ${response[0][key]}`);
                }
            });
        } catch (error) {
            Logger.log(Logger.ERROR, `Error while publishing message to topic ${topicToPublish} : ${error.message}\n${error.stack}`);
            // If enabled push back the message back to Redis on the topic it was received
            if (this.#publishBackMessage && typeof sourceTopic != 'undefined') {
                CloudConnect.publishMessageBackToRedis(sourceTopic, msgToPublish);
            }
            if (error.message.includes('Connection error') || error.message.includes('Connection timeout')) {
                if (!this.#isReconnecting) {
                    // Inform about cloud client disconnection
                    CloudConnect.notifyCloudClientDisconnection(this.#clientId);
                    this.#isReconnecting = true;
                    // Attempt to reconnect
                    await this.connect();
                }
            }
        }
    }

    /**
     * Function to scheduled for checking whether producer's connection in
     * entact or not.
     */
    async checkConnection() {
        await this.publish(undefined, `${this.#clientId}.health.check`, 'ping');
    };

    /**
     * Function to invoke Producer's fresh connection
     */
    async run() {
        await this.connect();
        // Start connection health check at fixed interval
        this.#connectionCheckIntervalId = setInterval(this.checkConnection.bind(this), KafkaConstants.CONNECTION_HEALTH_CHECK_INTERVAL); // Check connection every fixed interval
    }

    /**
     * The method to be used for disconnecting the Kafka Producer
     */
    async shutdown() {
        try {
            // Remove the mapping of cloud client from Main Invocation Class
            CloudConnect.removeCloudClientSubcriberMapping(this.#clientId);
            // Clear the interval set for checking connection
            clearInterval(this.#connectionCheckIntervalId);
            await this.#producer.disconnect();
        } catch (error) {
            Logger.log(Logger.ERROR, `Error while disconnecting the producer for consumer ${this.#consumerId}: ${error}`);
            process.exit(1);
        }
    }
}
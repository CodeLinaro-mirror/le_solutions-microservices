/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect.js
 * Description :- The file describe the cloud connect service. The Service 
 *                subscribes to the configured topic to read the data and 
 *                subsequently publishes it to cloud topic.
 */
'use strict';

import { CloudConnectConstants } from "./constants/cloud-connect-constants.js";
import { KafkaConstants } from "./constants/kafka-constants.js";
import { RedisConstants } from "./constants/redis-constants.js";
import * as Logger from "./utils/logging.js";
import { KafkaPublisher } from "./utils/kafka.js";
import { RedisSubscriber } from "./utils/redis.js";
import { CloudConnectRedis } from "./utils/cloud-connect-redis.js";
import { parseJsonFile, ValidationError } from "./utils/utility.js";

/**
 * Class encapsulating the Cloud Connect Service
 */
export class CloudConnect {

    /**
     * @static {Object} [Static instance of Cloud Connect Redis Client for Cloud Connect level Redis Operation.]
     */
    static cloudConnectRedisClient = undefined;
    /** 
     * @static {Map} [Map containing the mapping between Cloud Client Id and Initialized Subscriber.]
     */
    static cloudClientToInitializedSubscriber = new Map();

    /**
     * The function to be invoked by Cloud Client for publishing message back to source topic in Redis
     * 
     * @param {string} topicToPublishMessage [Redis topic on which message needs to be publish back.]
     * @param {string} messageToPublishBack [The message which need to be publish back.]
     */
    static publishMessageBackToRedis(topicToPublishMessage, messageToPublishBack) {
        (async () => {
            Logger.log(Logger.DEBUG, `Going to publish message back to Redis Topic ${topicToPublishMessage}`);
            await CloudConnect.cloudConnectRedisClient.publish(topicToPublishMessage, messageToPublishBack);
        })();
    }

    /**
     * The Utility Function to initiate the Subscription of an
     * initialized Subscriber
     * 
     * @param {Object} initializedSubscriber [Instance of an Initialized Subscriber]
     */
    static initiateSubscription(initializedSubscriber) {
        (async () => {
            // Subscribe to the required channel.
            await initializedSubscriber.subscribe();
        })();
    }

    /**
     * The Utility Function to remove the Subscription of an
     * initialized Subscriber
     * 
     * @param {Object} initializedSubscriber [Instance of an Initialized Subscriber]
     */
    static removeSubscription(initializedSubscriber) {
        (async () => {
            // Subscribe to the required channel.
            await initializedSubscriber.disconnectSubscriber();
        })();
    }

    /**
     * The Utility function which will be invoked in case a cloud client is disconnected.
     * The function will invoke the respective subscriber to pause the subscription
     * 
     * @param {String} cloudClientId [The Id of Cloud Client whose disconnection need to be notified.]
     */
    static notifyCloudClientDisconnection(cloudClientId) {
        if (CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId) != undefined) {
            Logger.log(Logger.DEBUG, `Invoke Subscriber Pause request for client id ${cloudClientId}`);
            // Pause the subcriber as Cloud Client is disconnected
            CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId).forEach((subscriber, index) => {
                subscriber.pauseSubscriber();
            });
        }
    }

    /**
     * The Utility function which will be invoked in case a cloud client is re-connected.
     * The function will invoke the respective subscriber to resume the subscription
     * 
     * @param {String} cloudClientId [The Id of Cloud Client whose reconnection need to be notified.]
     */
    static notifyCloudClientReconnection(cloudClientId) {
        if (CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId) != undefined) {
            Logger.log(Logger.DEBUG, `Invoke Subscriber Resume request for client id ${cloudClientId}`);
            // Resume the subcriber as Cloud Client has reconnected
            CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId).forEach((subscriber, index) => {
                subscriber.resumeClient();
            });
        }
    }

    /**
     * The Utility function which will be used for removing the mapping between Cloud Client
     * and its respective Subscriber Instance
     * 
     * @param {String} cloudClientId [The Id of Cloud Client whose mapping needs to be removed.]
     */
    static removeCloudClientSubcriberMapping(cloudClientId) {
        CloudConnect.cloudClientToInitializedSubscriber.delete(cloudClientId);
    }

    /**
     * @private {Object} [Object for storing broker details]
     */
    #brokerObject = {};

    /**
     * @private {Object} [JSON Object for storing configuration data]
     */
    #configObject = undefined;

    /** 
     * @private {Array} [Array containing the initialized Subscriber.]
     */
    #initializedSubscriber = [];
    /**
     * Initialises the Cloud Connect Service by loading the configuration mentioned
     * in the provided Configuration JSON File.
     * 
     * @param {string} configJSONFile [The path of the JSON Configuration File.]
     */
    constructor(configJSONFile) {
        // Retrieve Redis and Broker host and port from environment variable
        this.#brokerObject[RedisConstants.CONFIG_HOST_KEY] = process.env.REDIS_HOST;
        this.#brokerObject[RedisConstants.CONFIG_PORT_KEY] = process.env.REDIS_PORT;
        this.#brokerObject[RedisConstants.CONFIG_PASSWORD_KEY] = process.env.REDIS_PASSWORD;
        this.#brokerObject[KafkaConstants.CONFIG_HOST_KEY] = process.env.BROKER_HOST;
        this.#brokerObject[KafkaConstants.CONFIG_PORT_KEY] = process.env.BROKER_PORT;

        // Validate environment variable REDIS_HOST is set.
        if (typeof this.#brokerObject[RedisConstants.CONFIG_HOST_KEY] == 'undefined'
            || String(this.#brokerObject[RedisConstants.CONFIG_HOST_KEY]).trim() == '') {
            throw new ValidationError("Environment variable REDIS_HOST is not set.");
        }

        // Validate environment variable REDIS_PORT is set.
        if (typeof this.#brokerObject[RedisConstants.CONFIG_PORT_KEY] == 'undefined'
            || String(this.#brokerObject[RedisConstants.CONFIG_PORT_KEY]).trim() == '') {
            throw new ValidationError("Environment variable REDIS_PORT is not set.");
        }

        // Validate environment variable BROKER_HOST is set.
        if (typeof this.#brokerObject[KafkaConstants.CONFIG_HOST_KEY] == 'undefined'
            || String(this.#brokerObject[KafkaConstants.CONFIG_HOST_KEY]).trim() == '') {
            throw new ValidationError("Environment variable BROKER_HOST is not set.");
        }

        // Validate environment variable BROKER_HOST is set.
        if (typeof this.#brokerObject[KafkaConstants.CONFIG_PORT_KEY] == 'undefined'
            || String(this.#brokerObject[KafkaConstants.CONFIG_PORT_KEY]).trim() == '') {
            throw new ValidationError("Environment variable BROKER_PORT is not set.");
        }
        // Read the config JSON file to get configuration details and 
        // set it in the configuration object
        this.#configObject = parseJsonFile(configJSONFile);

        // Set maximum allowed log level as per the configuration
        process.env.LOG_LEVEL = this.#configObject[Logger.CONFIG_LOG_LEVEL_KEY];
    }

    /**
     * The function initializes Redis Subscriber based on the provided configuration.
     * Each initialized subscriber along with reading data from subscribed topic, 
     * send it to respective topic of cloud communicator.
     */
    initialiseSubscribers() {
        var subscriberIndex = 0;
        // Parse the configuration json to get list of topic to subcribe along with
        // respective topic to publish.
        var redisConsumersArray = this.#configObject[RedisConstants.CONFIG_CONSUMERS_KEY];
        for (var index = 0; index < redisConsumersArray.length; index++) {
            var pubSubObject = redisConsumersArray[index];
            var subscriberTopicArray = pubSubObject[RedisConstants.CONFIG_SOURCE_TOPIC_KEY];
            var publisherTopicArray = pubSubObject[RedisConstants.CONFIG_DESTINATION_TOPIC_KEY];
            var messageErrorHandleFlag = Boolean(Number(this.#configObject[CloudConnectConstants.MESSAGE_ERROR_HANDLE_FLAG_KEY]));
            Logger.log(Logger.DEBUG, `Message Error handling flag read from config is : ${messageErrorHandleFlag}`);
            var clientIdPrefix = this.#configObject[KafkaConstants.CONNECTION_ID_PREFIX_KEY];
            var isSSLEnabled = Boolean(Number(this.#configObject[KafkaConstants.CONNECTION_ENABLE_SSL_KEY]));
            var sslConfig = this.#configObject[KafkaConstants.CONNECTION_SSL_CONFIG_KEY];
            // Modify the client id in case it is provided
            if (typeof clientIdPrefix != 'undefined' && clientIdPrefix.trim() != '') {
                // Get first 10 characters in case it prefix is bigger than 10 characters
                if (clientIdPrefix.trim().length > CloudConnectConstants.CLIENT_PREFIX_MAX_ALLOWED_LENGTH) {
                    clientIdPrefix = clientIdPrefix.substring(0, CloudConnectConstants.CLIENT_PREFIX_MAX_ALLOWED_LENGTH);
                }
                clientIdPrefix = `${clientIdPrefix}_${index}`;
            }

            // Create the instance of Cloud communicator
            var cloudClient = new KafkaPublisher(`Consumer${index + 1}`, this.#brokerObject[KafkaConstants.CONFIG_HOST_KEY],
                this.#brokerObject[KafkaConstants.CONFIG_PORT_KEY], messageErrorHandleFlag, clientIdPrefix,
                isSSLEnabled, sslConfig);
            // Initiate the connection of Cloud communicator
            cloudClient.run();
            var cloudClientId = cloudClient.getClientId();
            for (var topicIndex = 0; topicIndex < subscriberTopicArray.length; topicIndex++) {
                // Create instance of Subscriber class for subscribing to required channels.
                this.#initializedSubscriber[subscriberIndex] =
                    new RedisSubscriber(this.#brokerObject[RedisConstants.CONFIG_HOST_KEY],
                        this.#brokerObject[RedisConstants.CONFIG_PORT_KEY],
                        this.#brokerObject[RedisConstants.CONFIG_PASSWORD_KEY]);
                this.#initializedSubscriber[subscriberIndex].setTopicToSubscribe(subscriberTopicArray[topicIndex]);
                this.#initializedSubscriber[subscriberIndex].setTopicToPublish(publisherTopicArray[topicIndex]);
                this.#initializedSubscriber[subscriberIndex].setCloudClient(cloudClient);
                Logger.log(Logger.INFO, `Processing ${subscriberTopicArray[topicIndex]} : ${publisherTopicArray[topicIndex]}`);
                // Once initialized add the subscriber to Cloud Client and Initialized Subcriber Mapping
                var cloudClientSubscriberList = [];
                if (CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId) != undefined) {
                    cloudClientSubscriberList = CloudConnect.cloudClientToInitializedSubscriber.get(cloudClientId);
                }
                cloudClientSubscriberList.push(this.#initializedSubscriber[subscriberIndex]);
                CloudConnect.cloudClientToInitializedSubscriber.set(cloudClientId, cloudClientSubscriberList);
                subscriberIndex++;
            }
        }
        // Post successful initialized, initialize the Cloud Connect Redis Client also
        // TODO: Move this initialization back to index.js once we have environment variable available for Redis Broker
        Logger.log(Logger.DEBUG, `Going to create Cloud Connect Redis Client.`);
        CloudConnect.cloudConnectRedisClient = new CloudConnectRedis(this.#brokerObject[RedisConstants.CONFIG_HOST_KEY],
            this.#brokerObject[RedisConstants.CONFIG_PORT_KEY],
            this.#brokerObject[RedisConstants.CONFIG_PASSWORD_KEY]);
    }

    /**
     * Getter method for the array/list of Initialized Subscriber
     * 
     * @returns {Array} [The array/list containing initialized subscriber of cloud connect service.]
     */
    getInitializedSubscriber() {
        return this.#initializedSubscriber;
    }
}
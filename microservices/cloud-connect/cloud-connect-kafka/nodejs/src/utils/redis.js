/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- redis.js
 * Description :- The utility file handling all the operations related to Redis Consumer's Subscriber.
 */
'use strict';

import { createClient } from 'redis';
import { RedisConstants } from "../constants/redis-constants.js";
import * as Logger from "./logging.js";
import { retrieveTopicNameToPublish } from "./utility.js";

/**
 * Class encapsulating the Cloud Communicator Redis Subscriber
 */
export class RedisSubscriber {
    /** 
     * @private {Object} [Redis Client Object of Subscriber]
     */
    #client = undefined;
    /** 
     * @private {string} [Topic to which subscriber subscribes. It can be exact name or regular expression.]
     */
    #topicToSubscribe = '';
    /** 
     * @private {string} [Topic to which subscriber will publish. It can be exact name or regular expression.]
     */
    #topicToPublish = '';
    /**
     * @private {Object} [The instance of cloud client which will be used for publishing message.]
     */
    #cloudClient = undefined;
    /**
     * @private {boolean} [Flag to indicate whether the Subscriber is paused or not.]
     */
    #isPaused = false;
    /**
     * @private {boolean} [Flag to indicate whether the topic has been subscribed to at least once.]
     */
    #isSubscribed = false;
    /**
     * Initialises the Redis Client, with respective callbacks, which
     * the subscriber will use to subscribe to a topic
     * 
     * @param {string} redisServerHost [DNS or IP of the Redis Server for making connection.]
     * @param {string} redisServerPort [Port of the Redis Server for making connection.]
     * @param {string} redisServerPassword [Password(in plain text) of the Redis Server for making connection.]
     */
    constructor(redisServerHost, redisServerPort, redisServerPassword) {
        // Create the Subscriber Redis Client
        this.#client = createClient({
            url: `redis://${redisServerHost}:${redisServerPort}`,
            password: redisServerPassword,
            socket: {
                connectTimeout: RedisConstants.CONNECTION_TIMEOUT, // Setting Timeout in milliseconds
                reconnectStrategy: function (retries) {
                    Logger.log(Logger.DEBUG, `Attempting to reconnect to Redis. Retry count: ${retries}`);
                    return RedisConstants.CONNECTION_RETRY_INTERVAL;
                }
            }
        });

        // Listener for successful Redis Subscriber connection
        this.#client.on('connect', async () => {
            Logger.log(Logger.INFO, `Redis Subscriber connection successful for topic ${this.#topicToSubscribe}.`);
            // Resubscribe to the topic after reconnecting if it was subscribed before
            if (this.#isSubscribed) {
                Logger.log(Logger.DEBUG, `Reconnected and resubscribed to Redis for topic ${this.#topicToSubscribe}.`);
            } else {
                Logger.log(Logger.DEBUG, `Fresh connection to Redis for topic ${this.#topicToSubscribe}.`);
            }
        });

        // Listener for errors in the Redis Subscriber client
        this.#client.on('error', error => {
            Logger.log(Logger.ERROR, `Redis Subscriber client error for topic ${this.#topicToSubscribe} : ${error}-${error.stack}`);
        });

        // Listener for reconnection attempts
        this.#client.on('reconnecting', () => {
            Logger.log(Logger.INFO, `Redis Subscriber attempting to reconnect for topic ${this.#topicToSubscribe}.`);
        });
    }

    /**
     * Initialise the topic to which subscriber will subscribe
     * 
     * @param {string} topicToSubscribe [Name or regular expression of topic to subscribe.]
     */
    setTopicToSubscribe(topicToSubscribe) {
        this.#topicToSubscribe = topicToSubscribe;
    }

    /**
     * Initialise the topic to which subscriber will publish
     * 
     * @param {string} topicToPublish [Name or regular expression of topic to publish.]
     */
    setTopicToPublish(topicToPublish) {
        this.#topicToPublish = topicToPublish;
    }

    /**
     * Initialise the cloud client instance
     * 
     * @param {Object} cloudClient [Instance of cloud client to be used in publishing message.] 
     */
    setCloudClient(cloudClient) {
        this.#cloudClient = cloudClient;
    }

    /**
     * Get the Initialised cloud client instance
     * 
     * @return {Object} [Initailized cloud client instance to be used in publishing message.] 
     */
    getCloudClient() {
        return this.#cloudClient;
    }

    /**
     * The method will be invoked to disconnect the Subscriber
     * from the client.
     */
    async disconnectSubscriber() {
        // Disconnect the Redis Subscriber Client
        if (this.#client != undefined) {
            await this.#client.pUnsubscribe();
            this.#isSubscribed = false;
            await this.#client.disconnect();
            this.#client = undefined;
        }
        // Disconnect the Cloud Client
        if (this.#cloudClient != undefined) {
            this.#cloudClient.shutdown();
            this.#cloudClient = undefined;
        }
    }

    /**
     * Function to subscribe to the topic and set up the handler for the message
     * received over the subscribed topic
     */
    async #subscribeToTopic() {
        Logger.log(Logger.INFO, `Going to subscribe to the topic ${this.#topicToSubscribe}`);
        await this.#client.pSubscribe(this.#topicToSubscribe, async (message, channel) => {
            const checkClientIsPaused = async () => {
                if (!this.#isPaused) {
                    Logger.log(Logger.DEBUG, `Channel ${channel} sent message: \n ${message}`);
                    // Publish the message to cloud connect service as it is.
                    this.#cloudClient.publish(channel, retrieveTopicNameToPublish(channel, this.#topicToPublish, this.#topicToSubscribe), message);
                } else {
                    setTimeout(checkClientIsPaused, 200); // Check again after few milliseconds
                }
            };
            checkClientIsPaused();
        });
        this.#isSubscribed = true; // Set the flag to true after subscribing
    }

    /**
     * The method which will be invoked to subscribe to the topic.
     * The method will also initialise the callback which will be
     * invoked on receipt of message.
     */
    async subscribe() {
        // Connect the redis client
        await this.#client.connect();
        await this.#subscribeToTopic();
    }

    /**
    * Function to pause the subcriber from reading any further message from subscribed topic
    */
    pauseSubscriber() {
        Logger.log(Logger.DEBUG, `Going to pause the subscriber for the topic ${this.#topicToSubscribe}.`);
        this.#isPaused = true;
    }

    /**
     * Function to make subscriber resume reading message from the subscribed topic.
     */
    async resumeClient() {
        Logger.log(Logger.DEBUG, `Going to resume the subscriber for the topic ${this.#topicToSubscribe}.`);
        this.#isPaused = false;
    }
}

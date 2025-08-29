/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- cloud-connect-redis.js
 * Description :- The utility file handling all the Cloud Connect Application level Redis transaction
 *                instead of individual Redis Consumer level like subscribing to configuration update
 *                channel etc.
 */
'use strict';

import { createClient } from 'redis';
import { RedisConstants } from "../constants/redis-constants.js";
import * as Logger from "./logging.js";

export class CloudConnectRedis {
    /** 
     * @private {Object} [Redis Client Object of Cloud Connect]
     */
    #cloudConnectRedisClient = undefined;
    /**
     * @private {boolean} [Flag to indicate whether the Redis Client is connected or not atleast once.]
     */
    #isConnectedOnce = false;
    /**
     * Initialises the Cloud Connect Redis Client, with respective callbacks
     * 
     * @param {string} redisServerHost [DNS or IP of the Redis Server for making connection.]
     * @param {string} redisServerPort [Port of the Redis Server for making connection.]
     * @param {string} redisServerPassword [Password(in plain text) of the Redis Server for making connection.]
     */
    constructor(redisServerHost, redisServerPort, redisServerPassword) {
        // Create the Cloud Connect Redis Client
        this.#cloudConnectRedisClient = createClient({
            url: `redis://${redisServerHost}:${redisServerPort}`,
            password: redisServerPassword,
            socket: {
                connectTimeout: RedisConstants.CONNECTION_TIMEOUT, // Setting Timeout in milliseconds
                reconnectStrategy: function (retries) {
                    Logger.log(Logger.DEBUG, `Attempting to reconnect Cloud Connect Redis Client. Retry count: ${retries}`);
                    return RedisConstants.CONNECTION_RETRY_INTERVAL;
                }
            }
        });

        // Listener for successful Cloud Connect Redis Client connection
        this.#cloudConnectRedisClient.on('connect', async () => {
            Logger.log(Logger.INFO, `Cloud Connect Redis Client connection successful.`);
            // Resubscribe to the topic after reconnecting if it was subscribed before
            if (this.#isConnectedOnce) {
                Logger.log(Logger.DEBUG, `Reconnected Cloud Connect Redis Client.`);
            } else {
                this.#isConnectedOnce = true;
                Logger.log(Logger.DEBUG, `Cloud Connect Redis Client connected for first time.`);
            }
        });

        // Listener for errors in the Redis Subscriber client
        this.#cloudConnectRedisClient.on('error', error => {
            Logger.log(Logger.ERROR, `Error in Cloud Connect Redis Client : ${error}-${error.stack}`);
        });

        // Listener for reconnection attempts
        this.#cloudConnectRedisClient.on('reconnecting', () => {
            Logger.log(Logger.INFO, `Reconnect attempt happening for Cloud Connect Redis Client.`);
        });
    }

    getCloudConnectRedisClient() {
        return this.#cloudConnectRedisClient;
    }

    /**
     * The method which will be invoked to initialize Cloud Connect Redis Client.
     * The method will create a redis client with provided parameters.
     */
    async initialize() {
        // Connect the redis client
        await this.#cloudConnectRedisClient.connect();
    }

    /**
     * Function to publish given message to provided topic
     * 
     * @param {string} topicToPublish [Redis Topic on which message needs to be published.]
     * @param {string} messageToPublish [The message which needs to be published.] 
     */
    async publish(topicToPublish, messageToPublish) {
        await this.#cloudConnectRedisClient.publish(topicToPublish, messageToPublish).then((subscriberCount) => {
            Logger.log(Logger.DEBUG, `Successfully published message via Cloud Connect Client to topic ${topicToPublish} and Subscribers ${subscriberCount}`);
        }).catch((error) => {
            Logger.log(Logger.ERROR, `Error while publishing message to topic ${topicToPublish} in Cloud Connect Redis Client: ${error}-${error.stack}`);
        });
    }

    /**
     * The method will be invoked to de-initialize Cloud Connect
     * Redis Client.
     */
    async deInitialize() {
        // Disconnect the Cloud Connect Redis Client
        if (this.#cloudConnectRedisClient != undefined) {
            await this.#cloudConnectRedisClient.pUnsubscribe();
            this.#isConnectedOnce = false;
            await this.#cloudConnectRedisClient.disconnect();
        }
    }
}
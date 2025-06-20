/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- index.js
 * Description :- The file responsible for initializing the Cloud Connect
 *                Service along with its required configuration.
 */
'use strict';

import { CloudConnectConstants } from "./constants/cloud-connect-constants.js";
import { CloudConnect } from "./cloud-connect.js";
import * as Logger from "./utils/logging.js";

try {
    // Initialize the Cloud Connect Service with Configuration JSON
    var cloudClient = new CloudConnect(CloudConnectConstants.CONFIG_FILE_NAME);

    // Initialize the Subscriber of the Cloud Connect Service
    cloudClient.initialiseSubscribers();
    var initializedSubscriberArray = cloudClient.getInitializedSubscriber();
    Logger.log(Logger.INFO, `Number of Subscriber Initialized : ${initializedSubscriberArray.length}`);

    // Once Intialized, initiate the Subscription
    initializedSubscriberArray.forEach((subscriber) => {
        CloudConnect.initiateSubscription(subscriber);
    });
    // Initialize the Cloud Connect Redis Client
    await CloudConnect.cloudConnectRedisClient.initialize();

    /**
     * The listener function to be invoked for Graceful exit of the process.
     * 
     * @param {string} signalType [Signal Type which invoked the listener function.]
     */
    function gracefulExit(signalType) {
        Logger.log(Logger.INFO, `${signalType} caught!!`);
        initializedSubscriberArray.forEach((subscriber) => {
            CloudConnect.removeSubscription(subscriber);
        });
        // Clean up the Client Id to Initialized Subcriber Map
        CloudConnect.cloudClientToInitializedSubscriber.clear();
        // De-initialize the Cloud Connect Redis Client
        (async () => {
            await CloudConnect.cloudConnectRedisClient.deInitialize();
            CloudConnect.cloudConnectRedisClient = undefined;
        })();
        // After triggering removal of subscription 
        // exit the process with exit status 0
        Logger.log(Logger.INFO, 'Exiting the process with exit status 0.');
        process.exit(0);
    }
    // Listen to SIGTERM Signal sent for Process Closure
    process.on('SIGTERM', () => {
        gracefulExit('SIGTERM');
    });
    // Listen to SIGINT (Ctrl + C) Signal sent for Process Closure
    process.on('SIGINT', () => {
        gracefulExit('SIGINT');
    });
} catch (error) {
    Logger.log(Logger.ERROR, `Error in index.js while initializing Cloud Connect Service : ${error.message} \n ${error.stack}`);
}
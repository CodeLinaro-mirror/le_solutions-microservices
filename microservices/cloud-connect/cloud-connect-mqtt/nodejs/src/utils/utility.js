/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- utility.js
 * Description :- The utility file handling all general utility functions.
 */
'use strict';

import { readFileSync } from 'fs';
import * as Logger from "./logging.js";

/**
 * Class representing the Custom Validation Error
 */
export class ValidationError extends Error {
    /**
     * Initialize the custom validation error
     * 
     * @param {string} message [The error message]
     */
    constructor(message) {
        // Invoke the super class constructor
        super(message);
        // Set the name property fo the error
        this.name = "ValidationError";
        // Omit all of the stack frames invoked by code inside the class itself
        Error.captureStackTrace(this, ValidationError);
    }
}

/**
 * The wrapper function to be used by application for logging.
 * Default value returned is DEBUG
 * 
 * @param {string} jsonFilePath [The path JSON File. Path should be relative to location of calling function]
 * @returns {Object} [The parsed JSON Object.]
 */
export function parseJsonFile(jsonFilePath) {
    // Read the JSON file
    var jsonData = readFileSync(jsonFilePath, 'utf8');
    Logger.log(Logger.INFO, `Data read from JSON File :\n ${jsonData}`);
    // Parse the read JSON in the object
    var jsonObject = JSON.parse(jsonData);
    return jsonObject;
}

/**
  * The method will be invoked to get the topic name of cloud communicator on
  * which message needs to be published.
  * 
  * @param {string} receivedChannelName [Name of the Redis Channel from which message was received.]
  * @param {string} topicToPublish [Publish topic name as provided in the configuration.]
  * @param {string} topicToSubscribe [Subscription topic name as provided in the configuration.]
  * @returns {string} [The absolute name of the topic to which message needs to be published.]
  */
export function retrieveTopicNameToPublish(receivedChannelName, topicToPublish, topicToSubscribe) {
    var topicNameToReturn = '';
    // Check if Publish Topic name contains * or not and proceed accordingly.
    if (topicToPublish.includes('*')) {
        // Get the Prefix of subscribed topic.
        var subscribedTopicPrefix = topicToSubscribe.slice(0, topicToSubscribe.length - 1);
        // Remove the Prefix from the Channel name on which message was received.
        var channelNameWithoutPrefix = receivedChannelName.replace(subscribedTopicPrefix, '');
        // Append or replace the * from topic name to get absolute publish topic name.
        topicNameToReturn = topicToPublish.replace('*', channelNameWithoutPrefix);
    } else {
        // In case there is no * in name then return the name of the topic as it is
        topicNameToReturn = topicToPublish;
    }
    Logger.log(Logger.DEBUG, `Topic to be used for publishing message ${topicNameToReturn}`);
    return topicNameToReturn;
}
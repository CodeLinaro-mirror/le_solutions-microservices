/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- mqtt.js
 * Description :- The utility file handling all the operations related to MQTT.
 */
'use strict';

import fs from 'fs';
import { connect } from "mqtt";
import { MQTTConstants } from "../constants/mqtt-constants.js";
import * as Logger from "./logging.js";
import { CloudConnect } from "../cloud-connect.js";

/**
 * Class encapsulating the Cloud Comunicator MQTT Publisher
 */
export class MQTTPublisher {
    /** 
     * @private {Object} [MQTT Client Object of Publishing]
     */
    #client = undefined;
    /**
     * @private {String} [Id of MQTT Client to be used for publishing]
     */
    #clientId = '';
    /**
     * @private {boolean} [Flag to decide whether conenction is reconnection or not.]
     */
    #isReconnecting = false;
    /**
     * @private {boolean} [Flag to decide whether conenction is ending or not.]
     */
    #isEnding = false;
    /**
     * @private {boolean} [Flag to determine whether message need to publish back in case publication failure]
     */
    #publishBackMessage = false;
    /**
     * Initialises the MQTT Client, with respective callbacks, which
     * the communicator will use to publish to a topic.
     * 
     * @param {string} mqttBrokerHost [DNS or IP of the MQTT Broker for making connection.]
     * @param {string} mqttBrokerPort [Port of the MQTT Broker for making connection.]
     * @param {boolean} publishBackMessage [Flag to decide whether to publish back message to Redis.]
     * @param {string} clientIdPrefix [The Prefix string to be used in client Id.]
     * @param {boolean} isSSLEnabled [Flag to decide whether SSL Conenction is enabled or not.]
     * @param {object} sslConfig [Object containing SSL related configurtaion values.]
     */
    constructor(mqttBrokerHost, mqttBrokerPort, publishBackMessage, clientIdPrefix,
        isSSLEnabled, sslConfig) {
        this.#isReconnecting = false;
        // Set the flag to decide whether message needs to be pushed back in Redis or not
        this.#publishBackMessage = publishBackMessage;
        // Set the Parameters for creating MQTT CLient
        var mqttOptions = {
            port: Number(mqttBrokerPort),
            host: mqttBrokerHost,
            keepalive: MQTTConstants.CONNECTION_KEEPALIVE,
            protocolVersion: MQTTConstants.CONNECTION_PROTCOL_VERSION,
            connectTimeout: MQTTConstants.CONNECTION_TIMEOUT,
            reconnectPeriod: MQTTConstants.CONNECTION_RECONNECT_PERIOD,
            reconnectOnConnackError: MQTTConstants.CONNECTION_RECONNECT_ON_CONNACK_ERROR,
        };
        // Set the Client Id if provided
        if (typeof clientIdPrefix != 'undefined' && clientIdPrefix.trim() != '') {
            mqttOptions.clientId = clientIdPrefix;
        }
        // If SSL Connection is enabled then set the required SSL related Configuration
        if (isSSLEnabled) {
            mqttOptions.protocol = MQTTConstants.CONNECTION_SSL_PROTOCOL;
            mqttOptions.ca = fs.readFileSync(`${sslConfig[MQTTConstants.CONNECTION_ROOT_CA_FILE_KEY]}`);
            mqttOptions.cert = fs.readFileSync(`${sslConfig[MQTTConstants.CONNECTION_CLIENT_CERT_FILE_KEY]}`);
            mqttOptions.key = fs.readFileSync(`${sslConfig[MQTTConstants.CONNECTION_CLIENT_KEY_FILE_KEY]}`);
            mqttOptions.rejectUnauthorized = Boolean(Number(sslConfig[MQTTConstants.CONNECTION_CHECK_HOST_NAME_KEY])); // Disable enable Host Name based on Flag
            var clientKeyPassword = sslConfig[MQTTConstants.CONNECTION_CLIENT_CERT_PWD_KEY];
            // Set Client Key Password if provided
            if (typeof clientKeyPassword != 'undefined' && clientKeyPassword.trim() != '') {
                mqttOptions.passphrase = clientKeyPassword.trim();
            }
        } else {
            mqttOptions.protocol = MQTTConstants.CONNECTION_NON_SSL_PROTOCOL;
        }
        // Create the MQTT Client
        this.#client = connect(mqttOptions);

        // Listener for successful MQTT Client connection.
        this.#client.on('connect', (connack) => {
            Logger.log(Logger.DEBUG, `Connection Ack Object received for Connection : ${JSON.stringify(connack)}`);
            if (this.#isReconnecting) {
                Logger.log(Logger.INFO, `Client reconnected to MQTT broker.`);
                this.#isReconnecting = false;
                CloudConnect.notifyCloudClientReconnection(this.#clientId);
            } else {
                Logger.log(Logger.INFO, `MQTT Connection successful for Client.`);
            }
        });

        // Listener for errors in the MQTT Client.
        this.#client.on('error', function (error) {
            Logger.log(Logger.ERROR, `MQTT Client Error : ${error}-${error.stack}`);
        });
        // Listener for MQTT Client Closure.
        this.#client.on('close', () => {
            if (this.#isEnding) {
                Logger.log(Logger.INFO, 'MQTT Connection closed by client end()');
                this.#isEnding = false; // Reset the flag
            } else {
                Logger.log(Logger.INFO, `MQTT Connection closed unexpectedly.`);
                CloudConnect.notifyCloudClientDisconnection(this.#clientId);
            }
        });
        // Listener for MQTT Client disconnection when Broker diconnects the connection.
        this.#client.on('disconnect', (packet) => {
            Logger.log(Logger.INFO, `MQTT Broker diconnected connection with parameter ${packet.cmd}:${packet.reasonCode}`);
            CloudConnect.notifyCloudClientDisconnection(this.#clientId);
        });
        // Listener for MQTT Client when it reconnects with the Broker.
        this.#client.on('reconnect', () => {
            Logger.log(Logger.INFO, 'Reconnecting to MQTT broker.');
            this.#isReconnecting = true;
        });
        // Listener for MQTT Client when its end function is invoked to close the connection.
        this.#client.on('end', () => {
            Logger.log(Logger.INFO, 'Connection to MQTT broker ended.');
        });
    }

    /**
     * Getter method for created MQTT Client.
     * 
     * @returns {Object} [The created MQTT Client.]
     */
    getClient() {
        return this.#client;
    }

    /**
     * Setter method for MQTT Client's Client Id.
     * 
     * @param {string} clientId [The id with which client is identified on MQTT Broker.]
     */
    setClientId(clientId) {
        this.#clientId = clientId;
    }

    /**
     * The method to be used for closing the MQTT Publisher connection with the Broker
     * along with removing the require mapping with its subscriber in the invocation class.
     */
    shutdown() {
        // Remove the mapping of cloud client from Main Invocation Class
        CloudConnect.removeCloudClientSubcriberMapping(this.#clientId);
        this.#isEnding = true;
        this.#client.end();
    }

    /**
     * The method which will be invoked to publish message to the topic.
     * The method will be invoked by cloud communicator subscriber.
     * 
     * @param {string} sourceTopic [The Topic name originally on which the message was received.]
     * @param {string} topicToPublish [Exact Name of the topic to publish the message]
     * @param {string} msgToPublish [Message to publish over topic]
     */
    publish(sourceTopic, topicToPublish, msgToPublish) {
        this.#client.publish(topicToPublish, msgToPublish, { qos: 1, retain: false }, (error) => {
            if (error) {
                Logger.log(Logger.ERROR, `Error while publishing message to topic ${topicToPublish} : ${error}-${error.stack}`);
                // If enabled push back the message back to Redis on the topic it was received
                if (this.#publishBackMessage) {
                    CloudConnect.publishMessageBackToRedis(sourceTopic, msgToPublish);
                }
            } else {
                Logger.log(Logger.DEBUG, `Successfully published message to topic ${topicToPublish}`);
            }
        });
    }
}
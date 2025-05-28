/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- aedesBroker.js
 * Description :- The mock file for mocking MQTT Broker via Aedes.
 */
'use strict';

import Aedes from 'aedes';
import tls from 'tls';
import net from 'net';
import fs from 'fs';

/**
 * Class representing Aedes based MQTT Broker
 */
export class AedesBroker {
  /**
   * @private {Object} [The instance of Aedes to be used for creating MQTT Broker.]
   */
  #aedes = undefined;
  /**
   * @private {Object} [The instance of created MQTT Broker.]
   */
  #broker = undefined;
  /**
   * @private {number} [The port on which MQTT Broker needs to be started.]
   */
  #port = 0;
  /**
   * @private {Object} [TLS options for the server.]
   */
  #tlsOptions = undefined;
  /**
    * Initialises the Aedes based MQTT Broker instance
    * 
    * @param {number} port [Port of thee MQTT Broker to be used for making connection.]
    * @param {boolean} enableSSL [Flag to decide whether SSL needs to be enabled.]
    * @param {string} caCertPath [Path to the CA certificate.]
    * @param {string} serverCertPath [Path to the server certificate.]
    * @param {string} serverKeyPath [Path to the server key.]
    */
  constructor(port, enableSSL, caCertPath, serverCertPath, serverKeyPath) {
    this.#aedes = Aedes();
    this.#port = port;

    if (enableSSL) {
      // Load TLS certificates
      this.#tlsOptions = {
        ca: fs.readFileSync(caCertPath, 'utf-8'),
        cert: fs.readFileSync(serverCertPath, 'utf-8'),
        key: fs.readFileSync(serverKeyPath, 'utf-8'),
        requestCert: true,
        rejectUnauthorized: true
      };
      // Create instance of MQTT Broker
      this.#broker = tls.createServer(this.#tlsOptions, this.#aedes.handle);
    } else {
      // Create instance of MQTT Broker
      this.#broker = net.createServer(this.#aedes.handle);
    }

    this.#aedes.on('client', (client) => {
      console.log(`[AEDES Broker] : Client connected: ${client.id}`);
    });

    this.#aedes.on('clientDisconnect', (client) => {
      console.log(`[AEDES Broker] : Client disconnected: ${client.id}`);
    });
  }

  /**
   * Function to start the MQTT Broker Instance
   */
  start() {
    this.#broker.listen(this.#port, () => {
      console.log(`[AEDES Broker] : MQTT Broker listening on port ${this.#port}`);
    });
  }

  /**
   * Function to stop the MQTT Broker Instance
   */
  async stop() {
    // Close the MQTT Broker Server
    this.#broker.close();
    // Close the Aedes Instance Handle
    await this.#aedes.close();
    console.log('Closed AEDES Instance.');
  }

  /**
   * Function to list all the connected client
   */
  listClients() {
    console.log('Listing all connected clients:');
    for (const [clientId, client] of Object.entries(this.#aedes.clients)) {
      console.log(`Client ID: ${clientId}, Client:`, client.id);
    }
  }

  /**
   * Function to close the connection of the provided client id's client.
   * 
   * @param {string} clientId [Id of the client whose connection needs to be disconnected.]
   */
  closeClientConnection(clientId) {
    this.listClients();
    console.log(`The client Id received for connection closure is ${clientId}.`);
    const client = this.#aedes.clients[clientId];
    if (client) {
      client.close();
      console.log(`Client ${clientId} connection closed.`);
    } else {
      console.log(`Client ${clientId} not found`);
    }
  }
}

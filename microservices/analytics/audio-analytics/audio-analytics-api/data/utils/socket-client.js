/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const net = require('net');
const fs = require('fs');
const path = require('path');
const { promisify } = require('util');
const { randomUUID } = require('crypto');

class SocketClient {
    constructor(socketPath) {
        this.socketPath = socketPath;
        this.client = null;
        this.connected = false;
        this.channelHandlers = new Map(); // Channel -> Set of handlers
        this.reconnectInterval = 1000; // 1 second
        this.maxReconnectAttempts = 10;
        this.reconnectAttempts = 0;
        // Guard against scheduling more than one reconnect timer at a time.
        // Both the 'error' and 'close' events fire on the same failed socket,
        // so without this flag each failure would schedule two timers, produce
        // two connect() calls, and leave the server with two live clients —
        // causing every broadcast message to be delivered twice.
        this.reconnectTimer = null;
    }

    async connect() {
        // Tear down any existing socket before opening a new one so we never
        // end up with two simultaneous connections to the server.
        if (this.client) {
            this.client.removeAllListeners();
            this.client.destroy();
            this.client = null;
        }

        return new Promise((resolve, reject) => {
            // Ensure socket directory exists
            const socketDir = path.dirname(this.socketPath);
            if (!fs.existsSync(socketDir)) {
                try {
                    fs.mkdirSync(socketDir, { recursive: true });
                } catch (err) {
                    console.error(`Error creating socket directory: ${err.message}`);
                }
            }

            this.client = net.createConnection({ path: this.socketPath }, () => {
                console.log(`Connected to socket: ${this.socketPath}`);
                this.connected = true;
                this.reconnectAttempts = 0;
                resolve();
            });

            this.client.on('data', (data) => {
                try {
                    const messages = data.toString().split('\n').filter(msg => msg.trim());
                    
                    for (const message of messages) {
                        const response = JSON.parse(message);
                        const channel = response.channel;
                        const msgData = response.message;
                        
                        // Call all registered handlers for this channel
                        if (channel && this.channelHandlers.has(channel)) {
                            const handlers = this.channelHandlers.get(channel);
                            handlers.forEach(handler => {
                                try {
                                    handler(false, null, JSON.stringify(msgData));
                                } catch (err) {
                                    console.error(`Error in handler for channel ${channel}:`, err.message);
                                }
                            });
                        }
                    }
                } catch (err) {
                    console.error(`Error processing socket data: ${err.message}`);
                }
            });

            this.client.on('error', (err) => {
                console.error(`Socket error: ${err.message}`);
                this.connected = false;
                if (this.reconnectAttempts === 0) {
                    reject(err);
                }
                this.reconnect();
            });

            this.client.on('close', () => {
                console.log('Socket connection closed');
                this.connected = false;
                this.reconnect();
            });
        });
    }

    reconnect() {
        // Only schedule one reconnect timer at a time.  Both 'error' and
        // 'close' fire on the same failed socket; without this guard each
        // failure would enqueue two timers → two connect() calls → two live
        // server connections → every broadcast message delivered twice.
        if (this.reconnectTimer) return;

        if (this.reconnectAttempts >= this.maxReconnectAttempts) {
            console.error(`Failed to reconnect after ${this.maxReconnectAttempts} attempts`);
            return;
        }

        this.reconnectAttempts++;
        console.log(`Attempting to reconnect (${this.reconnectAttempts}/${this.maxReconnectAttempts})...`);

        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.connect().catch(err => {
                console.error(`Reconnect attempt failed: ${err.message}`);
            });
        }, this.reconnectInterval);
    }

    async publish(channel, message) {
        if (!this.connected) {
            try {
                await this.connect();
            } catch (err) {
                throw new Error(`Failed to connect to socket: ${err.message}`);
            }
        }

        const request = {
            channel,
            message
        };

        try {
            this.client.write(JSON.stringify(request) + '\n');
        } catch (err) {
            throw new Error(`Failed to publish message: ${err.message}`);
        }
    }

    subscribe(channel, handler) {
        console.log(`Subscribing to socket channel: ${channel}`);
        if (!this.channelHandlers.has(channel)) {
            this.channelHandlers.set(channel, new Set());
        }
        this.channelHandlers.get(channel).add(handler);
    }

    unsubscribe(channel, handler) {
        console.log(`Unsubscribing from channel: ${channel}`);
        if (handler) {
            // Remove specific handler
            if (this.channelHandlers.has(channel)) {
                this.channelHandlers.get(channel).delete(handler);
                // Clean up empty sets
                if (this.channelHandlers.get(channel).size === 0) {
                    this.channelHandlers.delete(channel);
                }
            }
        } else {
            // Remove all handlers for this channel
            this.channelHandlers.delete(channel);
        }
    }

    async close() {
        if (this.client) {
            this.client.end();
            this.client = null;
            this.connected = false;
        }
    }
}

module.exports = SocketClient;

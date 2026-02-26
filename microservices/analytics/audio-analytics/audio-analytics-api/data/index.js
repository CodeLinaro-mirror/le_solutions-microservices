/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

var path = require('path');
var http = require('http');
var cors = require('cors');
const WebSocket = require('ws');
const messages = require('./utils/messages');
const config = require('./config/config');

var oas3Tools = require('oas3-tools');
var serverPort = config.apiPort;

// Timeout duration in milliseconds
let inactivityTimer = null;
const INACTIVITY_TIMER = config.inactivityTimer; // 30 seconds

// swaggerRouter configuration
var options = {
    routing: {
        controllers: path.join(__dirname, './controllers')
    },
};

var expressAppConfig = oas3Tools.expressAppConfig(path.join(__dirname, 'api/openapi.yaml'), options);
var app = expressAppConfig.getApp();

// Mount the API at the correct base path
var express = require('express');
var mainApp = express();

console.log('🚀 STARTING API SERVER');

// Log every single request that hits the server
mainApp.use((req, res, next) => {
    console.log(`🌐 INCOMING REQUEST: ${req.method} ${req.path} from ${req.ip}`);
    next();
});

// Handle OPTIONS requests FIRST, before anything else
mainApp.options('*', (req, res) => {
    //console.log('🔥 OPTIONS request intercepted for:', req.path, 'from IP:', req.ip);
    res.header('Access-Control-Allow-Origin', '*');
    res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
    res.header('Access-Control-Allow-Headers', 'Origin, X-Requested-With, Content-Type, Accept, Authorization');
    res.header('Access-Control-Max-Age', '3600');
    //console.log('🔥 Sending OPTIONS response with CORS headers');
    return res.sendStatus(200);
});

// Enhanced CORS configuration to handle preflight OPTIONS requests
const corsOptions = {
    origin: true, // Allow all origins for development
    methods: ['GET', 'POST', 'PUT', 'DELETE', 'OPTIONS'],
    allowedHeaders: ['Origin', 'X-Requested-With', 'Content-Type', 'Accept', 'Authorization'],
    credentials: false,
    optionsSuccessStatus: 200,
    preflightContinue: false
};

mainApp.use(cors(corsOptions));

mainApp.use('/audio-analytics/v1/api', app);

// Also serve at root for backward compatibility
mainApp.use('/', app);

// Add a simple health check route that bypasses oas3-tools
app.get('/health-simple', (req, res) => {
    res.status(200).json({ status: 'ok', message: 'Simple health check working' });
});

// Add error handling middleware
app.use((err, req, res, next) => {
    console.error('Express error handler caught:', err.message);
    console.error('Stack:', err.stack);
    res.status(500).json({
        error: {
            message: err.message,
            type: 'server_error'
        }
    });
});

// Initialize the Swagger middleware
let theApp = http.createServer(mainApp).listen(serverPort, function () {
    console.log('Your server is listening on port %d (http://localhost:%d)', serverPort, serverPort);
    console.log('Swagger-ui is available on http://localhost:%d/docs', serverPort);
});

theApp.timeout = INACTIVITY_TIMER;

const ws = new WebSocket.Server({server: theApp});
const socketClient = null;

async function gracefulshutdown() {
    console.log("SIGTERM caught, Shutting Down");
    
    // Close WebSocket server first
    if (ws) {
        console.log("Closing WebSocket server...");
        ws.close(() => {
            console.log("WebSocket server closed.");
        });
    }
    
    // Close HTTP server
    theApp.close(() => {
        console.log("HTTP server closed.");
        process.exit(0);
    });
    
    // Force exit after 5 seconds if graceful shutdown fails
    setTimeout(() => {
        console.log("Force exiting after timeout");
        process.exit(1);
    }, 5000);
}

async function startWebsocket() {

    ws.on('connection', wsc => {
        console.log('WebSocket client connected');

        try {
            // Send a proper JSON welcome message instead of plain text
            wsc.send(JSON.stringify({
                message_type: 'connection_established',
                message: 'Welcome to the Audio Analytics API!',
                timestamp: new Date().toISOString()
            }));
            resetInactivityTimer(wsc); // Start timer when connection opens

        } catch (err) {
            console.error('Error sending welcome message:', err);
        }

        // Send queued messages if first client
        if (ws.clients.size == 1) {
            resetInactivityTimer(wsc);
            messages.sendQueue(wsc);
        }

        // Process Incoming Audio
        wsc.on('message', (msg) => messageSwitch(msg, wsc));
        
        // Handle client disconnect
        wsc.on('close', (code, reason) => {
            console.log(`WebSocket client disconnected: code=${code}, reason=${reason}`);
            clearTimeout(inactivityTimer); // Stop timer when closed
        });
        
        // Handle client errors
        wsc.on('error', (err) => {
            console.error('WebSocket client error:', err);
        });
    });
    
    // Handle WebSocket server errors
    ws.on('error', (err) => {
        console.error('WebSocket server error:', err);
    });
}

// Function to reset inactivity timer
function resetInactivityTimer(wsc) {
    if (inactivityTimer) {
        console.info("Activity Detected, resetting WebSocket client timer");
        clearTimeout(inactivityTimer);
    } else {
        console.info("Client connected. Starting WebSocket client timer");
    }
    inactivityTimer = setTimeout(() => {
        console.warn(`No messages received for ${INACTIVITY_TIMER} seconds. Closing WebSocket...`);
        if (wsc.readyState === WebSocket.OPEN) {
            wsc.send(JSON.stringify({
                message_type: 'connection_close',
                message: `No Messages have been sent for ${INACTIVITY_TIMER} milliseconds. Connection Closing...`,
                timestamp: new Date().toISOString()
            }));
            wsc.close(); // Custom close code & reason
        }
    }, INACTIVITY_TIMER);
}

async function messageSwitch(msg, wsc) {
    try {
        console.log('WebSocket received message, type:', typeof msg, 'isBuffer:', Buffer.isBuffer(msg));
        resetInactivityTimer(wsc); // Reset timer on every message

        if (typeof msg === 'string') {
            console.log('Message is string, length:', msg.length, 'preview:', msg.substring(0, 100));
        } else if (Buffer.isBuffer(msg)) {
            console.log('Message is Buffer, length:', msg.length);
        }
        
        // Check if the message is JSON or binary data
        if (typeof msg === 'string') {
            try {
                let body = JSON.parse(msg);
                // Handle JSON messages
                if (body.message_type === 'transcriptions_session_audio') {
                    // Handle audio data in JSON format
                    console.log(`Received audio data for session: ${body.session_id}, data length: ${body.data ? body.data.length : 0}`);
                    messages.publish(config.asrTranscriptionIn, body, (err, data) => {
                        if (err) {console.error(data);}
                    });
                } else {
                    console.log('Received unknown JSON message type:', body.message_type || 'undefined');
                    // Try to handle it anyway
                    if (!body.message_type && body.session_id) {
                        body.message_type = 'transcriptions_session_audio';
                        messages.publish(config.asrTranscriptionIn, body, (err, data) => {
                            if (err) {console.error(data);}
                        });
                    } else {
                        messages.publish(config.asrTranscriptionIn, body, (err, data) => {
                            if (err) {console.error(data);}
                        });
                    }
                }
            } catch (e) {
                // Not valid JSON, treat as binary data
                console.log('Received non-JSON message, treating as binary data');
                // Forward binary data directly to the server
                messages.publish(config.asrTranscriptionIn, msg, (err, data) => {
                    if (err) {console.error(data);}
                });
            }
        } else {
            // Binary data (Buffer or ArrayBuffer) - but might be JSON string as Buffer
            console.log('Received binary data');
            
            // Try to convert Buffer to string and parse as JSON
            if (Buffer.isBuffer(msg)) {
                try {
                    const msgString = msg.toString('utf8');
                    console.log('Converted Buffer to string, length:', msgString.length, 'preview:', msgString.substring(0, 100));
                    const body = JSON.parse(msgString);
                    
                    // Handle as JSON message
                    if (body.message_type === 'transcriptions_session_audio') {
                        console.log(`Received audio data (from Buffer) for session: ${body.session_id}, data length: ${body.data ? body.data.length : 0}`);
                        messages.publish(config.asrTranscriptionIn, body, (err, data) => {
                            if (err) {console.error(data);}
                        });
                    } else {
                        console.log('Received unknown JSON message type (from Buffer):', body.message_type || 'undefined');
                        messages.publish(config.asrTranscriptionIn, body, (err, data) => {
                            if (err) {console.error(data);}
                        });
                    }
                    return;
                } catch (e) {
                    console.log('Buffer is not valid JSON, treating as raw binary data');
                }
            }
            
            // Forward raw binary data directly to the server (fallback)
            messages.publish(config.asrTranscriptionIn, msg, (err, data) => {
                if (err) {console.error(data);}
            });
        }
    } catch (e) {
        console.error('Error processing WebSocket message:', e);
    }
}

module.exports.getWebSocketServer = function() {
    return ws;
};

module.exports.getSocketClient = function() {
    return socketClient;
};

module.exports.resetInactivityTimer = resetInactivityTimer;

// Initialize
startWebsocket();
messages.initializeCommunication(socketClient).then(() => {
    console.log('✅ Audio Analytics API initialized successfully');
}).catch(err => {
    console.error('❌ Failed to initialize API:', err);
});
// Handle multiple shutdown signals
process.on('SIGTERM', gracefulshutdown);
process.on('SIGINT', gracefulshutdown);
process.on('SIGUSR2', gracefulshutdown); // For nodemon

// Handle uncaught exceptions
process.on('uncaughtException', (err) => {
    console.error('Uncaught Exception:', err);
    gracefulshutdown();
});

process.on('unhandledRejection', (reason, promise) => {
    console.error('Unhandled Rejection at:', promise, 'reason:', reason);
    gracefulshutdown();
});

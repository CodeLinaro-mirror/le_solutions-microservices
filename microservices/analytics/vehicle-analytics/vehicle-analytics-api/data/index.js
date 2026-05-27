/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

var path = require('path');
var http = require('http');
const config = require('./config/config');
const utils = require('./utils/utils');

var oas3Tools = require('oas3-tools');
var serverPort = 8084;

// swaggerRouter configuration
var options = {
    routing: {
        controllers: path.join(__dirname, './controllers')
    },
};

(async () => {
    try {
        await utils.initializeDB();
        await utils.populateRedis();
    } catch (e) {
        console.error('Error populating redis');
        gracefulshutdown();
    }
})();

// Listen for Camera Updates
utils.cameraUpdates();

var expressAppConfig = oas3Tools.expressAppConfig(path.join(__dirname, 'api/openapi.yaml'), options);
var app = expressAppConfig.getApp();

// Initialize the Swagger middleware
let theApp = http.createServer(app).listen(serverPort, function () {
    console.log('Your server is listening on port %d (http://localhost:%d)', serverPort, serverPort);
    console.log('Swagger-ui is available on http://localhost:%d/docs', serverPort);
});

async function gracefulshutdown() {
    console.log("SIGTERM caught, Shutting Down");
    theApp.close(() => {
        console.log("HTTP server closed.");

        // When server has stopped accepting connections
        // exit the process with exit status 0
        process.exit(0);
    });
}

process.on('SIGTERM', gracefulshutdown);

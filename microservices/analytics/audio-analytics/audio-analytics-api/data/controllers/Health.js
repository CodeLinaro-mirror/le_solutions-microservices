/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');

module.exports.healthCheck = async function healthCheck (req, res, next, body) {
    console.log('=== Health check function called ===');
    console.log('Arguments:', { req: typeof req, res: typeof res, next: typeof next, body: body });
    
    try {
        console.log('Health check: Starting...');

        // Get WebSocket connection count
        let active_sessions = 0;
        try {
            console.log('Health check: Getting WebSocket server...');
            const indexModule = require('../index');
            const ws = indexModule.getWebSocketServer();
            if (ws && ws.clients) {
                active_sessions = ws.clients.size;
                console.log('Health check: Active sessions:', active_sessions);
            } else {
                console.log('Health check: WebSocket server not ready');
            }
        } catch (e) {
            // WebSocket server might not be initialized yet
            console.log('Health check: WebSocket error (non-fatal):', e.message);
        }

        console.log('Health check: Building response...');
        const healthData = {
            status: "ok",
            uptime: Math.floor(process.uptime()),
            active_sessions: active_sessions,
            version: "1.0.0",
            mode: config.blackboxContainer ? "blackbox" : "standard",
            timestamp: new Date().toISOString()
        };

        console.log('Health check: Sending response:', JSON.stringify(healthData));
        res.status(200).json(healthData);
        console.log('Health check: Response sent successfully');
    } catch (e) {
        console.error('Health check: FATAL ERROR:', e.message);
        console.error('Health check: Stack trace:', e.stack);
        try {
            res.status(503).json({
                error: {
                    message: e.message || "Server is down or overloaded.",
                    type: "service_unavailable",
                    param: null,
                    code: null
                }
            });
        } catch (resError) {
            console.error('Health check: Failed to send error response:', resError.message);
        }
    }
};


/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');
const isDebug = config.logLevel <= 10;

module.exports.healthCheck = async function healthCheck (req, res, next, body) {
    if (isDebug) console.log('=== Health check function called ===');
    if (isDebug) console.log('Arguments:', { req: typeof req, res: typeof res, next: typeof next, body: body });
    
    try {
        if (isDebug) console.log('Health check: Starting...');

        // Get WebSocket connection count
        let active_sessions = 0;
        try {
            if (isDebug) console.log('Health check: Getting WebSocket server...');
            const indexModule = require('../index');
            const ws = indexModule.getWebSocketServer();
            if (ws && ws.clients) {
                active_sessions = ws.clients.size;
                if (isDebug) console.log('Health check: Active sessions:', active_sessions);
            } else {
                if (isDebug) console.log('Health check: WebSocket server not ready');
            }
        } catch (e) {
            // WebSocket server might not be initialized yet
            console.log('Health check: WebSocket error (non-fatal):', e.message);
        }

        if (isDebug) console.log('Health check: Building response...');
        const healthData = {
            status: "ok",
            uptime: Math.floor(process.uptime()),
            active_sessions: active_sessions,
            version: "1.0.0",
            mode: config.blackboxContainer ? "blackbox" : "standard",
            timestamp: new Date().toISOString()
        };

        if (isDebug) console.log('Health check: Sending response:', JSON.stringify(healthData));
        res.status(200).json(healthData);
        if (isDebug) console.log('Health check: Response sent successfully');
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

module.exports.getKPIBenchmarks = async function getKPIBenchmarks (req, res, next, body) {
    // Override default timeout for this request.
    req.setTimeout(300*1000);
    if (isDebug) console.log('=== Get KPIs called  ===');
    
    try {
        if (isDebug) console.log('KPIs: Starting...');

        const requestBody = {
            "message_type": 'get_kpi_benchmark'
        };

        // handle message
        messages.publishAndListenOnce(config.audioKPI, config.audioKPI, requestBody, (err, data) => {
            if (err) {
                res.status(400).json({
                    error: {
                        message: data.message,
                        type: "server_error",
                        param: null,
                        code: null
                    }
                });
            } else {
                console.log('Received kpis from server');
                // Respond with the models (data.result contains the array)
                res.status(200).json(data.result || data);
            }
        });
    } catch (e) {
        console.error('KPI error:', e);
        res.status(500).json({
            error: {
                message: e.message,
                type: "server_error",
                param: null,
                code: null
            }
        });
    }
};

module.exports.getKPILast = async function getKPILast (req, res, next, body) {
    // Override default timeout for this request.
    req.setTimeout(300*1000);
    if (isDebug) console.log('=== Get Last KPIs called  ===');

    try {
        if (isDebug) console.log('KPIs: Starting...');

        const requestBody = {
            "message_type": 'get_kpi_last'
        };

        // handle message
        messages.publishAndListenOnce(config.audioKPI, config.audioKPI, requestBody, (err, data) => {
            if (err) {
                res.status(400).json({
                    error: {
                        message: data.message,
                        type: "server_error",
                        param: null,
                        code: null
                    }
                });
            } else {
                console.log('Received kpis from server');
                // Respond with the models (data.result contains the array)
                res.status(200).json(data.result || data);
            }
        });
    } catch (e) {
        console.error('KPI error:', e);
        res.status(500).json({
            error: {
                message: e.message,
                type: "server_error",
                param: null,
                code: null
            }
        });
    }
};
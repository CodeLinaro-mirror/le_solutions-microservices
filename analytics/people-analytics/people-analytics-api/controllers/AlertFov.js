/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');

module.exports.getAllAlerts = async function getAllAlerts (req, res, next, body) {
    // Upload new job to the dataStore and get back unique ID
    try {
        let alertList = await db.getAlerts(req.query);
        let retList = [];
        for await (let e of alertList) {
            // Rebuild to match expected format schema
            retList.push({
                "source_trigger": {
                    "monitor_id": e.monitor_id,
                    "trigger_id": e.trigger_id,
                    "trigger_name": e.trigger_name,
                    "trigger_condition": e.trigger_condition,
                    "params": JSON.parse(e.params)
                },
                "time": e.time,
                "causes": JSON.parse(e.causes)
            });
        };
        res.status(200).json(retList);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

PAAlertsRedisHook();
function PAAlertsRedisHook() {
    redis.listenToChannel(config.redisPAAlertsChannel, async function(err, data) {
        try {
            if (err) {
                console.error(err);
            } else {
                await db.insertPAAlert(data);
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

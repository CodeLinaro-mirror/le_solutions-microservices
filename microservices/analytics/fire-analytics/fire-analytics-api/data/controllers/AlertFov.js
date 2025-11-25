/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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
                    "params": JSON.parse(e.params)
                },
                "time": parseFloat(e.time).toFixed(3),
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

FAAlertsRedisHook();
function FAAlertsRedisHook() {
    redis.listenToChannel(config.redisFAAlertsChannel, async function(err, data) {
        try {
            if (err) {
                console.error(err);
            } else {
                let jsonData = JSON.parse(data);

                let trigger = await db.getFATrigger(jsonData.source_trigger.trigger_id);
                if (trigger.length == 0 && jsonData.source_trigger.trigger_id == '0xDEADBEEF') {
                    // No Trigger Sample Alert.
                    if (process.env.LOG_LEVEL >= 2) {console.debug(`Fake Trigger from FAS has been detected. Ignore`);}
                } else {
                    if (process.env.LOG_LEVEL >= 2) {console.debug(`inserting Alert into Database ${data}`);}
                    await db.insertFAAlert(data);
                }
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

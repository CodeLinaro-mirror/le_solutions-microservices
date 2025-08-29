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
            let a = {
                "source_trigger": {
                    "monitor_id": e.monitor_id,
                    "trigger_id": e.trigger_id,
                    "trigger_name": e.trigger_name,
                    "trigger_condition": e.trigger_condition,
                    "params": JSON.parse(e.params)
                },
                "alert_id": e.alert_id,
                "time": parseFloat(e.time).toFixed(3),
                "end_time": e.time ? parseFloat(e.time).toFixed(3) : 0,
                "type": e.type
            };
            if (e.type == 'accessories') {
                a.causes = e.causes ? JSON.parse(e.causes) : JSON.parse(e.occupants);
            } else if (e.type == 'occupancy') {
                a.occupants = e.occupants ? JSON.parse(e.occupants) : JSON.parse(e.causes);
            }
            retList.push(a);
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
                let jsonData = JSON.parse(data);

                // Get source trigger for the incoming Alert
                let trigger = await db.getPATrigger(jsonData.source_trigger.trigger_id);

                // Check to see if Trigger is the fake one
                if (trigger.length == 0 && jsonData.source_trigger.trigger_id == '0xDEADBEEF') {
                    // No Trigger Sample Alert.
                    if (process.env.LOG_LEVEL >= 2) {console.debug(`Fake Trigger from PAS has been detected. Ignore`);}
                }
                else {
                    // Normal Alert with a real Trigger
                    if (process.env.LOG_LEVEL >= 2) {console.debug(`inserting Alert into Database ${data}`);}

                    // Check to see if Occupants or Causes exist
                    // Causes = Alert. Occupants = AlertOccupancy
                    if (jsonData.causes) {
                        jsonData.type = 'accessories';
                        await db.insertPAAlert(jsonData);
                    } else if (jsonData.occupants) {
                        jsonData.type = 'occupancy';
                        await db.insertPAAlertOccupancy(jsonData);
                    }
                }
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

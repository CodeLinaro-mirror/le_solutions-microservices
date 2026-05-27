/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');

module.exports.getAllVehicleAlerts = async function getAllVehicleAlerts (req, res, next, body) {
    // Upload new job to the dataStore and get back unique ID
    try {
        let alertList = await db.getAlerts(req.query);
        let retList = [];
        for await (let e of alertList) {
            // Rebuild to match expected format schema
            let Obj = {
                "source_trigger": {
                    "monitor_id": e.monitor_id,
                    "trigger_id": e.trigger_id,
                    "trigger_name": e.trigger_name,
                    "trigger_condition": e.trigger_condition,
                },
                "alert_id": e.alert_id,
                "time":  parseFloat(e.time).toFixed(3),
                "end_time": e.time ? parseFloat(e.time).toFixed(3) : 0,
                "type": e.type
            };

            // Check for existent of sub objects as each Alert type can be different based on schema
            if (e.params && typeof(e.params) != undefined && e.params != "undefined")
                Obj["source_trigger"]["params"] = JSON.parse(e.params);
            if (e.occupants && typeof(e.vehicles) != undefined && e.vehicles != "undefined")
                Obj["vehicles"] = JSON.parse(e.vehicles);
            if (e.region_id && typeof(e.region_id) != undefined && e.region_id != "undefined") 
                Obj["source_trigger"]["region_id"] = e.region_id;
            retList.push(Obj);
        };
        res.status(200).json(retList);
    } catch (e) {
        console.error(e);
        res.status(400).json({
            reason: e.message
        });
    }
};

VAAlertsRedisHook();
function VAAlertsRedisHook() {
    redis.listenToChannel(config.redisVAAlertsChannel, async function(err, jsonData) {
        try {
            if (err) {
                console.error(err);
            } else {
                // Check to see if we need to update an Alert vs a new one
                let data = JSON.parse(jsonData);
                await db.insertVAAlert(data);
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');

module.exports.getAllVehicleAlertsRegion = async function getAllVehicleAlertsRegion (req, res, next, body) {
    // Upload new job to the dataStore and get back unique ID
    try {
        let alertList = await db.getAlertsByRegion(req.query);
        let retList = [];
        for await (let e of alertList) {
            // Rebuild to match expected format schema
            let Obj = {
                "source_trigger": {
                    "monitor_id": e.monitor_id,
                    "region_id": e.region_id,
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
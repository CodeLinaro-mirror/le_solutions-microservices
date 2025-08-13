/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');

module.exports.getAllTripwireAlerts = async function getAllTripwireAlerts (req, res, next, body) {
    // Upload new job to the dataStore and get back unique ID
    try {
        let alertList = await db.getAlerts(req.query);
        let retList = [];
        for await (let e of alertList) {
            // Rebuild to match expected format schema
            let Obj = {
                "monitor_id": e.monitor_id,
                "source_trigger": {
                    "tripwire_id": e.tripwire_id,
                    "trigger_id": e.trigger_id,
                    "trigger_name": e.trigger_name,
                    "trigger_direction": e.trigger_direction,
                    "trigger_condition": e.trigger_condition,
                },
                "time":  parseFloat(e.time)
            };

            // Check for existent of sub objects as each Alert type can be different based on schema
            if (e.crossings && typeof(e.crossings) != undefined && e.crossings != "undefined")
                Obj["crossings"] = JSON.parse(e.crossings);
            if (e.params && typeof(e.params) != undefined && e.params != "undefined")
                Obj["source_trigger"]["params"] = JSON.parse(e.params);
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

TAAlertsRedisHook();
function TAAlertsRedisHook() {
    redis.listenToChannel(config.redisTAAlertsChannel, async function(err, data) {
        try {
            if (err) {
                console.error(err);
            } else {
                await db.insertTAAlert(data);
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');

module.exports.getAllRegionAlerts = async function getAllRegionAlerts (req, res, next, body) {
    // Upload new job to the dataStore and get back unique ID
    try {
        let alertList = await db.getAlerts(req.query);
        let retList = [];
        for await (let e of alertList) {
            // Rebuild to match expected format schema
            let Obj = {
                "monitor_id": e.monitor_id,
                "source_trigger": {
                    "region_id": e.region_id,
                    "trigger_id": e.trigger_id,
                    "trigger_name": e.trigger_name,
                    "trigger_condition": e.trigger_condition,
                },
                "time": e.time,
            };

            // Check for existent of sub objects as each Alert type can be different based on schema
            if (e.params && typeof(e.params) != undefined && e.params != "undefined")
                Obj["source_trigger"]["params"] = JSON.parse(e.params);
            if (e.occupants && typeof(e.occupants) != undefined && e.occupants != "undefined")
                Obj["occupants"] = JSON.parse(e.occupants);
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

RAAlertsRedisHook();
function RAAlertsRedisHook() {
    redis.listenToChannel(config.redisRAAlertsChannel, async function(err, data) {
        try {
            if (err) {
                console.error(err);
            } else {
                await db.insertRAAlert(data);
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

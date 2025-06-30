/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configTriggerRegionCreate = async function configTriggerRegionCreate (req, res, next, body) {
    // Creates new trigger attached to specified region. 

    try {
        await db.insertRATrigger(body);
        await redis.insertRATrigger(body);
        res.status(200).send("New trigger was successfully configured");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

module.exports.configTriggerRegionDelete = async function configTriggerRegionDelete (req, res, next, body) {
    // Deletes an existing trigger by Trigger ID
    try {
        // Get trigger with the matching triggerId in Database
        let trigger = await db.getRATrigger(body);

        // Check to see if trigger provided exists
        if (trigger.length !== 0) {
            // Remove trigger from Database
            await db.removeTrigger(body);
            // Remove trigger from Redis and potentially update Redis Channel
            await redis.removeTrigger(body);
            res.status(200).send('Trigger was successfully deleted');

        } else {
            res.status(400).json({
                reason: 'triggerId provided does not currently match any trigger configuration in the system.'
            });
        }
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

module.exports.configTriggerRegionGet = async function configTriggerRegionGet (req, res, next, body) {
    // Return list of triggers on a given monitorId

    try {
        let triggerList = await db.getRATriggersByMonitor(body);
        res.status(200).json(triggerList);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

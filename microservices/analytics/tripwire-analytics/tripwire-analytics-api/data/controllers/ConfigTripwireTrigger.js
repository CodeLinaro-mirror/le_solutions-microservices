/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configTriggerTripwireCreate = async function configTriggerTripwireCreate (req, res, next, body) {
    // Creates new trigger attached to specified tripwire. 

    try {
        await db.insertTATrigger(body);
        await redis.insertTATrigger(body);
        res.status(200).send("New trigger was successfully configured");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

module.exports.configTriggerTripwireDelete = async function configTriggerTripwireDelete (req, res, next, body) {
    // Deletes an existing trigger by Trigger ID
    try {
        // Get trigger with the matching triggerId in Database
        let trigger = await db.getTATrigger(body);

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

module.exports.configTriggerTripwireGet = async function configTriggerTripwireGet (req, res, next, body) {
    // Return list of triggers on a given monitorId

    try {
        let triggerList = await db.getTATriggersByMonitor(body);
        res.status(200).json(triggerList);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

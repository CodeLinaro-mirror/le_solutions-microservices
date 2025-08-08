/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configTrigger = async function configTrigger (req, res, next, body) {
    // Creates new trigger attached to specified monitor.

    try {
        // Make sure that the params match the trigger_condition
        await utils.checkTriggerConditionParams(body);

        await db.insertPATrigger(body);
        await redis.insertPATrigger(body);
        res.status(200).send("New trigger was successfully configured");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }



};

module.exports.deleteTrigger = async function deleteTrigger (req, res, next, body) {
    // Deletes an existing trigger by Trigger ID
    try {
        // Get all triggers in Database
        let trigger = await db.getPATrigger(body);

        // Check to see if trigger provided exists
        if (trigger.length !== 0) {
            // Remove trigger from Database
            await db.removeTrigger(body);
            // Remove trigger from Redis and potentially update Redis Channel
            await redis.removeTrigger(trigger[0]);
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

module.exports.getTriggers = async function getTriggers (req, res, next, body) {
    // Return list of triggers on a given monitorId

    try {
        let triggerList = await db.getPATriggersByMonitor(body);
        let retVal = [];
        for await (let T of triggerList) {
            if (T.params) {
                T.params = JSON.parse(T.params);
            }
            retVal.push(T);
        }
        res.status(200).json(retVal);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

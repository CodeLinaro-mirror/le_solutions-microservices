/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configTripwireCreate = async function configTripwireCreate (req, res, next, body) {
    // Creates new Tripwire attached to specified monitor. 

    try {
        await db.insertTATripwire(body);
        await redis.insertTATripwire(body);
        res.status(200).send("New Tripwire was successfully configured");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

module.exports.configTripwireDelete = async function configTripwireDelete (req, res, next, body) {
    // Deletes an existing Tripwire by Tripwire ID
    try {
        // Get tripwire with the matching tripwireId in Database
        let tripwire = await db.getTATripwire(body);
        let triggers = await db.getTATriggersByTripwire(tripwire[0].tripwire_id);

        // Check to see if Tripwire provided exists
        if (tripwire.length !== 0) {
            // Remove Tripwire from Database
            await db.removeTripwire(body);
            // Remove Tripwire from Redis and potentially update Redis Channel
            await redis.removeTripwire(body, triggers);

            res.status(200).send('Tripwire was successfully deleted');

        } else {
            res.status(400).json({
                reason: 'tripwireId provided does not currently match any tripwire configuration in the system.'
            });
        }
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

module.exports.configTripwireGet = async function configTripwireGet (req, res, next, body) {
    // Return list of Tripwires on a given monitorId

    try {
        let tripwireList = await db.getTATripwiresByMonitor(body);
        let retVal = [];
        for await (let T of tripwireList) {
            T.wire = JSON.parse(T.wire);
            T.direction = JSON.parse(T.direction);
            retVal.push(T);
        }
        res.status(200).json(retVal);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

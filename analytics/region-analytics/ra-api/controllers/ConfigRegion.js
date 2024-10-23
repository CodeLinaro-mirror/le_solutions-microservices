/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configRegionCreate = async function configRegionCreate (req, res, next, body) {
    // Creates new region attached to specified monitor. 

    try {
        await db.insertRARegion(body);
        await redis.insertRARegion(body);
        res.status(200).send("New Region was successfully configured");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

module.exports.configRegionDelete = async function configRegionDelete (req, res, next, body) {
    // Deletes an existing trigger by Region ID
    try {
        // Get region with the matching regionId in Database
        let region = await db.getRARegion(body);
        let triggers = await db.getRATriggersByRegion(region[0].region_id);

        // Check to see if region provided exists
        if (region.length !== 0) {
            // Remove region from Database
            await db.removeRegion(body);
            // Remove region from Redis and potentially update Redis Channel
            await redis.removeRegion(body, triggers);
            res.status(200).send('Region was successfully deleted');

        } else {
            res.status(400).json({
                reason: 'regionId provided does not currently match any region configuration in the system.'
            });
        }
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

module.exports.configRegionGet = async function configRegionGet (req, res, next, body) {
    // Return list of Regions on a given monitorId

    try {
        let regionList = await db.getRARegionsByMonitor(body);
        let retVal = [];
        for await (let R of regionList) {
            R.coordinates = JSON.parse(R.coordinates);
            retVal.push(R);
        }
        res.status(200).json(retVal);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

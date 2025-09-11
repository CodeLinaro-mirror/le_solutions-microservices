/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 */
'use strict';

const db = require('./database');
const redis = require('./redis');
const config = require('../config/config');

async function populateRedis() {
    try {
        let regions = await db.getAllVARegions();
        let triggers = await db.getAllVATriggers();
        console.info(regions);
        console.info(triggers);
        await redis.populateRedis(regions, triggers);
    } catch (e) {
        console.error(e);
        throw new Error({msg: e.message});
    }
}

async function cameraUpdates() {
    redis.listenToChannel(config.redisCameraUpdates, async function(err, data) {
        try {
            if (err) {
                console.error(err);
            } else {
                data = JSON.parse(data);
                console.log(data);
                // Update Camera
                if (data.message_type == 'Update') {
                    // Not much to update.
                }
                // Delete Camera
                else if (data.message_type == 'Removal') {
                    // Delete all Triggers and Alerts relating to camera
                    db.removeAllMonitor(data.camera_object.camera_id);
                }
                // New Camera
                else {}
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

async function initializeDB() {
    try {
        console.log("Info: Creating Tables in DB if they do not exist");
        let dbCheck = await db.initializeCheckTables();
        console.debug(dbCheck);
    } catch (e) {
        console.error(e);
        throw new Error({msg: e.message});
    }
}

function convertToms(time) {
    if (time > 10000000000) return time;
    else return time*1000;
}

function validateTrigger(body) {
    if (!config.validTriggers.includes(body.trigger_condition)) {
        throw new Error(`Error trigger condition invalid. Please enter one of the following: [${config.validTriggers}]`);
    }
    if (body.trigger_condition == "vehicle_count_over" || body.trigger_condition == "vehicle_count_under") {
        if (body.params == null || body.params == undefined) {
            throw new Error ('Error Params must be provided for the trigger condition in an Array of name value objects')
        }
    }
}

function greaterThanQueryInterval(fromTime, toTime, interval) {
    let fromDate = new Date(convertToms(fromTime));
    let toDate = new Date(convertToms(toTime));

    return toDate - fromDate > interval;
}

function convertMillisecondsToMinutes(time) {
    return time / (60 * 1000);
}

module.exports = {
    populateRedis,
    cameraUpdates,
    initializeDB,
    convertToms,
    validateTrigger,
    greaterThanQueryInterval,
    convertMillisecondsToMinutes
};

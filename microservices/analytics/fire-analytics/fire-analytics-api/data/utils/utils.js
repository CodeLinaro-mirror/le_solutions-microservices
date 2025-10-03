/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const db = require('./database');
const redis = require('./redis');
const config = require('../config/config');

async function populateRedis() {
    try {
        let triggers = await db.getAllFATriggers();
        console.log(triggers);
        await redis.populateRedis(triggers);
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
                } else if (data.message_type == 'Removal') {
                    // Delete all Triggers and Alerts relating to camera
                    db.removeTrigger(data.camera_object.camera_id);
                } else {}
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

module.exports = {
    populateRedis,
    cameraUpdates,
    initializeDB
};

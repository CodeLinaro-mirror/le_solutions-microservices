/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 */
'use strict';

const db = require('./database');
const redis = require('./redis');
const config = require('../config/config');

async function populateRedis() {
    try {
        let triggers = await db.getAllPATriggers();
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
                }
                // Delete Camera
                else if (data.message_type == 'Removal') {
                    // Delete all Triggers and Alerts relating to camera
                    db.removeTrigger(data.camera_object.camera_id);
                }
                // New Camera
                else {}
            }
        } catch (e) {
            console.error(e.message);
        }
    });
}

module.exports = {
    populateRedis,
    cameraUpdates
};

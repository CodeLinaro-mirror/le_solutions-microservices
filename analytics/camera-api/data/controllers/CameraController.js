/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const db = require('../utils/database');
const redis = require('../utils/redis');


module.exports.addCamera = async function addCamera (req, res, next, body) {
    // Creates new camera

    try {
        await db.insertNewCamera(body);
        await redis.publishNewCamera(body);
        res.status(200).send("New camera configuration was successfully added");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

module.exports.deleteCamera = async function deleteCamera (req, res, next, body) {
    // Deletes an existing Camera

    try {
        // Get camera in Database
        let camera = await db.getCameraById(body);

        // Check to see if camera provided exists
        if (camera.length !== 0) {
            // Remove camera from Database
            await db.removeCamera(body);
            // Remove camera from Redis and potentially update Redis Channel
            await redis.publishCameraRemoval(camera[0]);
            res.status(200).send('Camera was successfully deleted');

        } else {
            res.status(400).json({
                reason: 'cameraId provided does not currently match any trigger configuration in the system.'
            });
        }
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

module.exports.getCameras = async function getCameras (req, res, next, body) {
    // Return list of Cameras

    try {
        let cameraList = await db.getAllCameras(body);
        res.status(200).json(cameraList);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

module.exports.updateCamera = async function updateCamera (req, res, next, body) {
    // Updates an existing camera given a camera ID

    try {
        let camera = await db.getCameraById(body.camera_id);

        if (camera.length != 0) {
            await db.updateCameraById(body);
            await redis.publishCameraUpdate(body);
            res.status(200).send('Camera configuration was successfully updated.');
        } else {
            res.status(400).json({
                reason: 'No camera found with given camera ID.'
            })
        }
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

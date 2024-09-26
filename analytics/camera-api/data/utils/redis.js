/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');

const { createClient } = require('redis');

// Host can change. Experiment with Docker Compose more to mitigate this.
const client = createClient({
    socket: {
        port: config.redisPort,
        host: config.redisHost
    }
});
(async () => {
    await client.connect();
})();
client.on('connect', () => {
    console.log('connected');
});
client.on('error', (err) => {
    console.log(err);
});


async function publishNewCamera(data) {
    try {
        let msg = {
            'message_type': 'New',
            'camera_object': data
        };
        const cameraClient = client.duplicate();
        await cameraClient.connect();
        await cameraClient.publish(config.redisCameraUpdates, JSON.stringify(msg));
        // {
        //     'camera_name': data.camera_name,
        //     'camera_id': data.camera_id,
        //     'rtsp_url': data.rtsp_url,
        //     'camera_location': data.camera_location,
        //     'camera_fov': data.camera_fov,
        //     'camera_direction': data.camera_direction 
        // }
    } catch (e) {
        throw e;
    }
}

async function publishCameraRemoval(data) {
    try {
        let msg = {
            'message_type': 'Removal',
            'camera_object': data
        };
        const cameraClient = client.duplicate();
        await cameraClient.connect();
        await cameraClient.publish(config.redisCameraUpdates, JSON.stringify(msg));
    } catch (e) {
        throw e;
    }
}

async function publishCameraUpdate(data) {
    try {
        let msg = {
            'message_type': 'Update',
            'camera_object': data
        };
        const cameraClient = client.duplicate();
        await cameraClient.connect();
        await cameraClient.publish(config.redisCameraUpdates, JSON.stringify(msg));
    } catch (e) {
        throw e;
    }
}

module.exports = {
    publishNewCamera,
    publishCameraRemoval,
    publishCameraUpdate
};

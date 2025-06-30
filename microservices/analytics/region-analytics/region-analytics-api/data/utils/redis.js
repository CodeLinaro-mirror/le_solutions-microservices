/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');

const { createClient } = require('redis');

// TODO: Host can change. Experiment with Docker Compose more to mitigate this.
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

async function insertRARegion(data) {
    try {
        data.coordinates = JSON.stringify(data.coordinates);
        let res = await client.hSet(`${config.redisRARegionKey}`,
            `${data.region_id}`, JSON.stringify(data));
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function insertRATrigger(data) {
    try {
        if(data.params) data.params = JSON.stringify(data.params);
        let res = await client.hSet(`${config.redisRATriggerKey}`,
            `${data.trigger_id}`, JSON.stringify(data));
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function populateRedis(regions, triggers) {
    try {
        // Set data in Redis
        regions.forEach(async (data) => {
            await client.hSet(`${config.redisRARegionKey}`, `${data.region_id}`, JSON.stringify(data));
        });
        triggers.forEach(async (data) => {
            await client.hSet(`${config.redisRATriggerKey}`, `${data.trigger_id}`, JSON.stringify(data));
        });
    } catch (e) {
        throw e;
    }
}

async function listenToChannel(data, cb) {
    try {
        const RAAlertSubscriber = client.duplicate();
        await RAAlertSubscriber.connect();
        console.log('listening to', data);
        await RAAlertSubscriber.subscribe(data, (message) => {
            cb(false, message);
        });
    } catch (e) {

    }
}

async function removeRegion(data, triggers) {
    try {
        for await (let t of triggers) {
            await client.hDel(`${config.redisRATriggerKey}`, `${t.trigger_id}`);
        }
        let res = await client.hDel(`${config.redisRARegionKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

async function removeTrigger(data) {
    try {
        let res = await client.hDel(`${config.redisRATriggerKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

module.exports = {
    insertRARegion,
    insertRATrigger,
    populateRedis,
    listenToChannel,
    removeRegion,
    removeTrigger
};

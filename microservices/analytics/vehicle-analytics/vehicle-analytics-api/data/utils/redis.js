/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const { randomUUID } = require('crypto');

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

async function insertVARegion(data) {
    try {
        let res = await client.hSet(`${config.redisRegionKey}`,
            `${data.region_id}`, JSON.stringify(data));
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function insertVATrigger(data) {
    try {
        let res = await client.hSet(`${config.redisTriggerKey}`,
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
            if (data.coordinates && data.coordinates.length > 0) {
                data.coordinates = JSON.parse(data.coordinates);
            }
            await client.hSet(`${config.redisRegionKey}`, `${data.region_id}`, JSON.stringify(data));
        });
        triggers.forEach(async (data) => {
            await client.hSet(`${config.redisTriggerKey}`, `${data.trigger_id}`, JSON.stringify(data));
        });
    } catch (e) {
        throw e;
    }
}

async function listenToChannel(data, cb) {
    try {
        const VAAlertSubscriber = client.duplicate();
        await VAAlertSubscriber.connect();
        console.log('listening to', data);
        await VAAlertSubscriber.subscribe(data, (message) => {
            cb(false, message);
        });
    } catch (e) {

    }
}

async function removeRegion(data, triggers) {
    try {
        for await (let t of triggers) {
            await client.hDel(`${config.redisTriggerKey}`, `${t.trigger_id}`);
        }
        let res = await client.hDel(`${config.redisRegionKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

async function removeTrigger(data) {
    try {
        let res = await client.hDel(`${config.redisTriggerKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

async function publishAndListenOnce(data, cb) {
    try {
        let VARedisClient = client.duplicate();
        await VARedisClient.connect();
        let sync_id = randomUUID();
        // Send a message on the Analytics Channel
        // {
        //     'sync_id': sync_id,
        //     'region_id': data.regionId,
        //     'from_time': data.fromTime,
        //     'to_time': data.toTime,
        //     'analytics_type': analytics_type
        // }

        // Insert unique ID to ensure a synchronous callback
        data.sync_id = sync_id;

        // Make sure data matches above format
        await VARedisClient.publish(config.redisVAAnalyticsChannel, JSON.stringify(data));

        // Listen to the analytics channel until a message is received with same ID
        await VARedisClient.subscribe(config.redisVAAnalyticsChannel, (message) => {
            try {
                // Check to see if message matches unique ID from earlier
                let retData = JSON.parse(message);

                // If message matches the unique ID run Callback
                if (retData.sync_id == sync_id && retData.result !== undefined) {
                    cb(false, retData.result);

                    VARedisClient.unsubscribe();
                    VARedisClient.quit();
                }
            } catch (e) {
                console.error(e.message);
                cb(true, e)
            }
        });
    } catch (e) {
        console.error(e);
    }
}

module.exports = {
    insertVARegion,
    insertVATrigger,
    populateRedis,
    listenToChannel,
    removeRegion,
    removeTrigger,
    publishAndListenOnce
};

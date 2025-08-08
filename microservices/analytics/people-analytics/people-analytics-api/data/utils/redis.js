/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const { randomUUID } = require('crypto');

const { createClient } = require('redis');

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


async function insertPATrigger(data) {
    try {
        // let paramsString = JSON.stringify(data.params);
        // Set data in Redis
        let res = await client.hSet(`${config.redisTriggerKey}`, `${data.trigger_id}`, JSON.stringify(data)
        // {
        //     'monitor_id': data.monitor_id,
        //     'trigger_id': data.trigger_id,
        //     'trigger_name': data.trigger_name,
        //     'trigger_condition': data.trigger_condition,
        //     'params': paramsString
        // }
        );
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function populateRedis(triggers) {
    try {
        // Set data in Redis
        triggers.forEach(async (data) => {
            if (data.params && data.params.length > 0) {
                data.params = JSON.parse(data.params);
            }
            await client.hSet(`${config.redisTriggerKey}`, `${data.trigger_id}`, JSON.stringify(data));
        });
    } catch (e) {
        throw e;
    }
}

async function listenToChannel(data, cb) {
    try {
        const PAAlertSubscriber = client.duplicate();
        await PAAlertSubscriber.connect();
        console.log('listening to', data);
        await PAAlertSubscriber.subscribe(data, (message) => {
            cb(false, message);
        });
    } catch (e) {

    }
}

async function removeTrigger(data) {
    try {
        let res = await client.hDel(`${config.redisTriggerKey}`, `${data.trigger_id}`);
    } catch (e) {
        throw e;
    }
}

async function publishAndListenOnce(data, cb) {
    try {
        let PARedisClient = client.duplicate();
        await PARedisClient.connect();
        let sync_id = randomUUID();
        // Send a message on the Analytics Channel
        // {
        //     'sync_id': sync_id,
        //     'monitor_id': data.monitorId,
        //     'from_time': data.fromTime,
        //     'to_time': data.toTime,
        //     'analytics_type': analytics_type, // either 'count' or 'heatmap'
        // }

        // Insert unique ID to ensure a synchronous callback
        data.sync_id = sync_id;

        // Make sure data matches above format
        await PARedisClient.publish(config.redisPAAnalyticsChannel, JSON.stringify(data));

        // Listen to the analytics channel until a message is received with same ID
        await PARedisClient.subscribe(config.redisPAAnalyticsChannel, (message) => {
            try {
                // Check to see if message matches unique ID from earlier
                let retData = JSON.parse(message);
                // If message matches the unique ID run Callback
                if (retData.sync_id == sync_id) {
                    cb(false, retData.result);

                    PARedisClient.unsubscribe();
                    PARedisClient.quit();
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
    insertPATrigger,
    populateRedis,
    listenToChannel,
    removeTrigger,
    publishAndListenOnce
};

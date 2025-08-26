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

async function insertTATripwire(data) {
    try {
        let res = await client.hSet(`${config.redisTATripwireKey}`,
            `${data.tripwire_id}`, JSON.stringify(data));
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function insertTATrigger(data) {
    try {
        let res = await client.hSet(`${config.redisTATriggerKey}`,
            `${data.trigger_id}`, JSON.stringify(data));
        console.log(res);
    } catch (e) {
        throw e;
    }
}

async function populateRedis(tripwires, triggers) {
    try {
        // Set data in Redis
        tripwires.forEach(async (data) => {
            if (data.wire && data.wire.length > 0) {
                data.wire = JSON.parse(data.wire);
            }
            if (data.direction && data.direction.length > 0) {
                data.direction = JSON.parse(data.direction);
            }
            await client.hSet(`${config.redisTATripwireKey}`, `${data.tripwire_id}`, JSON.stringify(data));
        });
        triggers.forEach(async (data) => {
            await client.hSet(`${config.redisTATriggerKey}`, `${data.trigger_id}`, JSON.stringify(data));
        });
    } catch (e) {
        throw e;
    }
}

async function listenToChannel(data, cb) {
    try {
        const TAAlertSubscriber = client.duplicate();
        await TAAlertSubscriber.connect();
        console.log('listening to', data);
        await TAAlertSubscriber.subscribe(data, (message) => {
            cb(false, message);
        });
    } catch (e) {

    }
}

async function removeTripwire(data, triggers) {
    try {
        for await (let t of triggers) {
            await client.hDel(`${config.redisTATriggerKey}`, `${t.trigger_id}`);
        }
        let res = await client.hDel(`${config.redisTATripwireKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

async function removeTrigger(data) {
    try {
        let res = await client.hDel(`${config.redisTATriggerKey}`, `${data}`);
    } catch (e) {
        throw e;
    }
}

async function publishAndListenOnce(data, cb) {
    try {
        let TARedisClient = client.duplicate();
        await TARedisClient.connect();
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
        await TARedisClient.publish(config.redisTAAnalyticsChannel, JSON.stringify(data));

        // Listen to the analytics channel until a message is received with same ID
        await TARedisClient.subscribe(config.redisTAAnalyticsChannel, (message) => {
            try {
                // Check to see if message matches unique ID from earlier
                let retData = JSON.parse(message);
                // If message matches the unique ID run Callback
                if (retData.sync_id == sync_id) {
                    cb(false, retData.result);

                    TARedisClient.unsubscribe();
                    TARedisClient.quit();
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
    insertTATripwire,
    insertTATrigger,
    populateRedis,
    listenToChannel,
    removeTripwire,
    removeTrigger,
    publishAndListenOnce
};

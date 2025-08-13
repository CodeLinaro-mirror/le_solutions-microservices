/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');

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

module.exports = {
    insertTATripwire,
    insertTATrigger,
    populateRedis,
    listenToChannel,
    removeTripwire,
    removeTrigger
};

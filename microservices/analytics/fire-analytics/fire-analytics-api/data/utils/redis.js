/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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


async function insertFATrigger(data) {
    try {
        // let paramsString = JSON.stringify(data.params);
        // Set data in Redis
        if (data.params) data.params = JSON.stringify(data.params);
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
            await client.hSet(`${config.redisTriggerKey}`, `${data.trigger_id}`, JSON.stringify(data));
        });
    } catch (e) {
        throw e;
    }
}

async function listenToChannel(data, cb) {
    try {
        const FAAlertSubscriber = client.duplicate();
        await FAAlertSubscriber.connect();
        console.log('listening to', data);
        await FAAlertSubscriber.subscribe(data, (message) => {
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

module.exports = {
    insertFATrigger,
    populateRedis,
    listenToChannel,
    removeTrigger
};

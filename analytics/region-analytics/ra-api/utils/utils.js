/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const db = require('./database');
const redis = require('./redis');

async function populateRedis() {
    try {
        let regions = await db.getAllRARegions();
        let triggers = await db.getAllRATriggers();
        console.info(regions);
        console.info(triggers);
        await redis.populateRedis(regions, triggers);
    } catch (e) {
        console.error(e);
        throw new Error({msg: e.message});
    }
}
module.exports = {
    populateRedis
};

/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 */
'use strict';

const db = require('./database');

async function initializeDB() {
    try {
        console.log("Info: Creating Tables in DB if they do not exist");
        let dbCheck = await db.initializeCheckTables();
        console.debug(dbCheck);
    } catch (e) {
        console.error(e);
        throw new Error({msg: e.message});
    }
}

module.exports = {
    initializeDB
};

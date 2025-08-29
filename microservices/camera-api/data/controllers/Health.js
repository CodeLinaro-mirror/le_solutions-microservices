/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

module.exports.getHealth = async function getHealth (req, res, next, body) {
    try {
        // Check if Database and tables have properly been initialized
        let dbCheck = await db.initializeCheckTables();
        if (dbCheck.length == 2) {
            res.status(200).send(`Health Check Passed`);
        } else {
            res.status(500).send(`Problem initializing Database tables. Is the server available?`);
        }
    } catch (err) {
        res.status(500).send(`Problem setting up. Info: ${err}`);
    }
};


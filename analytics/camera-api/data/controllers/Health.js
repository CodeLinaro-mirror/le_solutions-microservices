/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

module.exports.getHealth = async function getHealth (req, res, next, body) {
    try {
        // Extremely simple health check for now
        res.status(200).send(`Health Check Passed`);
    } catch (err) {
        res.status(500).send(`Error updating Job. ${err}`);
    }
};


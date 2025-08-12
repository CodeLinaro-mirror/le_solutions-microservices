/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

// Get Environment Variables from env file if not production
if (process.env.NODE_ENV !== 'production') {
    // Env Vars are obtained from Kubernetes/other platform in production
    require('dotenv').config();
}

const config = {
    
    mariadbName: process.env.mariadbName,
    mariadbHost: process.env.mariadbHost,
    mariadbPass: process.env.mariadbPass,
    mariadbPort: process.env.mariadbPort,
    mariadbUser: process.env.mariadbUser,
    default_db: process.env.default_db,
    redisHost: process.env.redisHost,
    redisPort: process.env.redisPort,
    redisTAAlertsChannel: process.env.redisTAAlertsChannel,
    redisTATripwireKey: process.env.TA_TRIPWIRE_KEY,
    redisTATriggerKey: process.env.TA_TRIGGER_KEY,
    redisCameraUpdates: process.env.redisCameraUpdates
};

module.exports = config;

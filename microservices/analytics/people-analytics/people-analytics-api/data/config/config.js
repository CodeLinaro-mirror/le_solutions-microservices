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

    accessoriesTriggerConditions: ["required_accessories", "restricted_accessories"],
    peopleCountTriggerConditions: ["occupancy_changed", "occupancy_over", "occupancy_under", "loitering_over"],
    mariadbName: process.env.mariadbName,
    mariadbHost: process.env.mariadbHost,
    mariadbPass: process.env.mariadbPass,
    mariadbPort: process.env.mariadbPort,
    mariadbUser: process.env.mariadbUser,
    default_db: process.env.default_db,
    redisHost: process.env.redisHost,
    redisPort: process.env.redisPort,
    redisPAAlertsChannel: process.env.redisPAAlertsChannel,
    redisPAAnalyticsChannel: process.env.redisPAAnalyticsChannel,
    redisTriggerKey: process.env.PA_TRIGGER_KEY,
    redisCameraUpdates: process.env.redisCameraUpdates,
};

module.exports = config;

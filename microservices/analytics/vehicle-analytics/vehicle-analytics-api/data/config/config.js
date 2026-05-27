/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
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
    redisVAAlertsChannel: process.env.redisVAAlertsChannel,
    redisVAAnalyticsChannel: process.env.redisVAAnalyticsChannel,
    redisTriggerKey: process.env.VA_TRIGGER_KEY,
    redisRegionKey: process.env.VA_REGION_KEY,
    redisCameraUpdates: process.env.redisCameraUpdates,
    validTriggers: ["vehicle_count_changed", "vehicle_count_over", "vehicle_count_under", "vehicle_dwell_over"]
};

module.exports = config;

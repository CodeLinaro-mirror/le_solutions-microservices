/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const redis = require("../utils/redis");
const utils = require("../utils/utils");

const MIN_QUERY_INTERVAL = 300_000;   // 5 minutes
const AnalyticsType = "count";

module.exports.getVehicleAnalyticsCountRegion = async function getVehicleAnalyticsCountRegion(req, res, next, body) {
    try {
        let data = {
            "region_id"       : req.query.regionId,
            "from_time"       : req.query.fromTime,
            "to_time"         : req.query.toTime,
            "analytics_type" : AnalyticsType
        };

        if (!utils.greaterThanQueryInterval(data.from_time, data.to_time, MIN_QUERY_INTERVAL)) {
            return res.status(400).json({
                reason: `Please enter a timeframe of larger than ${utils.convertMillisecondsToMinutes(MIN_QUERY_INTERVAL)} minutes.`
            });
        }

        redis.publishAndListenOnce(data, async (err, retData) => {
            if (err) {
                res.status(400).json({
                    reason: retData.message
                });
            } else {
                let resData = {
                    min     : retData.min,
                    max     : retData.max,
                    average : retData.average
                };
                res.status(200).json(resData);
            }
        });
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
}
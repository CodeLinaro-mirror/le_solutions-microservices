/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const db = require('../utils/database');
const utils = require('../utils/utils');

module.exports.getAnalyticsCount = async function getAnalyticsCount (req, res, next, body) {
    // send Analytics request and get back data
    try {
        /*
        /   Send data to the PA Server through analytics redis channel
        /   Get Data back from the PA Server
        /   Send it back in this specific http call
        */

        let data = {
            'monitor_id': req.query.monitorId,
            'from_time': req.query.fromTime,
            'to_time': req.query.toTime,
            'analytics_type': 'count'
        };
        // Make sure that timeframe is 5minutes
        if (!isGreaterThan5Min(data.from_time, data.to_time)) {
            res.status(400).json({
                reason: 'Error: Please enter a timeframe of larger than 5 minutes.'
            });
        }
        redis.publishAndListenOnce(data, async (err, retData) => {
            if (err) {
                res.status(400).json({
                    reason: retData.message
                });
            } else {
                // Callback runs once a message is received that matches the one sent
                let resData = {
                    min: retData.min,
                    max: retData.max,
                    average: retData.average
                };
                // Return data from PA Server
                res.status(200).json(resData);
            }
        });
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

function isGreaterThan5Min(fromTime, toTime) {

    let fromDate = new Date(utils.convertToms(fromTime));
    let toDate = new Date(utils.convertToms(toTime));

    let ret = Math.abs(toDate - fromDate) > (5 * 60 * 1000) ? true : false;
    return ret;
}

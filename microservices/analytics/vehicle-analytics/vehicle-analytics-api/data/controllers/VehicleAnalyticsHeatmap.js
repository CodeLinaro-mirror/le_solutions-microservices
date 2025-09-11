/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const db = require('../utils/database');
const redis = require('../utils/redis');
const utils = require('../utils/utils')
const AnalyticsType = "heatmap"

const Invertal  = 300_000;

module.exports.getVehicleAnalyticsHeatmap = async function getVehicleAnalyticsHeatmap(req, res, next, body) {
    try{
        let data = {
            "monitor_id"      : req.query.monitorId,
            "from_time"       : req.query.fromTime,
            "to_time"         : req.query.toTime,
            "analytics_type" : AnalyticsType
        };

        //validate parameters
        const error = !utils.greaterThanQueryInterval(data.from_time,data.to_time,Invertal);
        if(error) {
            return res.status(400).json({ 
                reason: 'Error: Must enter time interval larger than 5 minutes.' });
        }
        
        //Query heatmap analystics
        redis.publishAndListenOnce(data, async(error,retData)=>{
            if(error){
                return res.status(400).json({
                reason: retData.message
            });
            }else{
                res.status(200).json(retData);
            }
        });

    }catch(e){
        res.status(400).json({
            reason: e.message
        });
    }
}
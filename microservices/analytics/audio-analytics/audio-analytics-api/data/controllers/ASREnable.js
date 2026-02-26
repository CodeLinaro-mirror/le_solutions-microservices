/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configASREnable = async function configASREnable (req, res, next, body) {
    try {

        // Publish to redis config channel
        body.message_type = "enable";
        redis.publish(config.redisASRConfigChannel, body);

        res.status(200).send("ASR processing configuration updated.");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

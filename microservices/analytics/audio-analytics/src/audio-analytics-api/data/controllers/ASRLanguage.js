/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const redis = require('../utils/redis');
const utils = require('../utils/utils');


module.exports.configASRLanguage = async function configASRLanguage (req, res, next, body) {
    try {
        //validate parameters
        if(utils.validateLanguage(body)) {
            return res.status(400).json({ 
                reason: 'Error: Language entered is not supported.' });
        }

        // Publish to redis config channel
        body.message_type = "language";
        redis.publish(config.redisASRConfigChannel, body);

        res.status(200).send("Language configuration updated.");
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const messages = require('../utils/messages');

module.exports.closeTranslation = async function closeTranslation (req, res, next, body) {
    try {
        console.log('Close translation request received');

        messages.publish(config.t2tTranslationIn, {message_type: 'translation_close'}, ()=>{});
        res.status(200).send('Translation session successfully closed.');
    } catch (e) {
        console.error('Close translation error:', e);
        res.status(500).json({
            error: {
                message: e.message,
                type: "server_error",
                param: null,
                code: null
            }
        });
    }
};

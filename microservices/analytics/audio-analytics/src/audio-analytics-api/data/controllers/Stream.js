/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

module.exports.streamAudioWebSocket = async function streamAudioWebSocket (req, res, next, body) {
    try {
        res.sendStatus(101);
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }
};

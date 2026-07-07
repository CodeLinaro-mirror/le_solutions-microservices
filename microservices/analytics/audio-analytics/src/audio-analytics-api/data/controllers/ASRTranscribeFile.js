/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');
const fs = require('fs');

module.exports.transcribeASRFile = async function transcribeASRFile (req, res, next, body) {
    try {
        let file = req.files[0];
        fs.writeFile(`${config.uploadPath}${file.originalname}`, file.buffer, (err) => {
            res.status(200).send("File Uploaded.");
        });
    } catch (e) {
        res.status(400).json({
            reason: e.message
        });
    }

    

};

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 */
'use strict';

const config = require('../config/config');
const fs = require('fs');

async function writeLocalFile(file) {
    fs.writeFileSync(`${config.uploadPath}${file.originalname}`, file.buffer);
}

async function readFileAsBase64(file) {
    const fileBuffer = fs.readFileSync(`${config.uploadPath}${file.originalname}`);
    return fileBuffer.toString('base64');
}
module.exports = {
    writeLocalFile,
    readFileAsBase64
};

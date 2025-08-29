/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- jest.config.js
 * Description :- Jest Configuration File.
 */
/** @type {import('jest').Config} */

const config = {
    collectCoverage: true,
    coverageProvider: "babel",
    coverageDirectory: "coverage/unit",
    verbose: true,
    reporters: [
        'default',
        ['jest-junit', { outputDirectory: 'reports/unit', outputName: 'report.xml' }],
        ['jest-html-reporter', { pageTitle: "Test Report", outputPath: "./reports/unit/test-report.html" }]
    ]
};

export default config;

/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- logging.js
 * Description :- The utility file handling the application loggin.
 */
'use strict';

// Config JSON key
export const CONFIG_LOG_LEVEL_KEY = 'log_level';

// Supported Logging Level
export const DEBUG = 'DEBUG';
export const INFO = 'INFO';
export const WARN = 'WARN';
export const ERROR = 'ERROR';

// List of Supported Logging Level where index of the list signifies the precendence of the level
const logLevels = [DEBUG, INFO, WARN, ERROR];

/**
 * The utility function to get current date in string format
 * @returns {String} Current date in String format
 */
function getCurrentDateString() {
    let date_time = new Date();

    // Get current date
    // Adjust 0 before single digit date
    let date = ("0" + date_time.getDate()).slice(-2);

    // Get current month
    let month = ("0" + (date_time.getMonth() + 1)).slice(-2);

    // Get current year
    let year = date_time.getFullYear();

    // Get current hours
    let hours = date_time.getHours();

    // Get current minutes
    let minutes = date_time.getMinutes();

    // Get current seconds
    let seconds = date_time.getSeconds();

    // Returns date & time in YYYY-MM-DD HH:MM:SS format
    return year + "-" + month + "-" + date + " " + hours + ":" + minutes + ":" + seconds;
}

/**
 * The wrapper function to be used by application for logging.
 * 
 * @param {string} logLevel [The Log level to be used for logging.]
 * @param {string} msgToLog [The message to be logged.]
 */
export function log(logLevel, msgToLog) {
    // Retrieve allowed log level from Environment variable
    var allowedLogLevel = process.env.LOG_LEVEL;
    if (allowedLogLevel == null || typeof allowedLogLevel == undefined) {
        allowedLogLevel = DEBUG;
    }
    if (logLevels.indexOf(logLevel) >= logLevels.indexOf(allowedLogLevel)) {
        var finalMsgToLog = `[${getCurrentDateString()}]:[${logLevel}]:${msgToLog}`;
        switch (logLevel) {
            case 'DEBUG':
                console.debug(finalMsgToLog);
                break;
            case 'INFO':
                console.info(finalMsgToLog);
                break;
            case 'WARN':
                console.warn(finalMsgToLog);
                break;
            case 'ERROR':
                console.error(finalMsgToLog);
                break;
        }
    }
}
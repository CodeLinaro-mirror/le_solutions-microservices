/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

const config = require('../config/config');

const mariadb = require('mariadb');
const pool = mariadb.createPool({
    host: config.mariadbHost,
    port: config.mariadbPort,
    user: config.mariadbUser, 
    password: config.mariadbPass,
    connectionLimit: 25,
    database: config.default_db,
});

async function fetchConn() {
    let conn = await pool.getConnection();
    if (process.env.LOG_LEVEL >= 2) {
        console.debug("Total connections: ", pool.totalConnections(), "Active connections: ", pool.activeConnections(), "Idle connections: ", pool.idleConnections());
    }
    return conn;
}

function cleanInput(data) {
    // For now escape all apostrophes if type string just in case
    if (typeof data == 'object') {
        Object.keys(data).forEach((key) => {
            if (typeof data[key] == 'string') {
                data[key] = data[key].replace("'", "''");
            }
        });

    } else if (typeof data == 'string') {
        data = data.replace("'", "''");
    }
    return data;
}

function makeUpdateQuery(data) {
    let res = `UPDATE camera_config SET `;
    let i=1;
    let dataLength = Object.keys(data).length;
    for (const [k, v] of Object.entries(data)) {
        if (i<dataLength) {
            if (typeof v == 'number') 
                res = res +
                    `${k} = ${v}, `;
            else
                res = res +
                    `${k} = '${v}', `;
        } else {
            if (typeof v == 'number')
                res = res +
                    `${k} = ${v} `;
            else
                res = res +
                    `${k} = '${v}' `;
        }
        i++;
    }
    res = res + `WHERE camera_id = '${data.camera_id}';`;
    return res;
}

async function insertNewCamera(data) {
    let conn;
    try {
        data = cleanInput(data);
        conn = await fetchConn();
        const res = await conn.query(`INSERT INTO camera_config ` +
            `(camera_name, camera_id, rtsp_url, camera_location, camera_fov, camera_direction)` +
            ` value ('${data.camera_name}', '${data.camera_id}', '${data.rtsp_url}',` + 
            `'${data.camera_location}', '${data.camera_fov}', '${data.camera_direction}');`);
        conn.end();
        console.log(res);
    } catch (e) {
        if (e.code && e.code === 'ER_DUP_ENTRY') {
            throw new Error(`Error: Duplicate entry already exists for the given trigger_id ${data.trigger_id}`, e.message);
        } else {
            console.error(e);
        }
        throw e;
    }
}

async function getAllCameras() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from camera_config;`);
        conn.end();
        return res;
    } catch (e) {
        throw e;
    }
}



async function getCameraById(data) {
    let conn;
    try {
        data = cleanInput(data);
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from camera_config where camera_id = '${data}';`);
        conn.end();
        return res;
    } catch (e) {
        throw e;
    }
}

async function removeCamera(data) {
    let conn;
    try {
        data = cleanInput(data);
        conn = await fetchConn();
        const res = await conn.query(`DELETE FROM camera_config WHERE ` +
            `camera_id = '${data}';`);
        conn.end();
        return res;
    } catch (e) {
        throw e;
    }
}

async function updateCameraById(data) {
    let conn;
    try {
        conn = await fetchConn();
        let queryString = makeUpdateQuery(data);
        const res = await conn.query(queryString);
        console.log(queryString);
        conn.end();
        console.log(res);
    } catch (e) {
        if (e.code && e.code === 'ER_DUP_ENTRY') {
            throw new Error(`Error: Duplicate entry already exists for the given trigger_id ${data.trigger_id}`, e.message);
        } else {
            console.error(e);
        }
        throw e;
    }
}

async function initializeCheckTables() {
    let conn;
    try {
        let res = [];
        res.push(await createDatabase());
        res.push(await createCameraTable());
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function createDatabase() {
    let conn;
    try {
        const dbpool = mariadb.createPool({
            host: config.mariadbHost,
            port: config.mariadbPort,
            user: config.mariadbUser, 
            password: config.mariadbPass
        });
        
        conn = await dbpool.getConnection();

        const res = await conn.query(`CREATE DATABASE IF NOT EXISTS iot_solutions;`);
        if (res.affectedRows == 1)
            console.log("iot_solutions database created");
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function createCameraTable() {
    let conn;
    try {
        conn = await fetchConn();

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS camera_config ` +
            `(camera_name text, camera_id VARCHAR(255) not null unique, ` +
            `rtsp_url text, camera_location text, ` +
            `camera_fov int, camera_direction int);`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

module.exports = {
    insertNewCamera,
    getAllCameras,
    getCameraById,
    removeCamera,
    updateCameraById,
    initializeCheckTables
};

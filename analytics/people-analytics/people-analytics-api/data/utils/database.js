/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * 
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

async function insertPATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        let paramsString = JSON.stringify(data.params);

        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO fov_triggers ` +
            `(monitor_id, trigger_id, trigger_name, trigger_condition, params)` +
            ` value ('${data.monitor_id}', '${data.trigger_id}', '${data.trigger_name}',` + 
            `'${data.trigger_condition}', '${paramsString}');`);
        console.log(res);
    } catch (e) {
        if (e.code && e.code === 'ER_DUP_ENTRY') {
            throw new Error(`Error: Duplicate entry already exists for the given trigger_id ${data.trigger_id}`, e.message);
        } else {
            console.error(e);
        }
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAllPATriggers() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from fov_triggers;`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getPATriggersByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from fov_triggers where monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getPATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from fov_triggers where trigger_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertPAAlert(jsonData) {
    let conn;
    try {
        let data = JSON.parse(jsonData);
        // Escape apostrophes
        data = cleanInput(data);

        let causesString = JSON.stringify(data.causes);
        conn = await fetchConn();
        const res = await conn.query(`INSERT INTO fov_alerts ` +
            `(source_trigger_id, time, causes)` +
            ` value ('${data.source_trigger.trigger_id}', FROM_UNIXTIME(${data.time}),` +
            ` '${causesString}');`);
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAlerts(data) {
    let conn;
    try {
        conn = await fetchConn();

        let retVal = [];
        let monMap = data.monitorIds[0].split(",");
        let rows = await conn.query(`select * FROM fov_alerts fa` +
            ` inner join fov_triggers ft on fa.source_trigger_id = ft.trigger_id ` +
            ` WHERE ft.monitor_id IN (${monMap.map(i=>`'${i}'`)}) AND ` +
            ` time >= FROM_UNIXTIME(${data.fromTime}) AND time <= FROM_UNIXTIME(${data.toTime}) ORDER BY time;`);
        retVal = [...retVal, ...rows];
        return retVal;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function removeTrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`DELETE FROM fov_triggers WHERE ` +
            `trigger_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function removeAllMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`DELETE FROM fov_triggers WHERE ` +
            `monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

module.exports = {
    insertPATrigger,
    getAllPATriggers,
    getPATriggersByMonitor,
    getPATrigger,
    insertPAAlert,
    getAlerts,
    removeTrigger,
    removeAllMonitor
};

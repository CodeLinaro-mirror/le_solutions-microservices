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
    database: config.default_db
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

        const res = await conn.query(`INSERT INTO people_analytics_triggers ` +
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
        const res = await conn.query(`SELECT * from people_analytics_triggers;`);
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

        const res = await conn.query(`SELECT * from people_analytics_triggers where monitor_id = '${data}';`);
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

        const res = await conn.query(`SELECT * from people_analytics_triggers where trigger_id = '${data}';`);
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
        const res = await conn.query(`INSERT INTO people_analytics_alerts ` +
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
        let rows = await conn.query(`select fa.source_trigger_id, UNIX_TIMESTAMP(fa.time) as time, fa.causes, ft.* FROM people_analytics_alerts fa` +
            ` inner join people_analytics_triggers ft on fa.source_trigger_id = ft.trigger_id ` +
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

        const res = await conn.query(`DELETE FROM people_analytics_triggers WHERE ` +
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

        const res = await conn.query(`DELETE FROM people_analytics_triggers WHERE ` +
            `monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function initializeCheckTables() {
    let conn;
    try {
        let res = [];
        res.push(await createDatabase());
        res.push(await createTriggersTable());
        res.push(await createAlertsTable());
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

async function createTriggersTable() {
    let conn;
    try {
        conn = await fetchConn();

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS people_analytics_triggers ` +
            `(id INT AUTO_INCREMENT PRIMARY KEY, monitor_id text, ` +
            `trigger_id VARCHAR(255) unique not null, trigger_name text, ` +
            `trigger_condition text, params text);`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function createAlertsTable() {
    let conn;
    try {
        conn = await fetchConn();

        const res = await conn.query('CREATE TABLE IF NOT EXISTS people_analytics_alerts ' +
            '(source_trigger_id VARCHAR(255) not null, ' +
            'time datetime(6), causes text, ' +
            'CONSTRAINT `pa_trigger_alerts_fk` ' +
            'FOREIGN KEY (source_trigger_id) REFERENCES people_analytics_triggers (trigger_id) ' +
            'on delete cascade ' +
            'on update RESTRICT );');
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
    removeAllMonitor,
    initializeCheckTables
};

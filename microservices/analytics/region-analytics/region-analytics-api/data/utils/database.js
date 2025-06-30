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

async function insertRARegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        let coordinatesString = JSON.stringify(data.coordinates);

        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO region_analytics_regions ` +
            `(monitor_id, region_id, region_name, coordinates)` +
            ` value ('${data.monitor_id}', '${data.region_id}', '${data.region_name}', ` +
            `'${coordinatesString}');`);
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

async function insertRATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        let paramsString = JSON.stringify(data.params);
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO region_analytics_triggers ` +
            `(region_id, trigger_id, trigger_name, trigger_condition, params)` +
            ` value ('${data.region_id}', '${data.trigger_id}', '${data.trigger_name}',` + 
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

async function getAllRARegions() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from region_analytics_regions;`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAllRATriggers() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from region_analytics_triggers;`);
        let triggersRet = [];
        for await (let T of res) {
            if (T.params != null && T.params != undefined && T.params != 'undefined') {
                T.params = JSON.parse(T.params);
            } else {
                delete T['params'];
            }
            triggersRet.push(T);
        }
        return triggersRet;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getRARegionsByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from region_analytics_regions where monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getRATriggersByRegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from region_analytics_triggers where region_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getRATriggersByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);
        const res = await conn.query(`select rt.region_id, rt.trigger_id, rt.trigger_name, `+
                `rt.trigger_condition, rt.params FROM region_analytics_triggers rt` +
                ` inner join region_analytics_regions rr on rt.region_id = rr.region_id ` +
                ` WHERE rr.monitor_id = '${data}';`);
        let triggersRet = [];
        for await (let T of res) {
            if (T.params != null && T.params != undefined && T.params != 'undefined') {
                T.params = JSON.parse(T.params);
            } else {
                delete T['params'];
            }
            triggersRet.push(T);
        }
        return triggersRet;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getRARegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from region_analytics_regions where region_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getRATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from region_analytics_triggers where trigger_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertRAAlert(jsonData) {
    let conn;
    try {
        let data = JSON.parse(jsonData);
        let causesString = JSON.stringify(data.occupants);
        // Escape apostrophes
        data = cleanInput(data);

        conn = await fetchConn();
        const res = await conn.query(`INSERT INTO region_analytics_alerts ` +
            `(monitor_id, source_trigger_id, time, occupants)` +
            ` value ('${data.monitor_id}', '${data.source_trigger.trigger_id}', `+
            `FROM_UNIXTIME(${data.time}), '${causesString}');`);
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
        for await (const id of data.monitorIds) {
            let rows = await conn.query(`select ra.monitor_id, ra.source_trigger_id, UNIX_TIMESTAMP(ra.time) as time, ra.occupants, rt.* FROM region_analytics_alerts ra` +
                ` inner join region_analytics_triggers rt on ra.source_trigger_id = rt.trigger_id ` +
                ` WHERE ra.monitor_id = '${id}' AND ` +
                ` time >= FROM_UNIXTIME(${data.fromTime}) AND time <= FROM_UNIXTIME(${data.toTime}) ORDER BY time;`);
            retVal = [...retVal, ...rows];
        };
        return retVal;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function removeRegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`DELETE FROM region_analytics_regions WHERE ` +
            `region_id = '${data}';`);
        return res;
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
        
        const res = await conn.query(`DELETE FROM region_analytics_triggers WHERE ` +
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

        const res = await conn.query(`DELETE FROM region_analytics_regions WHERE ` +
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
        res.push(await createRegionsTable());
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

async function createRegionsTable() {
    let conn;
    try {
        conn = await fetchConn();

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS region_analytics_regions ` +
            `(monitor_id text, region_id VARCHAR(255) unique not null, ` +
            `region_name text, coordinates text);`);
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

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS region_analytics_triggers ` +
            `(region_id VARCHAR(255) not null, ` +
            `trigger_id VARCHAR(255) unique not null, trigger_name text, ` +
            `trigger_condition text, params text, ` +
            `FOREIGN KEY (region_id) REFERENCES region_analytics_regions (region_id) ` +
            `on delete cascade ` +
            `on update restrict);`);
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

        const res = await conn.query('CREATE TABLE IF NOT EXISTS region_analytics_alerts ' +
            '(monitor_id text, source_trigger_id VARCHAR(255) not null, ' +
            'time datetime(6), occupants text, ' +
            'CONSTRAINT `region_analytics_trigger_alerts_fk` ' +
            'FOREIGN KEY (source_trigger_id) REFERENCES region_analytics_triggers (trigger_id) ' +
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
    insertRARegion,
    insertRATrigger,
    getAllRARegions,
    getAllRATriggers,
    getRARegionsByMonitor,
    getRATriggersByMonitor,
    getRATriggersByRegion,
    getRARegion,
    getRATrigger,
    insertRAAlert,
    getAlerts,
    removeRegion,
    removeTrigger,
    removeAllMonitor,
    initializeCheckTables
};

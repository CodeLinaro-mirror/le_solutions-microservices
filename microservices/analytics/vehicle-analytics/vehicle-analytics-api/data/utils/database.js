/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
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

async function insertVARegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        let coordinatesString = JSON.stringify(data.coordinates);

        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO vehicle_analytics_regions ` +
            `(monitor_id, region_id, region_name, coordinates)` +
            ` value ('${data.monitor_id}', '${data.region_id}', '${data.region_name}', ` +
            `'${coordinatesString}');`);
        console.log(res);
    } catch (e) {
        if (e.code && e.code === 'ER_DUP_ENTRY') {
            throw new Error(`Error: Duplicate entry already exists for the given region_id ${data.region_id}`, e.message);
        } else {
            console.error(e);
        }
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertVATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        let paramsString = JSON.stringify(data.params);
        // Escape apostrophes
        data = cleanInput(data);

        let res;
        // Replace region with null
        if (data.region_id == null || data.region_id == undefined || data.region_id == '') {
            res = await conn.query(`INSERT INTO vehicle_analytics_triggers ` +
            `(monitor_id, region_id, trigger_id, trigger_name, trigger_condition, params)` +
            ` value ('${data.monitor_id}', ${null}, '${data.trigger_id}', '${data.trigger_name}',` +
            `'${data.trigger_condition}', '${paramsString}');`);
        } else {
            res = await conn.query(`INSERT INTO vehicle_analytics_triggers ` +
            `(monitor_id, region_id, trigger_id, trigger_name, trigger_condition, params)` +
            ` value ('${data.monitor_id}', '${data.region_id}', '${data.trigger_id}', '${data.trigger_name}',` +
            `'${data.trigger_condition}', '${paramsString}');`);
        }
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

async function getAllVARegions() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from vehicle_analytics_regions;`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAllVATriggers() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from vehicle_analytics_triggers;`);
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

async function getVARegionsByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from vehicle_analytics_regions where monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getVATriggersByRegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);
        // TODO: Make sure this doesn't also return triggers that are null
        const res = await conn.query(`SELECT * from vehicle_analytics_triggers where region_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getVATriggersByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);
        const res = await conn.query(`SELECT * from vehicle_analytics_triggers where monitor_id = '${data}';`);

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

async function getVARegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from vehicle_analytics_regions where region_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getVATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from vehicle_analytics_triggers where trigger_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertVAAlert(data) {
    let conn;
    try {
        let causesString = JSON.stringify(data.vehicles);

        // Escape apostrophes
        data = cleanInput(data);

        conn = await fetchConn();
        const res = await conn.query(`INSERT INTO vehicle_analytics_alerts ` +
            `(monitor_id, source_trigger_id, alert_id, time, end_time, type, vehicles)` +
            ` value ('${data.source_trigger.monitor_id}', '${data.source_trigger.trigger_id}',` +
            ` '${data.alert_id}', FROM_UNIXTIME(${data.time}),` +
            ` FROM_UNIXTIME(${data.end_time}), '${data.type}', '${causesString}');`);
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
            let rows = await conn.query(
                `select va.source_trigger_id, UNIX_TIMESTAMP(va.time) as time, UNIX_TIMESTAMP(va.end_time) as end_time,` +
                ` va.alert_id, va.type, va.vehicles, vt.* FROM vehicle_analytics_alerts va` +
                ` inner join vehicle_analytics_triggers vt on va.source_trigger_id = vt.trigger_id ` +
                ` WHERE va.monitor_id = '${id}' AND ` +
                ` (time >= FROM_UNIXTIME(${data.fromTime}) AND time <= FROM_UNIXTIME(${data.toTime}))` +
                ` OR (end_time >= FROM_UNIXTIME(${data.fromTime}) AND end_time <= FROM_UNIXTIME(${data.toTime})) ORDER BY time;`);

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

async function getAlertsByRegion(data) {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(
            `select va.source_trigger_id, UNIX_TIMESTAMP(va.time) as time, UNIX_TIMESTAMP(va.end_time) as end_time,` +
            ` va.alert_id, va.type, va.vehicles, vt.* FROM vehicle_analytics_alerts va` +
            ` inner join vehicle_analytics_triggers vt on va.source_trigger_id = vt.trigger_id ` +
            ` WHERE vt.region_id = '${data.regionId}' AND ` +
            ` (time >= FROM_UNIXTIME(${data.fromTime}) AND time <= FROM_UNIXTIME(${data.toTime}))` +
            ` OR (end_time >= FROM_UNIXTIME(${data.fromTime}) AND end_time <= FROM_UNIXTIME(${data.toTime})) ORDER BY time;`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAlertById(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from vehicle_analytics_alerts where alert_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function updateVAAlert(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);
        const res = await conn.query(`UPDATE vehicle_analytics_alerts SET end_time = FROM_UNIXTIME(${data.end_time}) where alert_id = '${data.alert_id}';`);
        return res;
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

        const res = await conn.query(`DELETE FROM vehicle_analytics_regions WHERE ` +
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
        
        const res = await conn.query(`DELETE FROM vehicle_analytics_triggers WHERE ` +
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

        const res = await conn.query(`DELETE FROM vehicle_analytics_regions WHERE ` +
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

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS vehicle_analytics_regions ` +
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

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS vehicle_analytics_triggers ` +
            `(monitor_id text, region_id VARCHAR(255), ` +
            `trigger_id VARCHAR(255) unique not null, trigger_name text, ` +
            `trigger_condition text, params text, ` +
            `FOREIGN KEY (region_id) REFERENCES vehicle_analytics_regions (region_id) ` +
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

        const res = await conn.query('CREATE TABLE IF NOT EXISTS vehicle_analytics_alerts ' +
            '(monitor_id text, source_trigger_id VARCHAR(255) not null, ' +
            'alert_id text not null, ' +
            'time datetime(6), end_time datetime(6), type text, vehicles text, ' +
            'CONSTRAINT `vehicle_analytics_trigger_alerts_fk` ' +
            'FOREIGN KEY (source_trigger_id) REFERENCES vehicle_analytics_triggers (trigger_id) ' +
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
    insertVARegion,
    insertVATrigger,
    getAllVARegions,
    getAllVATriggers,
    getVARegionsByMonitor,
    getVATriggersByMonitor,
    getVATriggersByRegion,
    getVARegion,
    getVATrigger,
    insertVAAlert,
    getAlerts,
    getAlertsByRegion,
    getAlertById,
    updateVAAlert,
    removeRegion,
    removeTrigger,
    removeAllMonitor,
    initializeCheckTables
};

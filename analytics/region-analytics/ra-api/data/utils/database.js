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

        const res = await conn.query(`INSERT INTO ra_regions ` +
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

        const res = await conn.query(`INSERT INTO ra_triggers ` +
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
        const res = await conn.query(`SELECT * from ra_regions;`);
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
        const res = await conn.query(`SELECT * from ra_triggers;`);
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

        const res = await conn.query(`SELECT * from ra_regions where monitor_id = '${data}';`);
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

        const res = await conn.query(`SELECT * from ra_triggers where region_id = '${data}';`);
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
                `rt.trigger_condition, rt.params FROM ra_triggers rt` +
                ` inner join ra_regions rr on rt.region_id = rr.region_id ` +
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

        const res = await conn.query(`SELECT * from ra_regions where region_id = '${data}';`);
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

        const res = await conn.query(`SELECT * from ra_triggers where trigger_id = '${data}';`);
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
        const res = await conn.query(`INSERT INTO ra_alerts ` +
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
            let rows = await conn.query(`select ra.monitor_id, ra.source_trigger_id, UNIX_TIMESTAMP(ra.time) as time, ra.occupants, rt.* FROM ra_alerts ra` +
                ` inner join ra_triggers rt on ra.source_trigger_id = rt.trigger_id ` +
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

        const res = await conn.query(`DELETE FROM ra_regions WHERE ` +
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
        
        const res = await conn.query(`DELETE FROM ra_triggers WHERE ` +
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

        const res = await conn.query(`DELETE FROM ra_regions WHERE ` +
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
    removeAllMonitor
};

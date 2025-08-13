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

async function insertTATripwire(data) {
    let conn;
    try {
        conn = await fetchConn();
        let wireString = JSON.stringify(data.wire);
        let directionString = JSON.stringify(data.direction);

        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO tripwire_analytics_tripwires ` +
            `(monitor_id, tripwire_id, tripwire_name, wire, direction)` +
            ` value ('${data.monitor_id}', '${data.tripwire_id}', '${data.tripwire_name}', ` +
            `'${wireString}', '${directionString}');`);
        console.log(res);
    } catch (e) {
        if (e.code && e.code === 'ER_DUP_ENTRY') {
            throw new Error(`Error: Duplicate entry already exists for the given tripwire_id ${data.tripwire_id}`, e.message);
        } else {
            console.error(e);
        }
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertTATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        let paramsString = JSON.stringify(data.params);
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`INSERT INTO tripwire_analytics_triggers ` +
            `(tripwire_id, trigger_id, trigger_name, trigger_direction, trigger_condition, params)` +
            ` value ('${data.tripwire_id}', '${data.trigger_id}', '${data.trigger_name}',` + 
            ` '${data.trigger_direction}', '${data.trigger_condition}', '${paramsString}');`);
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

async function getAllTATripwires() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from tripwire_analytics_tripwires;`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getAllTATriggers() {
    let conn;
    try {
        conn = await fetchConn();
        const res = await conn.query(`SELECT * from tripwire_analytics_triggers;`);
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

async function getTATripwiresByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from tripwire_analytics_tripwires where monitor_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getTATriggersByTripwire(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from tripwire_analytics_triggers where tripwire_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getTATriggersByMonitor(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);
        const res = await conn.query(`select tt.tripwire_id, tt.trigger_id, tt.trigger_name, `+
                `tt.trigger_direction, tt.trigger_condition, tt.params FROM tripwire_analytics_triggers tt` +
                ` inner join tripwire_analytics_tripwires rr on tt.tripwire_id = rr.tripwire_id ` +
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

async function getTATripwire(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from tripwire_analytics_tripwires where tripwire_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function getTATrigger(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`SELECT * from tripwire_analytics_triggers where trigger_id = '${data}';`);
        return res;
    } catch (e) {
        throw e;
    } finally {
        // Close connection if it was still open
        if (conn) conn.end();
    }
}

async function insertTAAlert(jsonData) {
    let conn;
    try {
        let data = JSON.parse(jsonData);
        let crossingsString = JSON.stringify(data.crossings);
        // Escape apostrophes
        data = cleanInput(data);

        conn = await fetchConn();
        const res = await conn.query(`INSERT INTO tripwire_analytics_alerts ` +
            `(monitor_id, source_trigger_id, time, crossings)` +
            ` value ('${data.monitor_id}', '${data.source_trigger.trigger_id}', `+
            `FROM_UNIXTIME(${data.time}), '${crossingsString}');`);
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
            let rows = await conn.query(`select ta.monitor_id, ta.source_trigger_id, UNIX_TIMESTAMP(ta.time) as time, ta.crossings, tt.* FROM tripwire_analytics_alerts ta` +
                ` inner join tripwire_analytics_triggers tt on ta.source_trigger_id = tt.trigger_id ` +
                ` WHERE ta.monitor_id = '${id}' AND ` +
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

async function removeTripwire(data) {
    let conn;
    try {
        conn = await fetchConn();
        // Escape apostrophes
        data = cleanInput(data);

        const res = await conn.query(`DELETE FROM tripwire_analytics_tripwires WHERE ` +
            `tripwire_id = '${data}';`);
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
        
        const res = await conn.query(`DELETE FROM tripwire_analytics_triggers WHERE ` +
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

        const res = await conn.query(`DELETE FROM tripwire_analytics_tripwires WHERE ` +
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
        res.push(await createTripwiresTable());
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

async function createTripwiresTable() {
    let conn;
    try {
        conn = await fetchConn();

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS tripwire_analytics_tripwires ` +
            `(monitor_id text, tripwire_id VARCHAR(255) unique not null, ` +
            `tripwire_name text, wire text, direction text);`);
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

        const res = await conn.query(`CREATE TABLE IF NOT EXISTS tripwire_analytics_triggers ` +
            `(tripwire_id VARCHAR(255) not null, ` +
            `trigger_id VARCHAR(255) unique not null, trigger_name text, ` +
            `trigger_direction text, trigger_condition text, params text, ` +
            `FOREIGN KEY (tripwire_id) REFERENCES tripwire_analytics_tripwires (tripwire_id) ` +
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

        const res = await conn.query('CREATE TABLE IF NOT EXISTS tripwire_analytics_alerts ' +
            '(monitor_id text, source_trigger_id VARCHAR(255) not null, ' +
            'time datetime(6), crossings text, ' +
            'CONSTRAINT `tripwire_analytics_trigger_alerts_fk` ' +
            'FOREIGN KEY (source_trigger_id) REFERENCES tripwire_analytics_triggers (trigger_id) ' +
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
    insertTATripwire,
    insertTATrigger,
    getAllTATripwires,
    getAllTATriggers,
    getTATripwiresByMonitor,
    getTATriggersByMonitor,
    getTATriggersByTripwire,
    getTATripwire,
    getTATrigger,
    insertTAAlert,
    getAlerts,
    removeTripwire,
    removeTrigger,
    removeAllMonitor,
    initializeCheckTables
};

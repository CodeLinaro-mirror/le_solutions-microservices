-- Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
-- SPDX-License-Identifier: BSD-3-Clause-Clear
CREATE DATABASE IF NOT EXISTS fov;
USE fov;
create table IF NOT EXISTS fov_triggers 
    (id INT AUTO_INCREMENT PRIMARY KEY, monitor_id text, 
    trigger_id VARCHAR(255) unique not null, trigger_name text,
    trigger_condition text, params text);

CREATE TABLE IF NOT EXISTS fov_alerts (
    source_trigger_id VARCHAR(255) not null,
    time datetime(6), causes text,
    CONSTRAINT `pa_trigger_alerts_fk`
        FOREIGN KEY (source_trigger_id) REFERENCES fov_triggers (trigger_id)
        on delete cascade
        on update RESTRICT );

CREATE TABLE IF NOT EXISTS ra_regions
    (monitor_id text, region_id VARCHAR(255) unique not null,
    region_name text, coordinates text);

CREATE TABLE IF NOT EXISTS ra_triggers
    (region_id VARCHAR(255) not null,
    trigger_id VARCHAR(255) unique not null,
    trigger_name text, trigger_condition text,
    params text,
    FOREIGN KEY (region_id) REFERENCES ra_regions (region_id)
    on delete cascade
    on update restrict);

CREATE TABLE IF NOT EXISTS ra_alerts (
    monitor_id text, source_trigger_id VARCHAR(255) not null,
    time datetime(6), occupants text,
    CONSTRAINT `ra_trigger_alerts_fk`
        FOREIGN KEY (source_trigger_id) REFERENCES ra_triggers (trigger_id)
        on delete cascade
        on update restrict );

INSERT IGNORE INTO ra_regions
    (monitor_id, region_id, region_name,
    coordinates) VALUES
    ('0', 'EX_REGION',
    'Left half of the monitor',
    '[{"x": 0,"y": 0},{"x": 0.5,"y": 0},{"x": 0.5,"y": 1},{"x": 0, "y":1}]');

INSERT IGNORE INTO ra_triggers
    (region_id, trigger_id, trigger_name,
    trigger_condition) VALUES
    ('EX_REGION', 'EX_TRIGGER', 'Alert when someone enters or leaves left half of screen',
    'occupancy_changed');

INSERT IGNORE INTO ra_alerts
    (monitor_id, source_trigger_id, time,
    occupants) VALUES
    ('0', 'EXAMPLE_TRIGGER', FROM_UNIXTIME(0.000001),
    '[{"top_left": {"x": 0.2,"y": 0.3},"bottom_right": {"x": 0.4,"y": 0.5}}]'),
    ('0', 'EXAMPLE_TRIGGER', FROM_UNIXTIME(1.000202),
    '[{"top_left": {"x": 0.2,"y": 0.3},"bottom_right": {"x": 0.5,"y": 0.6}}]');

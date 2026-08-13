# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import signal
#from tkinter import CURRENT
from typing import Dict,Deque, Optional, Any, Tuple, List
from mysql.connector.constants import _obsolete_option
import redis.asyncio as redis
import asyncio
import os
import json
from types import SimpleNamespace
import logging
import time
from collections import defaultdict, deque, Counter
from region_algo import RegionHistory, Occupant
import mysql.connector
import numpy as np
import uuid
from datetime import datetime, timezone
from dataclasses import dataclass, field
import sys

# For Debug
#REDIS_HOST = os.environ.get('REDIS_HOST', 'localhost')
#MARIADB_PASSWORD = os.environ.get('MARIADB_PASSWORD', 'secretpw')
#MARIADB_HOST = os.environ.get('MARIADB_HOST', 'localhost')
#MARIADB_USER = os.environ.get('MARIADB_USER', 'root')
#logging.basicConfig(level=logging.INFO, stream=sys.stdout)
#logger = logging.getLogger()

# for production
REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
MARIADB_PASSWORD = os.environ.get('MARIADB_PASSWORD')
MARIADB_HOST = os.environ.get('MARIADB_HOST')
MARIADB_USER = os.environ.get('MARIADB_USER')
logger = logging.getLogger(__file__)

REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'region-analytics.alerts')
REGION_KEY = os.environ.get('REGION_KEY', 'RARegions')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'RATriggers')
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.rz') + ':'
# e.g., monitor 0 would be "detection.rz:0"
ANALYTICS_CHANNEL = os.environ.get('ANALYTICS_CHANNEL', 'region-analytics.analytics')

MARIADB_PORT = os.environ.get('MARIADB_PORT', 3306)
MARIADB_DB = os.environ.get('MARIADB_DB', 'iot_solutions')
COUNT_TABLE = os.environ.get('DEFAULT_COUNT_TABLE', 'count_ra_statistics')
STATISTICS_COMMIT_INTERVAL = os.environ.get('STATISTICS_COMMIT_INTERVAL', 60.0) # 60 seconds

TIME_DRIFT_LIMIT_SECS = 3.0 # TODO: pull from environment within loop
seconds_offsets_by_channel : dict[str, float]= {}

# meanings of elements in the object_detection "rectangle" member
RECT_IDX_TOP = 0
RECT_IDX_LEFT = 1
RECT_IDX_BOTTOM = 2
RECT_IDX_RIGHT = 3

message_list = [] # list of (loop.time(), message)
evt_stop = asyncio.Event()

region_histories : Dict[str, RegionHistory] = {}
previous_regions = {}
# last_trigger_occupancy[trigger_id] = { trigger_condition, (timestamp, count, alert_id) }
last_trigger_occupancy: Dict[str, Dict[str, Tuple[float, int, str]]] = {}
# last_trigger_loitering[trigger_id] = ( {occupant_id, Occupant}, timestamp, alert_id )
last_trigger_loitering: Dict[str, Tuple[Dict[str, Occupant], float, str]] = {}

# Class to store count stats
@dataclass
class CountStats:
    count_min: int = field(default_factory=lambda: float('inf'))
    count_max: int = field(default_factory=lambda: float('-inf'))
    count_current: int = 0
    count_avg: float = 0.0
    count_avg_count: int = 0
    count_avg_sum: int = 0

    def __post_init__(self):
        # Ensure consistency if initialized with values
        if self.count_avg_count > 0:
            self.count_avg = self.count_avg_sum / self.count_avg_count
        else:
            self.count_avg = 0.0

    def update(self, new_count: int):
        self.count_current = new_count

        self.count_min = min(self.count_min, new_count)
        self.count_max = max(self.count_max, new_count)

        self.count_avg_sum += new_count
        self.count_avg_count += 1
        self.count_avg = 0 if self.count_avg_count == 0 else self.count_avg_sum / self.count_avg_count

    def reset(self, full: bool = True):
        self.count_min = float('inf')
        self.count_max = float('-inf')
        self.count_current = 0

        if full:
            self.count_avg = 0.0
            self.count_avg_count = 0
            self.count_avg_sum = 0

# Store count statistics per region
count_statistics: Dict[str, CountStats] = {}

# For concurrency
count_lock = asyncio.Lock()

# Statistics are processed at fixed intervals.
# Statistics are only saved periodically.
last_statistics_commit = 0

# MariaDB connector
db_connection = None

def alert_period_has_elapsed(now):
    global message_list
    if len(message_list) == 0:
        return False # no messages
    oldest_message_time, _ = message_list[0]
    return (now - oldest_message_time) >= ALERT_PERIOD


async def connect_to_redis(r):
    logger.info(f'Pinging redis at {REDIS_HOST}:{REDIS_PORT}..')
    ping_retval = await r.ping()
    logger.info(f'Ping: {ping_retval}')


async def connect_to_db():
    global db_connection
    if db_connection:
        return db_connection

    def check_database_exists(cursor, database_name):
        cursor.execute("SHOW DATABASES LIKE %s", (database_name,))
        return cursor.fetchone() is not None

    def create_database_if_not_exists(cursor, database_name):
        if not check_database_exists(cursor, database_name):
            cursor.execute(f"CREATE DATABASE {database_name}")
            logger.info(f"Database '{database_name}' created.")
        else:
            logger.info(f"Database '{database_name}' already exists.")

    db_connection = None

    while True:
        try:
            logger.info(f"Connecting to MariaDB..{MARIADB_HOST}.{MARIADB_PORT}.")

            logger.info(f'Checking if microservice db exists...')
            db_connection = mysql.connector.connect(
                user=MARIADB_USER,
                password=MARIADB_PASSWORD,
                host=MARIADB_HOST,
                port=MARIADB_PORT,
                collation='utf8mb4_general_ci'
            )

            if db_connection is None or not db_connection.is_connected():
               logger.error(f'Error connecting to MariaDB!')
               db_connection = None
               return db_connection

            cursor = db_connection.cursor()
            create_database_if_not_exists(cursor, MARIADB_DB)
            cursor.close()
            db_connection.close()

            db_connection = mysql.connector.connect(
                user=MARIADB_USER,
                password=MARIADB_PASSWORD,
                host=MARIADB_HOST,
                port=MARIADB_PORT,
                database=MARIADB_DB,
                collation='utf8mb4_general_ci'
            )

            if db_connection is None or not db_connection.is_connected():
                handle_no_db_error()
                return None

            logger.info(f"Connected to MariaDB successfully!")

            cursor = db_connection.cursor()

            logger.info(f'Create tables...')

            cursor.execute(f"""
                 CREATE TABLE IF NOT EXISTS {COUNT_TABLE} (
                 timestamp DATETIME NOT NULL,
                 region_id VARCHAR(255) NOT NULL,
                 min_count INT,
                 max_count INT,
                 avg_count FLOAT
                 )
                """)

            # Add an index for faster query
            cursor.execute(f"""
                CREATE INDEX IF NOT EXISTS idx_timestamp_region
                ON {COUNT_TABLE}(timestamp, region_id)
                """)

            cursor.close()
            return db_connection

        except Exception as e:
            logger.error(f"Exception connecting to MariaDB: {e}")
            logger.info("Retrying in 5 seconds...")
            await asyncio.sleep(5)

    return db_connection

async def get_regions(r):
    # Get regions from Redis
    raw_regions = await r.hgetall(REGION_KEY)
    logger.debug(f'Raw regions from redis: {raw_regions}')

    # redis HGETALL returns in format {k : v} where k = region_id, v = full region in JSON string
    # Decode region JSON info
    regions = {id: json.loads(region_str) for id, region_str in raw_regions.items()}

    logger.debug(f'Parsed & validated regions: {regions}')
    return regions


async def get_triggers(r):
    # Get triggers from Redis
    raw_triggers = await r.hgetall(TRIGGER_KEY)
    logger.debug(f'Raw triggers from redis: {raw_triggers}')

    # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
    # Convert to list of triggers
    triggers = [json.loads(trigger) for trigger in raw_triggers.values()]

    # TODO: add occupancy_over, occupancy_under
    supported_trigger_conditions = ['occupancy_changed', 'occupancy_over', 'occupancy_under', 'loitering_over']
    triggers = [t for t in triggers if t['trigger_condition'] in supported_trigger_conditions]

    # TODO: remove below code when Redis payload has json-decoded inner objects
    # JSON-decode any inner objects
    for trigger in triggers:
        if 'param' in trigger:
            trigger['params'] = json.loads(trigger['params'])

    logger.debug(f'Parsed & validated triggers: {triggers}')
    return triggers


def convert_msg_timestamp_to_epoch_time(msg_ts, sys_time, channel=None):
    return sys_time

    # channel = None only used in some tests
    global seconds_offsets_by_channel
    msg_time = float(msg_ts) / 1e9 # ns to s

    # All units are seconds
    offset = seconds_offsets_by_channel.get(channel, 0.0)
    if abs(msg_time + offset - sys_time) > TIME_DRIFT_LIMIT_SECS:
        new_offset = sys_time - msg_time
        time_fmt = '.3f'
        log_str = (
            f'Clock drift past limit, channel: {channel}, '
            f'old offset: {offset:{time_fmt}}s, '
            f'adjusted message time: {msg_time + offset:{time_fmt}}s, '
            f'sys time: {sys_time:{time_fmt}}s, '
            f'delta: {msg_time + offset - sys_time:{time_fmt}}s, '
            f'new offset: {new_offset:{time_fmt}}s'
        )

        logger.info(log_str)
        offset = sys_time - msg_time
        seconds_offsets_by_channel[channel] = offset
    return msg_time + offset

async def save_count_statistics():
    '''
    Save for all region_ids. Counts are reset after saving.
    '''

    if not count_statistics:
        return

    #logger.debug(f'save_count_statistics {datetime.now()}')

    async with count_lock:
        for region_id, stats in count_statistics.items():

            timestamp = datetime.now(timezone.utc)

            logger.debug(f"Save {COUNT_TABLE}[{region_id}] {timestamp}")

            try:
                conn = await connect_to_db()

                cursor = conn.cursor()

                data_insert = f'''
                 INSERT INTO {COUNT_TABLE} (timestamp, region_id, min_count, max_count, avg_count)
                     VALUES (%s, %s, %s, %s, %s)
                 '''

                #logger.debug(f'{data_insert}')

                # Insert the data
                vmin = 0 if stats.count_min == float('inf') else stats.count_min
                vmax = 0 if stats.count_max == float('-inf') else stats.count_max
                #logger.debug(f'{vmin} {vmax} {stats.count_avg}')
                cursor.execute(data_insert, (timestamp, region_id, vmin, vmax, stats.count_avg))

                # Save and close
                conn.commit()
                cursor.close()

                logger.debug(f"Successfully saved count data")

                # Reset once saved
                stats.reset()

            except Exception as e:
                logger.error(f"An error occurred: {e}")
                return False

    return True # success


async def run_count_query(r : redis.Redis, token, region_id, start_time, end_time):
    '''
    Process a count analystics request.
    '''

    logger.info(f'run_count_query {datetime.fromtimestamp(start_time)} -> {datetime.fromtimestamp(end_time)}')

    try:
        # Save the current count statistics in case it needs to be included
        await save_count_statistics()

        conn = await connect_to_db()

        cursor = conn.cursor()

        # Query the database for entries within the timestamp range.
        # The max_count is the max count across all returned rows,
        # the min_count is the min_count acrosss all returned row,
        # and the average_count is the average of all the averages across returned rows.
        query = f'''
            SELECT
                MAX(max_count) AS max_of_max_counts,
                MIN(min_count) AS min_of_min_counts,
                AVG(avg_count) AS avg_of_avg_counts
            FROM {COUNT_TABLE}
            WHERE region_id = %s
                AND timestamp BETWEEN %s AND %s
            '''

        cursor.execute(query, (region_id, datetime.fromtimestamp(start_time, timezone.utc), datetime.fromtimestamp(end_time, timezone.utc)))

        # Get and post results
        result = cursor.fetchone()

        if len(result) == 0:
            logger.info(f'No results found.')
            max_of_max_counts = 0
            min_of_min_counts = 0
            avg_of_avg_counts = 0.0
        else:
            max_of_max_counts = float(0 if not result[0] else result[0])
            min_of_min_counts = float(0 if not result[1] else result[1])
            avg_of_avg_counts = float(0 if not result[2] else result[2])

        cursor.close()


        # Post results
        #{
        #   'min': 0,
        #   'max': 0,
        #   'average': 0y
        #}
        result_json = {
                'min': min_of_min_counts,
                'max': max_of_max_counts,
                'average': avg_of_avg_counts
            }

        result = {
                'sync_id': token,
                'result': result_json
            }

        logger.debug(f'Publishing to channel {ANALYTICS_CHANNEL}: {result}')
        asyncio.ensure_future(r.publish(ANALYTICS_CHANNEL, json.dumps(result)))

    except Exception as e:
        logger.error(f"An error occurred: {e}")

async def statistics_task():
    logger.info(f'Statistics Task Started...')
    while True:
        now = time.time()
        if statistics_commit_interval_elapsed(now):
            await save_count_statistics()
            last_statistics_commit = time.time()

        await asyncio.sleep(STATISTICS_COMMIT_INTERVAL)

    logger.info(f'Statistics Task Stopped...')

def statistics_commit_interval_elapsed(now):
    return (now - last_statistics_commit) >= STATISTICS_COMMIT_INTERVAL

def parse_messages(messages):
    '''
    Parse incoming messages and group by monitor
    '''

    messages_by_monitor = defaultdict(list) # list of messages by monitor, in received order

    try:
        for recv_time, message in messages:
            # first element is time message arrived, second element is Redis payload
            msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))
            monitor = message['channel'][len(DETECTION_CHANNEL_PREFIX):] # monitor follows prefix
            messages_by_monitor[monitor].append((recv_time, msg_obj))
    except Exception as exc:
        logger.error(f'Error parsing message: {message}')
        logger.exception(exc)
        return None

    return messages_by_monitor

def group_triggers_by_region(triggers):
    '''
    Group triggers by region_id
    '''
    triggers_by_region = defaultdict(list)

    for trigger in triggers:
        triggers_by_region[trigger['region_id']].append(trigger)

    return triggers_by_region

def format_records(messages, max_lookback):
    '''
    Format message data into algo's format, skipping messages that would pop out of lookback
    '''
    records_by_frame = []

    try:
        for _, message in messages[-max_lookback:]:
            if not hasattr(message, 'object_detection'):
                continue # no people in message
            frame_objects = message.object_detection
            records = {}
            for person in frame_objects:
                if not person.label.startswith('person') or not hasattr(person, 'landmarks'):
                    continue  # not a person or  missing landmarks
                landmarks = person.landmarks
                if not (hasattr(landmarks, 'left_ankle') and hasattr(landmarks, 'right_ankle')):
                    continue # need both ankles
                foot_coords = {
                    'x': (landmarks.left_ankle.x + landmarks.right_ankle.x) / 2.0,
                    'y': (landmarks.left_ankle.y + landmarks.right_ankle.y) / 2.0
                }
                rectangle = person.rectangle
                bounding_box = {
                    'top_left': {
                        'x': rectangle.x,
                        'y': rectangle.y
                    },
                    'bottom_right': {
                        'x': rectangle.x + rectangle.width,
                        'y': rectangle.y + rectangle.height
                    }
                }
                records[person.tracking_id] = {
                    'id': person.tracking_id,
                    'x': foot_coords['x'],
                    'y': foot_coords['y'],
                    'bounding_box': bounding_box
                }
            records_by_frame.append(records)
    except Exception as e:
        logger.exception(f"Exception! {e}")

    return records_by_frame

def make_alert(trigger, region, alert_occupants, alert_id, alert_time, end_time=0):

    if not alert_id:
        alert_id = str(uuid.uuid4())

    alert = {
        'monitor_id': region['monitor_id'],
        'source_trigger': trigger,
        'alert_id': alert_id,
        'time': alert_time,
        'end_time': end_time,
        'occupants': alert_occupants
    }

    logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")

    #logger.debug(f'{alert}')
    return alert

def get_loitering_alert(trigger, region, alert_time, occupants, previous_occupants, threshold):
    '''
    Create the loitering alerts.

    If there are no loitering occupants and a loitering alert exists for the trigger then it should clear.
    The previous alert is updated with an end_time and sent.

    Otherwise, if there are loitering occupants:

    If new person(s) are first detected as loitering, create a new alert with a end_time of 0,
    where the occupant has an enter time (first detected) and exit time of 0.

    If a previously loitering person is no longer detected, create a new alert and set the exit_time
    for the occupant. If there are no loitering occupants left then the alert should have an end_time.

    If there are loitering occupants but no new occupants have been added or removed then do not send a new alert.
    '''
    global last_trigger_loitering

    trigger_id = trigger[f'trigger_id']
    loitering_occupants = []
    occupancy_changed = False

    for occupant in occupants.values():
        dwell_time = alert_time - occupant.first_seen
        occupant.loitering = dwell_time > threshold

        if occupant.loitering:
            loitering_occupants.append(occupant)
        else:
            dwell_str = "Entered the region" if dwell_time == 0 else f"{dwell_time:.2f} seconds"
            logger.info(f'   occupant[{occupant.id}]: {dwell_str}')
            occupancy_changed = True

    if previous_occupants:
        for occupant_id in previous_occupants:
            logger.info(f'   occupant [{occupant_id}]: Exited the region')
            occupancy_changed = True

    # There are no loitering occupants and alert exists then it should clear.
    if not loitering_occupants:
        if last_trigger_loitering and trigger_id in last_trigger_loitering:
            alert_occupants = []

            occupants_dict, last_timestamp, alert_id = last_trigger_loitering[trigger_id]

            for occupant_id, occupant in occupants_dict.items():
                #logger.info(f'   occupant [{occupant_id}]: Exited the region')
                alert_occupants.append({
                    'bounding_box': occupant.bounding_box,
                    'enter_time': occupant.first_seen,
                    'exit_time': alert_time,
                    'dwell_time':  alert_time - occupant.first_seen,
                    'person_id': occupant.id
                })

            del last_trigger_loitering[trigger_id] # no more active alert
            logger.info(f"*** LOITERING_OVER {threshold} CLEARED ***")
            #logger.info(alert_occupants)
            return make_alert(trigger_id, region, alert_occupants, alert_id, last_timestamp, alert_time)
        else:
            return None

    still_present = []
    new_occupants = []
    removed_occupants = []
    alert_occupants = []

    # Sort occupants
    current_keys = {o.id for o in loitering_occupants}
    previous_keys = {
        occ_id
        for occs, _, _ in last_trigger_loitering.values()
        for occ_id in occs.keys()
    }

    # Determine which occupants are still present or new
    for occupant in loitering_occupants:
        key = occupant.id

        if key in previous_keys:
            still_present.append(occupant)
        else:
            occupant.alert_time = alert_time
            new_occupants.append(occupant)

        logger.info(f'   occupant[{occupant.id}]: {alert_time-occupant.first_seen:.2f} seconds...is loitering > {threshold} secs')

        if trigger_id not in last_trigger_loitering:
            alert_id = str(uuid.uuid4())
            last_trigger_loitering[trigger_id] = ({ key: occupant }, alert_time, alert_id)
        else:
            occupants_dict, timestamp, alert_id = last_trigger_loitering[trigger_id]
            occupants_dict[key] = occupant
            last_trigger_loitering[trigger_id] = (occupants_dict, alert_time, alert_id)

        # These occupants are still loitering
        alert_occupants.append({
            'bounding_box': occupant.bounding_box,
            'enter_time': occupant.first_seen,
            'exit_time': 0,
            'dwell_time':  alert_time - occupant.first_seen,
            'person_id': occupant.id
        })

    # Determine which occupants were removed.
    # Set an exit time and alert once the remove from the list.
    if not trigger_id in last_trigger_loitering:
        logger.info("NONE EXISTS")

    occupants_dict, timestamp, alert_id = last_trigger_loitering[trigger_id]
    for occ_id, occupant in occupants_dict.items():
        if occ_id not in current_keys:
            alert_occupants.append({
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': alert_time,
                'dwell_time':  alert_time - occupant.first_seen,
                'person_id': occupant.id
            })
            #logger.info(f'   occupant [{occupant.id}]: Exited the region')
            removed_occupants.append(occupant)

    # Remove occupants that have left from the history
    for occupant in removed_occupants:
        occupants_dict.pop(occupant.id, None)

    # There are new or existing occupants so alert started
    if new_occupants or still_present:
        if len(previous_keys) == 0:
            logger.info(f'*** LOITERING_OVER > {threshold} STARTED ***')
            #logger.info(alert_occupants)
            return make_alert(trigger, region, alert_occupants, alert_id, alert_time, 0)
        elif occupancy_changed:
            logger.info(f'*** LOITERING_OVER > {threshold} ACTIVE *** : Occupancy Changed')
            #logger.info(alert_occupants)
            return make_alert(trigger, region, alert_occupants, alert_id, alert_time, 0)
        else:
            logger.info(f'*** LOITERING_OVER > {threshold} ACTIVE *** : NO Occupancy Changed')

    # Set the end time for any removed_occupants.
    # Alert is cleared if there are no more occupants.
    if removed_occupants:
        end_time = 0
        #if not still_present:
        #    end_time = alert_time
        #    logger.info("*** Loitering Alert CLEARED ***")
        #    del last_trigger_loitering[trigger] # no more active alert
        return make_alert(trigger, region, alert_occupants, alert_id, alert_time, end_time)

def process_region_triggers(region, messages, triggers):
    '''
    Process triggers by region
    '''
    global last_trigger_occupancy

    region_id = region['region_id']
    monitor_id = region['monitor_id']

    # Get region history or initialize if needed
    if region_id not in region_histories:
        logging.info(f"New region history for {region_id}")
        region_histories[region_id] = RegionHistory(polygon=region['coordinates'])
        # TODO: delete stale regions histories?

    rh = region_histories.get(region_id)

    if not rh:
        logging.info(f"No region history available for {region_id}")
        return

    records_by_frame = format_records(messages, rh.get_max_lookback())
    if not records_by_frame:
        return []

    # get timestamp of last message
    last_recv_time, last_message = messages[-1]
    alert_time = convert_msg_timestamp_to_epoch_time(
        last_message.parameters.timestamp,
        last_recv_time,
        monitor_id )

     # Filter detections to get occupants within the region
    occupants, removed_occupants = rh.add_detections(records_by_frame, alert_time)
    current_occupancy = len(occupants)

    logger.debug(f"Region {region_id} occupants: {list(occupants.keys())}, frame history buffer: {rh._frame_history}")

    # Update count stats
    if region_id not in count_statistics:
        count_statistics[region_id] = CountStats()

    count_statistics[region_id].update(current_occupancy)

    logger.info(f'[{region_id}]: {current_occupancy} people')

    alerts = []

    # Evaluate triggers for this region
    for trigger in triggers:

        trigger_id = trigger['trigger_id']

        occupancy_alerts = False
        trigger_condition = trigger['trigger_condition']
        loitering_occupants = []
        loitering_alert = None
        alert_id = str(uuid.uuid4())
        trigger_data = last_trigger_occupancy.get(trigger_id, {}).get(trigger_condition, (0.0, 0, alert_id))
        timestamp, last_occupancy, alert_id = trigger_data

        match trigger_condition:
            case 'occupancy_changed':
                occupancy_alerts = last_occupancy != current_occupancy
                alert_log = f'OCCUPANCY_CHANGED {last_occupancy}->{current_occupancy}'
            case 'occupancy_over':
                threshold = int(get_trigger_param(trigger, 'threshold'))
                occupancy_alerts = current_occupancy > threshold
                alert_log = f'OCCUPANCY_OVER > {threshold}'
            case 'occupancy_under':
                threshold = int(get_trigger_param(trigger, 'threshold'))
                occupancy_alerts = current_occupancy < threshold
                alert_log = f'OCCUPANCY_UNDER < {threshold}'
            case 'loitering_over':
                threshold = int(get_trigger_param(trigger, 'threshold')) / 1000.0 # ms to sec
                loitering_alert = get_loitering_alert(trigger, region, alert_time, occupants, removed_occupants, threshold)
            case _:
                raise RuntimeError(f'Unexpected trigger_condition \'{trigger_condition}\' in {trigger}')

        if occupancy_alerts:
            if timestamp == 0.0:
                logger.info(f"*** {alert_log} STARTED ***")
                alerts.append(make_alert(trigger, region, rh.get_occupant_data_for_alert(alert_time, 0), alert_id, alert_time, end_time=0))
            else:
                logger.info(f"*** {alert_log} ACTIVE ***")

            if not trigger_id in last_trigger_occupancy:
                last_trigger_occupancy[trigger_id] = {}

            last_trigger_occupancy[trigger_id][trigger_condition] = (alert_time, current_occupancy, alert_id)


        elif timestamp != 0.0:
            alert_log = f"*** {alert_log} CLEARED ***"
            logging.info(alert_log)
            alerts.append(make_alert(trigger, region, rh.get_occupant_data_for_alert(alert_time, alert_time), alert_id, alert_time, alert_time))
            del last_trigger_occupancy[trigger_id][trigger_condition]

        if loitering_alert:
            alerts.append(loitering_alert)

    logger.debug(f'region={region_id}, monitor={monitor_id} generated {len(alerts)} from {len(triggers)} triggers.')

    return alerts

def apply_triggers(triggers, regions, messages):
    messages_by_monitor = parse_messages(messages)
    triggers_by_region = group_triggers_by_region(triggers)
    all_alerts = []

    # Apply triggers by region
    for region in regions.values():
        region_id = region['region_id']
        monitor_id = region['monitor_id']
        region_messages = messages_by_monitor.get(monitor_id, [])

        if not region_messages:
            continue

        region_triggers = triggers_by_region.get(region_id, [])
        alerts = process_region_triggers(region, region_messages, region_triggers)
        all_alerts.extend(alerts)

    logger.debug(f'Filtered {len(messages)} Detection messages through {len(triggers)} trigger(s), generating {len(all_alerts)} alert(s)')
    return all_alerts

def get_trigger_param(trigger, name):
    for param in trigger['params']:
        if param['name'] == name:
            return param['value']

    raise ValueError(f'{name} not in params for trigger {trigger}')

async def check_regions(current_regions):
    global previous_regions

    current_ids = set(current_regions.keys())
    previous_ids = set(previous_regions.keys())
    deleted_ids = previous_ids - current_ids

    previous_regions = current_regions

    async def delete_region_entries(region_id):
        query = f"DELETE FROM {COUNT_TABLE} WHERE region_id = %s"
        async with db_connection.acquire() as conn:
            async with conn.cursor() as cur:
                await cur.execute(query, (region_id,))

    if deleted_ids:
        logger.info(f"Deleted region IDs: {deleted_ids}")
        for region_id in deleted_ids:
            logger.info(f'Removing {region_id} stats from the database...')
            asyncio.create_task(delete_region_entries(region_id))
            count_statistics.pop(region_id, None)

async def check_for_alerts(r : redis.Redis):
    try:
        global message_list
        if len(message_list) == 0:
            return # no messages

        current_messages = message_list
        message_list = [] # empty the message list

        current_regions = await get_regions(r)
        triggers = await get_triggers(r)
        alerts = apply_triggers(triggers, current_regions, current_messages)

        for alert in alerts:
            logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
            # TODO: add this to task list instead of awaiting?
            await r.publish(ALERT_CHANNEL, json.dumps(alert))

        # Check for any regions that were deleted
        await check_regions(current_regions)

    except Exception as exc:
        logger.error('Exception in check_for_alerts:')
        logger.exception(exc)
        logger.error('Database or received messages likely corrupted, will keep trying..')

async def register_pubsub_listeners(r: redis.Redis):

    def detection_message_handler(message):
        # called by the pubsub async runner when a message is received
        global message_list

        now = time.time()
        if message is not None:
            message_list.append((now, message))
            data = message["data"]
            data_len = len(data)
            # create a readable log with first & last SNIP_SIZE characters of message
            SNIP_SIZE = 20
            data_str = str(data) if data_len < (2*SNIP_SIZE+2) else f'{str(data)[:SNIP_SIZE]}..{str(data)[-SNIP_SIZE:]} ({data_len} bytes)'
            logger.debug(f'Received message on ch "{message["channel"]}" >> "{data_str}"')
        else:
            logger.debug('No message from get_message')
        if alert_period_has_elapsed(now):
            asyncio.ensure_future(check_for_alerts(r))

    def analytics_request_handler(message):
        logger.debug(f'Received message on analytics channel')

        if message and  message['type'] == 'message':

            # {
            #     'sync_id': sync_id,
            #     'region_id': data.regionId,
            #     'from_time': data.fromTime,
            #     'to_time': data.toTime,
            #     'analytics_type': analytics_type, # 'count'
            # }

            try:
                #logger.info(f'Analytics Request: {message}')
                request = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))

                if hasattr(request, 'result'):
                    #logger.debug(f'Ignore own messages')
                    return

                #data = json.loads(message['data'])
                #logger.debug(f"[{ANALYTICS_CHANNEL}] Received JSON: {json.dumps(data, indent=2)}")
                #sync_id = data.get('sync_id')
                #region_id = data.get('region_id')
                #from_time = data.get('from_time')
                #to_time = data.get('to_time')
                #analytics_type = data.get('analytics_type')

                def handle_unknown_analytics(query_type):
                    logger.error(f'Unknown {query_type} analytics type')

                # Handling should be non-blocking
                if( request.analytics_type == 'count'):
                    asyncio.create_task(run_count_query(r, request.sync_id, request.region_id, request.from_time, request.to_time))
                else:
                    handle_unknown_analytics()
            except json.JSONDecodeError:
                print(f"[Subscriber] Received non-JSON message: {message['data'].decode('utf-8')}")
        else:
            logger.debug('No message for analytics')


    # register subscribe pattern handler, return pubsub to be used in caller for .run()
    pubsub = r.pubsub()
    channel_pattern = DETECTION_CHANNEL_PREFIX + '*'
    await pubsub.psubscribe(**{channel_pattern: detection_message_handler})

    await pubsub.subscribe(**{ANALYTICS_CHANNEL: analytics_request_handler})
    logger.debug(f'subscribed to {ANALYTICS_CHANNEL} channel')

    return pubsub

def handle_no_db_error():
       logger.info(f"No MariaDB connection!")
       # should fail like api
       #sys.exit()

async def async_main():
    global db_connection

    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    channel_listener_task = None
    statistics_process_task = None
    pubsub = None

    try:
        await connect_to_redis(r)
        await connect_to_db()

        def quit_handler():
            logging.info('Got shutdown signal, cancelling tasks..')
            if channel_listener_task and not channel_listener_task.done():
                channel_listener_task.cancel()
            if statistics_process_task and not statistics_process_task.done():
                statistics_process_task.cancel()

        pubsub = await register_pubsub_listeners(r)
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, quit_handler)
        channel_listener_task = asyncio.create_task(pubsub.run()) # runs forever until cancelled
        statistics_process_task = asyncio.create_task(statistics_task())

        await asyncio.gather(
            channel_listener_task,
            statistics_process_task,
            return_exceptions=True
        )

    except asyncio.CancelledError:
        logger.info('Got cancelled exception, shutting down..')
    finally:
        logger.info('Cleaning up resources..')
        if pubsub is not None:
            try:
                await pubsub.aclose()
            except Exception as e:
                logger.debug(f'pubsub close: {e}')
        await r.aclose()
        if db_connection:
            db_connection.close()
            db_connection = None
        logger.info('Shutdown complete.')

if __name__ == "__main__":

    # logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(filename)s::%(funcName)s %(message)s')
    logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(message)s')
    asyncio.run(async_main())

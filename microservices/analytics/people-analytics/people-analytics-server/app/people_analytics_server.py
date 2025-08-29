# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from ast import Try
from collections import deque, Counter, defaultdict
from lzma import CHECK_NONE
from re import L
from typing import Dict, Deque, Optional, Any, Tuple
from xmlrpc.client import DateTime
import redis.asyncio as redis
import asyncio
import os
import json
from types import SimpleNamespace
import logging
import time
import signal
import mysql.connector
import numpy as np
import uuid
from datetime import datetime, timezone
#import matplotlib.pyplot as plt
import sys

from dataclasses import dataclass, field


REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
MARIADB_PASSWORD = os.environ.get('MARIADB_PASSWORD')
MARIADB_HOST = os.environ.get('MARIADB_HOST')
MARIADB_USER = os.environ.get('MARIADB_USER')
logger = logging.getLogger(__file__)

REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'people-analytics.alerts')
ANALYTICS_CHANNEL = os.environ.get('ANALYTICS_CHANNEL', 'people-analytics.analytics')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'PATriggers')
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))
LOOKBACK_FRAMES = int(os.environ.get('LOOKBACK_FRAMES', 5))
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.ppe') + ':'
# e.g., monitor 0 channel would be "detection.ppe:0"

MARIADB_PORT = os.environ.get('MARIADB_PORT', 3306)
MARIADB_DB = os.environ.get('MARIADB_DB', 'iot_solutions')
HEATMAP_ROWS = int(os.environ.get('DEFAULT_HEATMAP_ROWS', 64))
HEATMAP_COLS = int(os.environ.get('DEFAULT_HEATMAP_COLS', 64))
HEATMAP_TABLE = os.environ.get('DEFAULT_HEATMAP_TABLE', 'heatmap_pa_statistics')
COUNT_TABLE = os.environ.get('DEFAULT_COUNT_TABLE', 'count_pa_statistics')
HEATMAP_PROCESSING_INTERVAL = os.environ.get('HEATMAP_PROCESSING_INTERVAL', 2.0) # 2 seconds
STATISTICS_COMMIT_INTERVAL = os.environ.get('STATISTICS_COMMIT_INTERVAL', 60.0) # 60 seconds

TIME_DRIFT_LIMIT_SECS = 3.0 # TODO: pull from environment within loop
seconds_offsets_by_channel : dict[str, float]= {}

message_list = [] # list of (loop.time(), message)
evt_stop = asyncio.Event()

frame_history_by_channel : Dict[str, deque] = {}

# Class to store count stats
@dataclass
class CountStats:
    count_min: int = field(default_factory=lambda: float('inf'))
    count_max: int = field(default_factory=lambda: float('-inf'))
    count_current: int = 0
    count_current_frame: Optional[Any] = None
    count_avg: float = 0.0
    count_avg_count: int = 0
    count_avg_sum: int = 0

    def __post_init__(self):
        # Ensure consistency if initialized with values
        if self.count_avg_count > 0:
            self.count_avg = self.count_avg_sum / self.count_avg_count
        else:
            self.count_avg = 0.0

    def update(self, new_count: int, frame: Optional[Any] = None):
        self.count_current = new_count
        self.count_current_frame = frame

        self.count_min = min(self.count_min, new_count)
        self.count_max = max(self.count_max, new_count)

        self.count_avg_sum += new_count
        self.count_avg_count += 1
        self.count_avg = 0 if self.count_avg_count == 0 else self.count_avg_sum / self.count_avg_count

    def reset(self, full: bool = True):
        self.count_min = float('inf')
        self.count_max = float('-inf')
        self.count_current = 0
        self.count_current_frame = None

        if full:
            self.count_avg = 0.0
            self.count_avg_count = 0
            self.count_avg_sum = 0


# Store count statistics per monitor
count_statistics: Dict[str, CountStats] = {}

# For concurrency
count_lock = asyncio.Lock()

# Class to store heatmap stats
@dataclass
class HeatmapStats:
    rows: int = HEATMAP_ROWS
    cols: int = HEATMAP_COLS
    heatmap: list = field(init=False)

    def __post_init__(self):
        # Initialize heatmap to 0s
        self.heatmap = np.zeros((self.rows, self.cols), dtype=int)

    def reset(self):
        self.heatmap =  np.zeros((self.rows, self.cols), dtype=int)

# Store heatmap statistics per monitor
heatmap_statistics: Dict[str, HeatmapStats] = {}

# Statistics are processed at fixed intervals.
# Statistics are only saved periodically.
last_statistics_commit = 0
last_heatmap_processing = 0

# MariaDB connector
db_connection = None


def get_foot_coordinates(person):
    '''
    To calculate position within the grid:
        - Use average position of both ankles if both are available
        - else use either left or right ankle, whichever one is available
        - else if no ankles are detected then use the midpoint of the bottom of the bounding box
        - otherwise not a valid person
    '''

    have_left = False
    have_right = False
    landmarks = None
    bb = None
    foot_coords = {}

    if hasattr(person, 'landmarks'):
        landmarks = person.landmarks
        have_left = hasattr(landmarks, 'left_ankle')
        have_right = hasattr(landmarks, 'right_ankle')

    if hasattr(person, 'rectangle'):
        bb = person.rectangle

    if have_left and have_right:
        # foot_coords (x,y) = midpoint of ankles
        #logger.info('using left and right')
        foot_coords['x'] = (landmarks.left_ankle.x + landmarks.right_ankle.x) / 2.0
        foot_coords['y'] = (landmarks.left_ankle.y + landmarks.right_ankle.y) / 2.0
    elif have_left:
        #logger.info('using left only')
         foot_coords['x'] = landmarks.left_ankle.x
         foot_coords['y'] = landmarks.left_ankle.y
    elif have_right:
         #logger.info('using right only')
         foot_coords['x'] = landmarks.right_ankle.x
         foot_coords['y'] = landmarks.right_ankle.y
    elif bb:
         #logger.info('using bb')
         foot_coords['x'] = (bb.x + bb.width) / 2.0
         foot_coords['y'] = bb.y + bb.height
    else:
         logger.error(f'Person detected without landmarks or bounding box!')
         foot_coords = None

    return foot_coords

@dataclass
class Occupant:
    id: str
    first_seen: float
    x: float
    y: float
    bounding_box: Dict[str, Dict[str, float]]
    last_seen: float = field(init=False)

    def __post_init__(self):
        self.last_seen = self.first_seen

    def update(self, timestamp: float, x: float, y: float, bounding_box: Dict[str, Dict[str, float]]):
        self.last_seen = timestamp
        self.x = x
        self.y = y
        self.bounding_box = bounding_box

    def to_dict(self):
        return {
            'bounding_box': self.bounding_box,
            'enter_time': self.first_seen,
            'exit_time': self.last_seen
        }

class OccupantHistory:
    # Tracks occupants as Occupant instances.
    # Maintains detection history.
    # Updates or removes occupants based on majority vote in frame(s)

    def __init__(self):
        self._current_occupants = {} # key: occupant_id, value: Occupant instance

    def add_detections(self, channel, msg_time, frame):
        """
        Adds detection records to the fov's frame history and updates current occupants.
        Each record is annotated with whether the person is inside the fov.
        Returns a tuple of (new_occupants, removed_occupants).
        """

        if not frame:
            removed_occupants = self._current_occupants or {}
            self._current_occupants = {}
            return self._current_occupants, removed_occupants


        if not hasattr(frame, "object_detection"):
            removed_occupants = self._current_occupants
            self._current_occupants = {}
            return self._current_occupants, removed_occupants

        occupants = []

        # snapshot of previous occupants
        previous_occupants_snapshot = self._current_occupants.copy()
        previous_ids = set(self._current_occupants.keys())
        current_ids = set()

        for person in frame.object_detection:

            if not hasattr(person, 'label'):
                continue # this person has no bounding box

            if not person.label.startswith('person'):
                logger.debug(f'[{channel}]: no people detected!')
                continue # obj is not a person, skip

             # process the person
            if not hasattr(person, 'rectangle'):
                continue # this person has no bounding box

            person_id = str(person.tracking_id)
            current_ids.add(person_id)

            rectangle = person.rectangle
            bb = {
                 'top_left': {
                     'x': rectangle.x,
                     'y': rectangle.y
                 },
                 'bottom_right': {
                     'x': rectangle.x + rectangle.width,
                     'y': rectangle.y + rectangle.height
                 }
            }

            foot_coords = get_foot_coordinates(person)

            if  person_id not in self._current_occupants:
                self._current_occupants[person_id] = Occupant(
                    id=person_id,
                    first_seen=msg_time,
                    x=foot_coords['x'],
                    y=foot_coords['y'],
                    bounding_box= bb)
            else:
                self._current_occupants[person_id].update(
                    timestamp=msg_time,
                    x=foot_coords['x'],
                    y=foot_coords['y'],
                    bounding_box=bb)

        missing_ids = previous_ids - current_ids

        removed_occupants = {
            oid: previous_occupants_snapshot[oid]
            for oid in missing_ids
        }

        for oid in missing_ids:
            self._current_occupants.pop(oid, None)

        return self._current_occupants, removed_occupants

    def get_occupant_data_for_alert(self, current_time):
        alert_data = []
        for person_id in sorted(self._current_occupants):
            occupant = self._current_occupants[person_id]
            for frame in reversed(self._frame_history):
                if person_id in frame:
                    alert_data.append({
                        'bounding_box': frame[person_id]['bounding_box'],
                        'enter_time': occupant.first_seen,
                        'exit_time': 0,  # did not exit yet since in current_occupants
                        'dwell_time': current_time - self.first_seen,
                        'person_id': id
                    })
                    break
        return alert_data

occupant_histories : Dict[str, OccupantHistory] = {}

# last_trigger_occupancy[trigger_id] = { trigger_condition, (timestamp, count, alert_id) }
last_trigger_occupancy: Dict[str, Dict[str, Tuple[float, int, str]]] = {}
# last_trigger_loitering[trigger_id] = ( {occupant_id, Occupant}, timestamp, alert_id )
last_trigger_loitering: Dict[str, Tuple[Dict[str, Occupant], float, str]] = {}

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
    '''
    Create a connection to the database if it is not already connected.
    Ensures the db exists or create one if it does not.
    Ensures the tables exists or create them if they do not.
    '''

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
                 monitor_id VARCHAR(255) NOT NULL,
                 min_count INT,
                 max_count INT,
                 avg_count FLOAT
                 )
                """)

            # Add an index for faster query
            cursor.execute(f"""
                CREATE INDEX IF NOT EXISTS idx_timestamp_monitor
                ON {COUNT_TABLE}(timestamp, monitor_id)
                """)

            cursor.execute(f"""
                CREATE TABLE IF NOT EXISTS {HEATMAP_TABLE} (
                   timestamp DATETIME,
                   monitor_id VARCHAR(255) NOT NULL,
                   heatmap JSON
                )
                """)

            # Add an index for faster query
            cursor.execute(f"""
                CREATE INDEX IF NOT EXISTS idx_timestamp_monitor
                ON {HEATMAP_TABLE}(timestamp, monitor_id)
                """)

            cursor.close()
            return db_connection

        except Exception as e:
            logger.error(f"Exception connecting to MariaDB: {e}")
            logger.info("Retrying in 5 seconds...")
            await asyncio.sleep(5)

    return db_connection


async def get_triggers(r):
    #logger.debug("get_triggers")
    # Get triggers from Redis
    raw_triggers = await r.hgetall(TRIGGER_KEY)
    logger.debug(f'Raw triggers from redis: {raw_triggers}')

    # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
    # Convert to list of
    trigger_objs = [json.loads(trigger) for trigger in raw_triggers.values()]
    supported_trigger_conditions = [
        'required_accessories',
        'restricted_accessories',
        'occupancy_changed',
        'occupancy_over',
        'occupancy_under',
        'loitering_over'
    ]

    triggers = []
    for trigger in trigger_objs:
        if trigger['trigger_condition'] not in supported_trigger_conditions:
            logger.warning(f'Ignoring trigger with unsupported condition: "{trigger}"')
            continue
        triggers.append(trigger)

    if triggers == []:
        logger.debug(f'No triggers found at {TRIGGER_KEY}; adding default')
        triggers = [
             {
                "monitor_id": "0",
                "trigger_id": "0xDEADBEEF",
                "trigger_name": "Person missing vest (hard-coded)",
                "trigger_condition": "required_accessories",
                "params": [{"name":"accessories","value":"vest"}]
            }
        ]

    logger.debug(f'Parsed & validated triggers: {triggers}')
    return triggers

def convert_msg_timestamp_to_epoch_time(msg_ts, sys_time, channel=None):
    return sys_time # hack to stop drift


async def statistics_task():
    '''
    Task that saves the current statistics to the database at a fixed interval.
    '''

    logger.info(f'Statistics Task Started...')
    while True:
        now = time.time()
        if statistics_commit_interval_elapsed(now):
            await save_heatmap_statistics()
            await save_count_statistics()
            last_statistics_commit = time.time()

        await asyncio.sleep(STATISTICS_COMMIT_INTERVAL)

    logger.info(f'Statistics Task Stopped...')

def statistics_commit_interval_elapsed(now):
    return (now - last_statistics_commit) >= STATISTICS_COMMIT_INTERVAL


def heatmap_processing_interval_elapsed(now):
    return (now - last_heatmap_processing) >= HEATMAP_PROCESSING_INTERVAL

def get_people_count(frame) -> int:
    '''
    Find all the 'person' objects in the object_detection
    '''
    #logger.info(f'get_people_count - {frame}')
    if not hasattr(frame, 'object_detection'):
        return 0
    else:
        return sum(1 for obj in frame.object_detection if obj.label.startswith("person"))

async def update_count_statistics(recent_history):
    '''
    Update counts on last frames using the majority vote analytics method
    since objects may not be detected in each frame
    '''

    logger.debug(f"update_count_statistics()")

    def count_people_in_frames_by_channel(recent_frames: Dict[str, Deque]) -> Dict[str, Deque[int]]:
        # Extracts people counts from frames for each channel
        count_in_frames: Dict[str, Deque[Tuple[int, Any]]] = defaultdict(deque)

        for channel, frames in recent_frames.items():
            #logger.debug(f"count_people_in_frames_by_channel {channel} {len(frames)}")
            if not frames:
                logger.error(f'[{channel}]: no frames found!')
                continue

            #frames = [msg_obj for _, msg_obj in items]

            for frame in frames:
                msg_time, msg_obj = frame
                #logger.debug(f"[{channel}]: {frame}")
                count = get_people_count(msg_obj)
                logger.debug(f'...[{channel}]: {count}')
                count_in_frames[channel].append((count, frame))

        return count_in_frames

    def get_majority_vote(counts_by_channel: Dict[str, Deque[int]]) -> Dict[str, Optional[int]]:
        '''
        Returns the most common people count per channel
        '''

        majority_vote_result: Dict[str, Optional[Tuple[int, Any]]] = {}

        # Apply majority vote
        # (1)[0]: the top-most/most common value
        for channel, count_frame_pairs in counts_by_channel.items():
            logger.debug(f"get_majority_vote {channel} {len(count_frame_pairs)}")
            if count_frame_pairs:
                count_freq = Counter([count for count, _ in count_frame_pairs])
                most_common_count, _ = count_freq.most_common(1)[0]

                logger.debug(f'{count_freq} {most_common_count}')

                # Find the first frame with the most common count
                for count, frame in count_frame_pairs:
                    if count == most_common_count:
                        majority_vote_result[channel] = (most_common_count, frame)
                        break
                    else:
                        majority_vote_result[channel] = None

        return majority_vote_result


    # For each channel, get the people count detected in each frame.
    # Apply majority vote across frames per channel to get count.
    count_in_frames = count_people_in_frames_by_channel(recent_history)
    majority_vote_result = get_majority_vote(count_in_frames)

    for channel, result in majority_vote_result.items():

        count, frame = result

        monitor_id = channel.replace(DETECTION_CHANNEL_PREFIX, "")
        logger.info(f'[{monitor_id}]: {count} people')

        async with count_lock:
            # Update the stats for the channel
            if channel not in count_statistics:
                count_statistics[monitor_id] = CountStats()

            count_statistics[monitor_id].update(count, frame)

async def save_count_statistics():
    '''
    Save for all monitor_ids. Counts are reset after saving.
    '''
    if not count_statistics:
        return

    logger.debug(f'save_count_statistics {datetime.now()}')

    async with count_lock:
        for monitor_id, stats in count_statistics.items():

            timestamp = datetime.now(timezone.utc)

            logger.debug(f"Save {COUNT_TABLE}[{monitor_id}] {timestamp}")

            try:
                conn = await connect_to_db()

                cursor = conn.cursor()

                data_insert = f'''
                 INSERT INTO {COUNT_TABLE} (timestamp, monitor_id, min_count, max_count, avg_count)
                     VALUES (%s, %s, %s, %s, %s)
                 '''

                #logger.debug(f'{data_insert}')

                # Insert the data
                vmin = 0 if stats.count_min == float('inf') else stats.count_min
                vmax = 0 if stats.count_max == float('-inf') else stats.count_max
                cursor.execute(data_insert, (timestamp, monitor_id, vmin, vmax, stats.count_avg))

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

def visualize_heatmap(heatmap):

    if np.all(heatmap==0):
        #logger.info("Heatmap is empty. Nothing to visualize.")
        return

    # Define the intensity levels using Unicode block characters
    intensity_levels = " ����"

    # Function to map the matrix values to intensity levels
    def map_to_intensity(matrix):
        max_val = matrix.max()
        min_val = matrix.min()
        range_val = max_val - min_val
        step = range_val / (len(intensity_levels) - 1)

        def get_intensity(value):
            index = int((value - min_val) / step)
            return intensity_levels[index]

        return np.vectorize(get_intensity)(heatmap)

    # Map the matrix to intensity levels
    intensity_matrix = map_to_intensity(heatmap)

    # Print the heatmap to stdout
    for row in intensity_matrix:
        logger.info("".join(row))

async def update_heatmap_statistics(recent_history):
    '''
    Updates the current heatmap statistics.
    The heatmap is a histogram table representing a snapshot of the people_count in the fov.
    '''
    #logger.debug("update_heatmap_statistics()")

    def get_max_count_frame_by_channel(recent_frames: Dict[str, Deque]) -> Dict[str, Any]:
        # Return the frame with the most people detected per channel
        result = {}

        for channel, items in recent_frames.items():
            if not items:
                logger.error(f'[{channel}]: no frames found!')
                continue

            max_count = -1
            max_frame = None

            frames = [msg_obj for _, msg_obj in items]

            for frame in frames:
                logger.debug(f"[{channel}]: {frame}")
                count = get_people_count(frame)
                logger.debug(f'...[{channel}]: {count}')

                if count > max_count and count > 0:
                    max_count = count
                    max_frame = frame

            if max_frame:
               result[channel] = max_frame

            return result

    # For each channel, get the people count detected in each frame.
    # Use the frame with the most number of people found.
    max_count_frames = get_max_count_frame_by_channel(recent_history)

    for channel, frame in max_count_frames.items():
        logger.debug(f"{channel}: {frame}")

        if not hasattr(frame, 'object_detection'):
            logger.debug(f'[{channel}]: no objects detected!')
            continue # No objects found in frame

        for person in frame.object_detection:

            if not person.label.startswith('person'):
                logger.debug(f'[{channel}]: no people detected!')
                continue # obj is not a person, skip

            id = person.tracking_id

            have_left = False
            have_right = False
            landmarks = None
            bb = None
            foot_coords = get_foot_coordinates(person)

            if not foot_coords:
                continue;

            # Convert to heatmap array indices
            #logger.debug(f'{foot_coords}')

            x_idx = int(float(foot_coords['x']) * float(HEATMAP_COLS-1))
            y_idx = int(float(foot_coords['y']) * float(HEATMAP_ROWS-1))

            monitor_id = channel.replace(DETECTION_CHANNEL_PREFIX, "")

            # Update heatmap for the channel
            if monitor_id not in heatmap_statistics:
                heatmap_statistics[monitor_id] = HeatmapStats()

            try:
                heatmap_statistics[monitor_id].heatmap[y_idx, x_idx] += 1
            except Exception as exc:
                logger.exception(exc)
                logger.info(f'{x_idx}, {y_idx}')

def parse_messages(messages):
    try:
         # sort messages into frame history by monitor
        for recv_time, message in messages:
            channel = message['channel']
            if channel not in frame_history_by_channel:
                frame_history_by_channel[channel] = deque(maxlen=LOOKBACK_FRAMES)
                # TODO: delete stale histories?
            #logger.info(f'RAW {message}')
            # TODO: optimize by only json-decoding the messages that remain in frame history?
            msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))
            msg_time = convert_msg_timestamp_to_epoch_time(
                msg_obj.parameters.timestamp,
                recv_time,
                channel)
            frame_history_by_channel[channel].append( (msg_time, msg_obj) )
        #logger.debug(f'messages={len(messages)} frame_history_by_channel={len(frame_history_by_channel[channel])}')
    except Exception as exc:
        logger.exception(exc)

    return frame_history_by_channel

def get_trigger_param(trigger, name):
    for param in trigger['params']:
        if param['name'] == name:
            return param['value']

    raise ValueError(f'{name} not in params for trigger {trigger}')

def make_loitering_alert(trigger, monitor_id, alert_occupants, alert_id, alert_time, end_time=0):

    if not alert_id:
        alert_id = str(uuid.uuid4())

    alert = {
        'monitor_id': monitor_id,
        'source_trigger': trigger,
        'alert_id': alert_id,
        'time': alert_time,
        'end_time': end_time,
        'occupants': alert_occupants
    }

    logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")

    return alert

def get_loitering_alert(trigger, monitor_id, alert_time, threshold, occupants, left_occupants):
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
    occupants_changed = False

    for occupant in occupants.values():
        dwell_time = alert_time - occupant.first_seen
        occupant.loitering = dwell_time > threshold

        if occupant.loitering:
            loitering_occupants.append(occupant)
        else:
            dwell_str = "Entered the fov" if dwell_time == 0 else f"{dwell_time:.2f} seconds"
            logger.info(f'   occupant[{occupant.id}]: {dwell_str}')
            occupants_changed = True

    if left_occupants:
        for occupant in left_occupants.values():
            logger.info(f'   occupant [{occupant.id}]: Exited the fov')
            occupants_changed = True

    # There are no loitering occupants and alert exists then it should clear.
    if not loitering_occupants:
        if last_trigger_loitering and trigger_id in last_trigger_loitering:
            alert_occupants = []

            occupants_dict, last_timestamp, alert_id = last_trigger_loitering[trigger_id]

            for occupant_id, occupant in occupants_dict.items():
                #logger.info(f'   occupant [{occupant_id}]: Exited the fov')
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
            return make_loitering_alert(trigger, monitor_id, alert_occupants, alert_id, last_timestamp, alert_time)
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
            #logger.info(f'   occupant [{occupant.id}]: Exited the fov')
            removed_occupants.append(occupant)

    # Remove occupants that have left from the history
    for occupant in removed_occupants:
        occupants_dict.pop(occupant.id, None)

    # There are new or existing occupants so alert started
    if new_occupants or still_present:
        if len(previous_keys) == 0:
            logger.info(f'*** LOITERING_OVER > {threshold} STARTED ***')
            #logger.info(alert_occupants)
            return make_loitering_alert(trigger, monitor_id, alert_occupants, alert_id, alert_time, 0)
        elif occupants_changed:
            logger.info(f'*** LOITERING_OVER > {threshold} ACTIVE *** : Occupancy Changed')
            #logger.info(alert_occupants)
            return make_loitering_alert(trigger, monitor_id, alert_occupants, alert_id, alert_time, 0)
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
        return make_loitering_alert(trigger, monitor_id, alert_occupants, alert_id, alert_time, end_time)

def make_occupancy_alert(channel, trigger, alert_id, alert_time, end_time, current_occupants, removed_occupants):
    '''
    Create a new occupancy alert.
    '''

    trigger_id = trigger['trigger_id']
    trigger_condition = trigger['trigger_condition']
    occupants = []

    if not alert_id:
        alert_id = str(uuid.uuid4())

    if current_occupants:
        for occupant in current_occupants.values():
            new_occupant = {
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': 0,
                'dwell_time':  alert_time - occupant.first_seen,
                'person_id': occupant.id
            }
            occupants.append(new_occupant)
    elif not current_occupants and removed_occupants:
        # Sending the cleared alert
        for occupant in removed_occupants.values():
            old_occupant = {
                'bounding_box': occupant.bounding_box,
                'enter_time': occupant.first_seen,
                'exit_time': alert_time,
                'dwell_time':  alert_time - occupant.first_seen,
                'person_id': occupant.id
            }
            occupants.append(old_occupant)
    else:
        return None # no previous or current

    alert = {
        'source_trigger': trigger,
        'alert_id': alert_id,
        'time': alert_time,
        'end_time': end_time,
        'occupants': occupants,
        'type': 'occupancy'
     }

    #logger.debug(f'ALERT - {alert}')
    logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")

    return alert

def process_occupancy_trigger(trigger, trigger_condition, channel, alert_time, occupants, removed_occupants):
    '''
    Process the occupancy trigger and create an alert if needed
    '''
    global last_trigger_occupancy

    trigger_id = trigger['trigger_id']
    alert_id = str(uuid.uuid4())
    trigger_data = last_trigger_occupancy.get(trigger_id, {}).get(trigger_condition, (0.0, 0, alert_id))
    timestamp, last_occupancy, alert_id = trigger_data
    occupancy_alerts = False
    alert = None

    if channel not in count_statistics:
        logger.warning(f'No count stats for channel [{channel}]!')
        return None

    if count_statistics[channel].count_current_frame is None:
        logger.warning(f'No frame info for channel [{channel}]!')
        return None

    current_occupancy = count_statistics[channel].count_current

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

    if occupancy_alerts:
        alert_log = f"*** {alert_log} {'STARTED' if timestamp == 0.0 else 'ACTIVE'} ***"
        logging.info(alert_log)
        end_time = 0
        if not trigger_id in last_trigger_occupancy:
            last_trigger_occupancy[trigger_id] = {}
        last_trigger_occupancy[trigger_id][trigger_condition] = (alert_time, current_occupancy, alert_id)
        alert = make_occupancy_alert(channel, trigger, alert_id, alert_time, end_time, occupants, removed_occupants)
        #logger.info(f"        -> {alert_id} time={alert_time}, end={end_time}")
    elif timestamp != 0.0:
        alert_log = f"*** {alert_log} CLEARED ***"
        end_time = alert_time
        logging.info(alert_log)
        alert = make_occupancy_alert(channel, trigger, alert_id, alert_time, end_time, None, removed_occupants)
        del last_trigger_occupancy[trigger_id][trigger_condition] # no more active alert

    return alert


def apply_triggers(triggers, recent_history):
    global occupant_histories

    alerts = []
    trigger = None
    message = None # set these for logging in exception handler
    try:
        for trigger in triggers:
            monitor_id = trigger['monitor_id']
            trigger_channel = DETECTION_CHANNEL_PREFIX + monitor_id

            if not count_statistics or monitor_id not in count_statistics:
                logger.info("UH OH")
                if not count_statistics:
                    logger.info("no monitor")
                else:
                    logger.info("no monitor_id")
                continue; # There needs to be data

            if not occupant_histories or not monitor_id in occupant_histories:
                logging.info(f"New occupant history for {monitor_id}")
                occupant_histories[monitor_id] = OccupantHistory()

            history = occupant_histories.get(monitor_id)

            if not history:
                logging.info(f"No occupant history for {monitor_id}")
                continue;

            # Get the latest occupants list
            msg_time, msg_obj = count_statistics[monitor_id].count_current_frame
            occupants, removed_occupants = occupant_histories.get(monitor_id).add_detections(monitor_id, msg_time, msg_obj)

            trigger_accessories = None

            for param in trigger['params']:
                if param['name'] == 'accessories':
                    trigger_accessories = param['value'].split(',')
                    trigger_accessories = [a.strip() for a in trigger_accessories] # strip all whitespace
                    break

            # Set "accessory_violations" function depending on the trigger type
            trigger_condition = trigger['trigger_condition']

            match trigger_condition:
                case 'required_accessories':
                    def accessory_violations(detected_accessories):
                        return sorted(set(trigger_accessories) - set(detected_accessories))
                case 'restricted_accessories':
                    def accessory_violations(detected_accessories):
                        return sorted(set(trigger_accessories) & set(detected_accessories))
                case 'occupancy_changed' | 'occupancy_over' | 'occupancy_under':
                    alert = process_occupancy_trigger(trigger, trigger_condition, monitor_id, msg_time, occupants, removed_occupants)
                    if alert:
                        alerts.append(alert)
                    continue # not an accessories trigger
                case 'loitering_over':
                    threshold = int(get_trigger_param(trigger, 'threshold')) / 1000.0 # ms to sec
                    alert = get_loitering_alert(trigger, monitor_id, msg_time, threshold, occupants, removed_occupants)
                    if alert:
                        alerts.append(alert)
                    continue # not an accessories trigger
                case _:
                    raise RuntimeError(f'Unexpected trigger condition \'{trigger_condition}\' in trigger {trigger}')

            # look for violators in the history buffer
            recent_frames = recent_history.get(trigger_channel, [])

            # If not enough history in buffer (beginning of stream), no alert
            if len(recent_frames) < LOOKBACK_FRAMES:
                continue

            msg_time, _ = recent_frames[-1]

            # Find violators in each frame
            def get_violators_in_frame(frame):
                if not hasattr(frame, 'object_detection'):
                    return [] # no people = no violators

                violators = []
                for top_object in frame.object_detection:
                    # If the top-level object is not a person, go to next one
                    if not top_object.label.startswith('person'):
                        continue

                    # Top object is in fact a person; check for sub-object(s), i.e., accessories
                    person = top_object
                    sub_objects = person.object_detection if hasattr(person, 'object_detection') else []

                    # extract labels -- these are the accessories if any
                    accessories = [so.label for so in sub_objects]

                    # check for any accessory violations
                    violations = accessory_violations(accessories)
                    if not violations:
                        continue # this person is in compliance, check the next person

                    # This person violated the accessory policy; log their rectangle & the violations
                    violators.append( (person.rectangle, violations) )
                return violators

            violators_by_frame = [get_violators_in_frame(frame) for _, frame in recent_frames]

            # Don't trigger an alert unless all frames in history have a violator
            if not all(violators_by_frame):
                continue # at least one frame without violators, continue to next trigger

            # Every frame had at least one violator. Set causes to the last frame.
            latest_frame_violators = violators_by_frame[-1]

            # Create the causes clause for the response from the
            # last frame's list of bounding boxes for people and missing
            # accessories.
            # Also convert bounding box from x,y,w,h to x0,y0,x1,y1.
            causes = [
                {
                    'accessories': ', '.join(accessory_violations),
                    'violator': {
                        'top_left': {
                            'x': bb.x,
                            'y': bb.y
                        },
                        'bottom_right': {
                            'x': bb.x + bb.width,
                            'y': bb.y + bb.height
                        }
                    }
                } for bb, accessory_violations in latest_frame_violators
            ]

            # Each alert has a unique id
            alert_id = str(uuid.uuid4())

            end_time = 0;

            alerts.append(
                {
                    'source_trigger': trigger,
                    'alert_id': alert_id,
                    'time': msg_time,
                    'end_time': end_time,
                    'causes': causes,
                    'type': 'accessories'
                }
            )

            #logger.debug(alerts[-1])

        # todo back to info pton
        logger.debug(f'Filtered {len(recent_history)} Detection messages through {len(triggers)} trigger(s), generating {len(alerts)} alert(s)')
        return alerts

    except Exception as exc:
        logger.error(f"Exception while applying trigger '{trigger}' to redis message '{message}': {str(exc)}")
        logger.exception(exc)
        # raise(exc) # : remove in release
        return []

async def check_for_alerts(r : redis.Redis):
    global message_list
    if len(message_list) == 0:
        return # no messages

    # TODO: move logic into another module that doesn't talk to redis or post to task group
    current_messages = message_list
    message_list = [] # empty the message list

    # Process count before checking alerts
    recent_history = parse_messages(current_messages)
    await update_count_statistics(recent_history)

    # Apply triggers and process alerts
    triggers = await get_triggers(r)
    alerts = apply_triggers(triggers, recent_history)

    for alert in alerts:
        logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
        asyncio.ensure_future(r.publish(ALERT_CHANNEL, json.dumps(alert)))

    now = time.time()

    if heatmap_processing_interval_elapsed(now):
        await update_heatmap_statistics(recent_history)
        last_heatmap_processing = now

async def save_heatmap_statistics():
    '''
    Saves the heatmap to the database.
    Resets the heatmap statistics after committing.
    '''

    timestamp = datetime.now(timezone.utc)

    if not heatmap_statistics:
        logger.debug(f'No heatmap stats to save')
        return

    async with count_lock:
        # Save for all monitor_ids
        for monitor_id, stats in heatmap_statistics.items():
            try:
                #visualize_heatmap(stats.heatmap)

                logger.debug(f"Saving heatmap stats to db for {monitor_id}...{timestamp}")
                conn = await connect_to_db()

                cursor = conn.cursor()

                # Convert it to a JSON string to store
                array_json = json.dumps(stats.heatmap.tolist())

                # Insert the data
                data_insert = f'''
                    INSERT INTO {HEATMAP_TABLE} (timestamp, monitor_id, heatmap)
                         VALUES (%s, %s, %s)
                     '''

                cursor.execute(data_insert, (timestamp, monitor_id, array_json))

                # Save and close
                conn.commit()
                cursor.close()

                logger.debug(f"Successfully saved heatmap data for {monitor_id}")

                # Clear the statistis once it has been saved
                stats.reset()

            except Exception as e:
                logger.error(f"An error occurred: {e}")
                return False

    return True # success

async def run_count_query(r : redis.Redis, token, monitor_id, start_time, end_time):
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
            WHERE monitor_id = %s
                AND timestamp BETWEEN %s AND %s
            '''

        cursor.execute(query, (monitor_id, datetime.fromtimestamp(start_time, timezone.utc), datetime.fromtimestamp(end_time, timezone.utc)))

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

        #logger.debug(f'Publishing to channel {ANALYTICS_CHANNEL}: {result}')
        asyncio.ensure_future(r.publish(ANALYTICS_CHANNEL, json.dumps(result)))

    except Exception as e:
        logger.error(f"An error occurred: {e}")


async def run_heatmap_query(r : redis.Redis, token, monitor_id, start_time, end_time):
    '''
    Process a heatmap analytics request.
    '''

    logger.info(f'run_heatmap_query {datetime.fromtimestamp(start_time)} -> {datetime.fromtimestamp(end_time)}')

    try:
        await save_heatmap_statistics()

        #logger.info("....querying db....")

        conn = await connect_to_db()

        if conn is None or not conn.is_connected():
           logger.error(f"Error connecting to database!")
           # TODO ERROR
           return False

        cursor = conn.cursor()

        # Query the database for entries within the timestamp range
        query = f'''
            SELECT heatmap
            FROM {HEATMAP_TABLE}
            WHERE monitor_id = %s
                AND timestamp BETWEEN %s AND %s
            '''

        #logger.info(f'{query}')

        cursor.execute(query, (monitor_id, datetime.fromtimestamp(start_time, timezone.utc), datetime.fromtimestamp(end_time, timezone.utc)))

        # Fetch all heatmap rows for processing
        results = cursor.fetchall()
        cursor.close()

        sum_array = np.zeros((HEATMAP_ROWS, HEATMAP_COLS))
        #sum_array2 = np.zeros((HEATMAP_ROWS, HEATMAP_COLS))
        if len(results) == 0:
            logger.info(f'No results found.')
        else:
            logger.info(f'{len(results)} entries found')

            #for i, row in enumerate(results):
            #    arr = np.array(json.loads(row[0]))
            #    logger.info(f'Array {i} shape: {arr.shape}')
            #    if arr.shape != (HEATMAP_ROWS, HEATMAP_COLS):
            #        logger.warning(f'Array {i} has shape {arr.shape}, expected ({HEATMAP_ROWS}, {HEATMAP_COLS})')
            #    else:
            #        sum_array2 += arr

            # Process the results and find the sum in each cell/bin
            # Convert JSON strings to arrays
            arrays = [np.array(json.loads(row[0])) for row in results]

            # Compute element-wise maximum
            # This is the sum value from each element in the arrays that are returned,
            # e.g. element [0][0] to element [63][63] down all the rows (arrays) that are returned
            sum_array = np.sum(arrays, axis=0)
            visualize_heatmap(sum_array)
            #visualize_heatmap(sum_array2)

        # Post results
        #{
        #   'rows': rows,
        #   'cols': cols,
        #   'data': 2D array
        #}
        result_json = {
                'rows': HEATMAP_ROWS,
                'cols': HEATMAP_COLS,
                'data': sum_array.tolist()
            }

        result = {
                'sync_id': token,
                'result': result_json
            }

        #logger.debug(f'Publishing to channel {ANALYTICS_CHANNEL}: {result}')
        asyncio.ensure_future(r.publish(ANALYTICS_CHANNEL, json.dumps(result)))

    except Exception as e:
        logger.error(f"An error occurred: {e}")


#def visualize_heatmap(heatmap_data):
    # Plot the heatmap
    #plt.figure(figsize=(10, 10))
    #plt.imshow(heatmap_data, cmap='viridis', interpolation='nearest')
    #plt.colorbar(label='Max Value')
    #plt.title('Heatmap for Each (row_idx, col_idx)')
    #plt.xlabel('Column Index')
    #plt.ylabel('Row Index')
    #plt.show()


async def register_pubsub_listeners(r: redis.Redis):
    logger.info(f'Register REDIS pubsub');

    def detection_message_handler(message):
        # called by the pubsub async runner when a message is received
        global message_list

        now = time.time()
        if message is not None:
            message_list.append((now, message))
            data = message["data"]
            data_len = len(data)
            SNIP_SIZE = 20
            data_str = str(data) if data_len < (2*SNIP_SIZE+2) else f'{str(data)[:SNIP_SIZE]}..{str(data)[-SNIP_SIZE:]} ({data_len} bytes)'
            logger.debug(f'Received message on ch "{message["channel"]}" >> "{data_str}"')
        else:
            logger.debug('No message from get_message')
        if alert_period_has_elapsed(now):
            asyncio.ensure_future(check_for_alerts(r))

    def analytics_request_handler(message):
        logger.debug(f'Received message on analytics channel')

        if message and message['type'] == 'message':

            # {
            #     'sync_id': sync_id,
            #     'monitor_id': data.monitorId,
            #     'from_time': data.fromTime,
            #     'to_time': data.toTime,
            #     'analytics_type': analytics_type, # either 'count' or 'heatmap'
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
                #monitor_id = data.get('monitor_id')
                #from_time = data.get('from_time')
                #to_time = data.get('to_time')
                #analytics_type = data.get('analytics_type')

                def handle_unknown_analytics(query_type):
                    logger.error(f'Unknown {query_type} analytics type')

                # Handling should be non-blocking
                if( request.analytics_type == 'count'):
                    asyncio.create_task(run_count_query(r, request.sync_id, request.monitor_id, request.from_time, request.to_time))
                elif( request.analytics_type == 'heatmap'):
                    asyncio.create_task(run_heatmap_query(r, request.sync_id, request.monitor_id, request.from_time, request.to_time))
                else:
                    handle_unknown_analytics()
            except json.JSONDecodeError:
                print(f"[Subscriber] Received non-JSON message: {message['data'].decode('utf-8')}")
        else:
            logger.debug('No message for analytics')

    # register subscribe pattern handler, return pubsub to be used in caller for .run()
    pubsub = r.pubsub()
    await pubsub.psubscribe(**{DETECTION_CHANNEL_PREFIX + '*': detection_message_handler})
    logger.debug(f'subscribed to {DETECTION_CHANNEL_PREFIX} channel(s)')

    await pubsub.subscribe(**{ANALYTICS_CHANNEL: analytics_request_handler})
    logger.debug(f'subscribed to {ANALYTICS_CHANNEL} channel')

    return pubsub

def handle_no_db_error():
       logger.info(f"No MariaDB connection!")
       # should fail like api
       sys.exit()

async def async_main():
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    channel_listener_task = None
    statistics_process_task = None

    try:
        await connect_to_redis(r)
        await connect_to_db()

        def quit_handler ():
            if channel_listener_task == None:
                raise RuntimeError('Got SIGTERM but no task to cancel!')
            logging.info('Got SIGTERM, cancelling listener task..')
            channel_listener_task.cancel()

            if statistics_task:
                statistics_task.cancel()

            if db_connection:
                db_connection.close()
                db_connection = None

        pubsub = await register_pubsub_listeners(r)
        loop = asyncio.get_event_loop()
        loop.add_signal_handler(signal.SIGTERM, quit_handler)
        channel_listener_task = asyncio.create_task(pubsub.run()) # runs forever until cancelled
        statistics_process_task = asyncio.create_task(statistics_task())

        await channel_listener_task
        await statistics_process_task

    except asyncio.CancelledError:
        logger.info('Got cancelled exception, shutting down..')
    finally:
        logger.info('Closing redis client..')
        await r.aclose()

if __name__ == "__main__":

    # logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(filename)s::%(funcName)s %(message)s')
    logging.basicConfig(level=LOG_LEVEL, format='%(levelname)s %(message)s')
    asyncio.run(async_main())
    logging.info('Exiting server.')

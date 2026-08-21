# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import redis.asyncio as redis
import asyncio
import logging
import json
from collections import defaultdict, deque, Counter
import os
from typing import Dict, Deque, Optional, Any, Tuple
from dataclasses import dataclass, field
from tripwire_crossings import Tripwire, calculate_tripwire_crossings
from datetime import datetime, timezone
import mysql.connector
import numpy as np
import sys
import time
from types import SimpleNamespace

ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'tripwire-analytics.alerts')
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
TRIPWIRE_KEY = os.environ.get('TRIPWIRE_KEY', 'TATripwires')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'TATriggers')
ANALYTICS_CHANNEL = os.environ.get('ANALYTICS_CHANNEL', 'trjipwire-analytics.analytics')

# e.g., monitor 0 would be "detection.rz:0"
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.rz') + ':'

MARIADB_PASSWORD = os.environ.get('MARIADB_PASSWORD')
MARIADB_HOST = os.environ.get('MARIADB_HOST')
MARIADB_USER = os.environ.get('MARIADB_USER')
logger = logging.getLogger(__file__)
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))

MARIADB_PORT = os.environ.get('MARIADB_PORT', 3306)
MARIADB_DB = os.environ.get('MARIADB_DB', 'iot_solutions')
HEATMAP_ROWS = int(os.environ.get('DEFAULT_HEATMAP_ROWS', 64))
HEATMAP_COLS = int(os.environ.get('DEFAULT_HEATMAP_COLS', 64))
HEATMAP_TABLE = os.environ.get('DEFAULT_HEATMAP_TABLE', 'heatmap_ta_statistics')
COUNT_TABLE = os.environ.get('DEFAULT_COUNT_TABLE', 'count_ta_statistics')
HEATMAP_PROCESSING_INTERVAL = os.environ.get('HEATMAP_PROCESSING_INTERVAL', 2.0) # 2 seconds
STATISTICS_COMMIT_INTERVAL = os.environ.get('STATISTICS_COMMIT_INTERVAL', 60.0) # 60 seconds
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)

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

# Store count statistics per monitor
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

def handle_no_db_error():
       logger.info(f"No MariaDB connection!")
       # should fail like api
       sys.exit()

def close_db_connection():
    '''
    Safely close the module-level db_connection, if open.
    '''
    global db_connection
    if db_connection:
        try:
            db_connection.close()
        except Exception as e:
            logger.debug(f'Error closing db connection: {e}')
        db_connection = None

async def connect_to_db():
    '''
    Create a connection to the database if it is not already connected.
    Ensures the db exists or create one if it does not.
    Ensures the tables exists or create them if they do not.
    '''

    global db_connection
    if db_connection:
        return db_connection

    logger.info(f"connecting to db...")

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


def statistics_commit_interval_elapsed(now):
    return (now - last_statistics_commit) >= STATISTICS_COMMIT_INTERVAL


def heatmap_processing_interval_elapsed(now):
    return (now - last_heatmap_processing) >= HEATMAP_PROCESSING_INTERVAL

def get_people_count(frame) -> int:
    '''
    Find all the 'person' objects in the object_detection
    '''
    #logger.info(f'get_people_count - {frame}')
    if not 'object_detection' in frame:
        logger.debug("no object detection")
        return 0
    else:
        return sum(1 for obj in frame['object_detection'] if obj['label'].startswith("person"))

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
        logger.info(f'camera[{monitor_id}]: {count} people')

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

    logger.info(f'save_count_statistics {datetime.now()}')

    async with count_lock:
        for monitor_id, stats in count_statistics.items():

            timestamp = datetime.now(timezone.utc)

            logger.info(f"Save {COUNT_TABLE}[{monitor_id}] {timestamp}")

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

def get_foot_coordinates(person):
    '''
    To calculate position within the grid:
        - Use average position of both ankles if both are available
        - else use either left or right ankle, whichever one is available
        - else if no ankles are detected then use the midpoint of the bottom of the bounding box
        - otherwise not a valid person
    '''

    if isinstance(person, dict):
        person = SimpleNamespace(**person)

    have_left = False
    have_right = False
    landmarks = None
    bb = None
    foot_coords = {}

    # Convert landmarks and rectangle if they exist and are dicts
    if hasattr(person, 'landmarks') and isinstance(person.landmarks, dict):
        person.landmarks = SimpleNamespace(**person.landmarks)

    if hasattr(person, 'rectangle') and isinstance(person.rectangle, dict):
        person.rectangle = SimpleNamespace(**person.rectangle)

    # Check for ankle landmarks or Bounding Box

    landmarks = getattr(person, 'landmarks', None)
    bb = getattr(person, 'rectangle', None)

    left_ankle = getattr(landmarks, 'left_ankle', None)
    right_ankle = getattr(landmarks, 'right_ankle', None)

    # Convert ankles to SimpleNamespace if needed
    if isinstance(left_ankle, dict):
        left_ankle = SimpleNamespace(**left_ankle)
    if isinstance(right_ankle, dict):
        right_ankle = SimpleNamespace(**right_ankle)


    if left_ankle and right_ankle:
        # foot_coords (x,y) = midpoint of ankles
        #logger.info('using left and right')
        foot_coords['x'] = (left_ankle.x + right_ankle.x) / 2.0
        foot_coords['y'] = (left_ankle.y + right_ankle.y) / 2.0
    elif left_ankle:
        #logger.info('using left only')
         foot_coords['x'] = left_ankle.x
         foot_coords['y'] = left_ankle.y
    elif right_ankle:
         #logger.info('using right only')
         foot_coords['x'] = right_ankle.x
         foot_coords['y'] = right_ankle.y
    elif bb:
         #logger.info('using bb')
         foot_coords['x'] = (bb.x + bb.width) / 2.0
         foot_coords['y'] = min(1, bb.y + bb.height)
    else:
         logger.error(f'Person detected without landmarks or bounding box!')
         foot_coords = None

    return foot_coords

def visualize_heatmap(heatmap):

    if np.all(heatmap==0):
        #logger.info("Heatmap is empty. Nothing to visualize.")
        return

    # Define the intensity levels using Unicode block characters
    intensity_levels = ""

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
               #result[channel] = max_frame
                ns_frame = SimpleNamespace(**max_frame)
                if 'object_detection' in max_frame:
                    ns_frame.object_detection = [SimpleNamespace(**obj) for obj in max_frame['object_detection']]
                result[channel] = ns_frame


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
                continue

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


async def save_heatmap_statistics(visualize=False):
    '''
    Saves the heatmap to the database.
    Resets the heatmap statistics after committing.
    '''

    timestamp = datetime.now(timezone.utc)

    if not heatmap_statistics:
        logger.info(f'No heatmap stats to save')
        return

    async with count_lock:
        # Save for all monitor_ids
        for monitor_id, stats in heatmap_statistics.items():
            try:
                if visualize:
                    visualize_heatmap(stats.heatmap)

                logger.info(f"Saving heatmap stats to db for {monitor_id}...{timestamp}")
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


#def visualize_heatmap(heatmap_data):
    # Plot the heatmap
    #plt.figure(figsize=(10, 10))
    #plt.imshow(heatmap_data, cmap='viridis', interpolation='nearest')
    #plt.colorbar(label='Max Value')
    #plt.title('Heatmap for Each (row_idx, col_idx)')
    #plt.xlabel('Column Index')
    #plt.ylabel('Row Index')
    #plt.show()

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
        await save_heatmap_statistics(visualize=True)

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


def normalize_ts(ts):
    ts = float(ts)
    # If ts is in milliseconds, convert to seconds
    return ts / 1000 if ts > 1e10 else ts

class TripwireAnalytics():
    # TODO: filter_size, overlap_size, max_len overridden in env
    def __init__(self, r: redis.Redis, filter_len = 8):
        self._r = r
        self._message_list = []
        self._frame_metadata_by_monitor = defaultdict(deque) # deque of messages by monitor, in received order
        self._filter_len = filter_len
        self._overlap_len = 1
        self._calculate_tripwires_impl = calculate_tripwire_crossings
        self._tripwires = []
        self._triggers = []
        self.recent_crossings = defaultdict(list)
        self.recent_ids = defaultdict(dict)  # tripwire_id ? {tracking_id: timestamp}

    def enqueue_message(self, timestamp, message):
        self._message_list.append((timestamp, message))

        # check if there's more than 1 second worth of messages
        # TODO: should it be from last alert?
        oldest_message_time, _ = self._message_list[0]
        if timestamp - oldest_message_time >= ALERT_PERIOD:
            asyncio.ensure_future(self.check_for_alerts())

    async def get_tripwires(self):
        if not self._tripwires:
            await self.update_tripwires()
        return self._tripwires

    async def get_triggers(self):
        if not self._triggers:
            await self.update_triggers()
        return self._triggers

    async def check_for_alerts(self):
        try:
            if len(self._message_list) == 0:
                return # no messages

            current_messages = self._message_list
            self._message_list = [] # empty the message list

            tripwires = await self.get_tripwires()
            triggers = await self.get_triggers()

            # Process count before checking alerts
            recent_history = self.parse_messages(current_messages)
            await update_count_statistics(recent_history)

            # Process count before checking alerts
            alerts = self.apply_triggers(triggers, tripwires, current_messages)

            for alert in alerts:
                logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
                # TODO: add this to task list instead of awaiting?
                await self._r.publish(ALERT_CHANNEL, json.dumps(alert))

            now = time.time()

            if heatmap_processing_interval_elapsed(now):
                await update_heatmap_statistics(recent_history)
                last_heatmap_processing = now

        except Exception as exc:
            logger.error('Exception in check_for_alerts:')
            logger.exception(exc)
            logger.error('Database or received messages likely corrupted, will keep trying..')


    async def update_tripwires(self):
        # Get tripwires from Redis
        raw_tripwires = await self._r.hgetall(TRIPWIRE_KEY)
        logger.debug(f'Raw tripwires from redis: {raw_tripwires}')

        # redis HGETALL returns in format {k : v} where k = region_id, v = full region in JSON string
        # Decode region JSON info
        tripwires = {id: json.loads(tripwire_str) for id, tripwire_str in raw_tripwires.items()}

        logger.debug(f'Parsed & validated tripwires: {tripwires}')
        self._tripwires = tripwires
        return tripwires


    async def update_triggers(self):
        # Get triggers from Redis
        raw_triggers = await self._r.hgetall(TRIGGER_KEY)
        logger.debug(f'Raw triggers from redis: {raw_triggers}')

        # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
        # Convert to list of triggers
        triggers = [json.loads(trigger) for trigger in raw_triggers.values()]

        supported_trigger_conditions = ['flowrate']
        triggers = [t for t in triggers if t['trigger_condition'] in supported_trigger_conditions]

        logger.debug(f'Parsed & validated triggers: {triggers}')
        self._triggers = triggers
        return triggers



    def calculate_crossings(self, ts_frame_tuples, filter_len, window_len, tripwires):
        # Helper to flatten direction data
        def flatten_direction(d):
            entry = d['entry']
            exit = d['exit']
            return [
                [entry['x'], entry['y']],
                [exit['x'], exit['y']]
            ]

        # Helper to flatten wire coordinates
        def flatten_wire(wire):
            return [[coord['x'], coord['y']] for coord in wire]

        # Convert tripwire definitions to Tripwire objects
        tripwire_objs = [
            Tripwire(
                tripwire_id=t['tripwire_id'],
                name=t['tripwire_name'],
                direction=flatten_direction(t['direction']),
                wire=flatten_wire(t['wire'])
            ) for t in tripwires.values()
        ]

        # Run the crossing detection algorithm
        try:
            logger.debug(
                f'Calling calculate_tripwires_impl: raw_data={ts_frame_tuples!r}, '
                f'filter_size={filter_len!r}, '
                f'window_size={window_len!r}, '
                f'tripwires={tripwire_objs!r}'
            )
            counts = self._calculate_tripwires_impl(
                raw_data=ts_frame_tuples,
                filter_size=filter_len,
                window_size=window_len,
                tripwires=tripwire_objs
            )
            logger.debug(f'Result of calculate_tripwires_impl: {counts}')
        except Exception:
            logger.exception('Exception in crossing calculation algorithm')
            counts = [(0, 0)] * len(tripwire_objs)

        # Build results with direction-specific timestamps
        results = {}

        for idx, result in enumerate(counts):
            tripwire_id = list(tripwires.keys())[idx]

            entry_count = result['entries']
            exit_count = result['exits']
            entry_times = result['entry_times']
            exit_times = result['exit_times']
            entry_ids = result['entry_ids']
            exit_ids = result['exit_ids']

            logger.debug(f'Tripwire {tripwire_id}: entries={entry_count}, exits={exit_count}')
            logger.debug(f'Tripwire {tripwire_id}: entry_times={entry_times}, exit_times={exit_times}')
            logger.debug(f'Tripwire {tripwire_id}: entry_ids={entry_ids}, exit_ids={exit_ids}')

            results[tripwire_id] = {
                'entries': entry_count,
                'exits': exit_count,
                'entry_timestamps': entry_times,
                'exit_timestamps': exit_times,
                'entry_ids': entry_ids,
                'exit_ids': exit_ids
            }

        return results


    def has_crossings_within_window(self, ref_time, timestamps, threshold, duration):
        """
        Returns (True, count) if at least `threshold` timestamps occur within the window [ref_time - duration, ref_time].
        """
        window_start = ref_time - duration

        # Filter timestamps within the window
        recent = [float(ts) for ts in timestamps if window_start <= float(ts) <= ref_time]
        count = len(recent)

        logger.debug(f'Window check: now={ref_time}, duration={duration}, window_start={window_start}, count={count}, threshold={threshold}')
        logger.debug(f'Timestamps in window: {recent}')

        return (count > threshold, count)

    def make_alerts(self, triggers, tripwires, crossings, alert_time):
        alerts = []

        for trigger in triggers:
            tripwire_id = trigger['tripwire_id']
            crossing = crossings.get(tripwire_id, None)
            if crossing:
                cross_key = 'entries' if trigger['trigger_direction'] == 'entry' else 'exits'
                cross_count = crossing[cross_key]

                ts_key = 'entry_timestamps' if trigger['trigger_direction'] == 'entry' else 'exit_timestamps'
                #new_timestamps = crossing.get(ts_key, [])
                threshold = float(self.get_trigger_param(trigger, 'threshold'))
                duration = float(self.get_trigger_param(trigger, 'duration'))
                new_timestamps = []
                new_ids = crossing.get('entry_ids' if trigger['trigger_direction'] == 'entry' else 'exit_ids', [])
                raw_timestamps = crossing.get(ts_key, [])


                direction = trigger['trigger_direction']


                cooldown = 3.0  # seconds
                now = alert_time

                # Prune old tracking_ids from recent_ids
                for tripwire_id in self.recent_ids:
                    self.recent_ids[tripwire_id] = {
                        tid: ts for tid, ts in self.recent_ids[tripwire_id].items()
                        if now - ts <= cooldown
                    }

                # Filter to avoid duplicate count, e.g. same person crossed slowly, ankles across multiple frames
                for ts, tid in zip(raw_timestamps, new_ids):
                    ts_norm = normalize_ts(ts)
                    last_seen = self.recent_ids[tripwire_id].get(tid)

                    # Only count if this ID hasn't been seen recently
                    if last_seen is None or now - last_seen > duration:
                        new_timestamps.append(ts)


                # Skip alert logic if no new crossings
                if not new_timestamps:
                    continue

                # Update rolling buffer
                buffer = self.recent_crossings[tripwire_id]
                buffer.extend(new_timestamps)

                logger.debug(f'New timestamps: {new_timestamps}')
                logger.debug(f'Updated buffer: {buffer}')
                logger.info(f'Crossing data: {crossing}')

                # Remove old timestamps outside the duration window
                now = alert_time #time.time()
                buffer = [ts for ts in buffer if now - normalize_ts(ts) <= duration]
                self.recent_crossings[tripwire_id] = buffer

                # Check for alert condition
                should_alert, observed_count = self.has_crossings_within_window(now, [normalize_ts(ts) for ts in buffer], threshold, duration)

                logger.info(f'{tripwire_id}: cross_count={crossing[cross_key]} observed_count={observed_count} direction={direction} threshold={threshold} duration={duration}')
                logger.debug(f'Rolling buffer for {tripwire_id}: {buffer}')


                if should_alert:
                    tripwire = tripwires[tripwire_id]
                    monitor_id = tripwire['monitor_id']
                    alert = {
                        'monitor_id': monitor_id,
                        'source_trigger': trigger,
                        'time': alert_time,
                        'crossings': {
                            'count': observed_count,
                            'duration': duration
                        }
                    }
                    alerts.append(alert)

                    logger.info(f'\n\n** ALERT: {observed_count} / {duration:.2f}\n')

        return alerts

    def parse_messages(self, messages):
        try:
             # sort messages into frame history by monitor
            for recv_time, message in messages:
                channel = message['channel'][len(DETECTION_CHANNEL_PREFIX):]
                # first element is time message arrived, second element is Redis payload containing frame metadata
                msg_obj = json.loads(message['data'])
                self._frame_metadata_by_monitor[channel].append( (recv_time, msg_obj) )
            #logger.debug(f'messages={len(messages)} frame_history_by_channel={len(frame_history_by_channel[channel])}')
        except Exception as exc:
            logger.exception(exc)

        return self._frame_metadata_by_monitor

    def apply_triggers(self, triggers, tripwires, ts_frames):

        # Sort messages by monitor
        #try:
        #    for recv_time, message in ts_frames:
        #        # first element is time message arrived, second element is Redis payload containing frame metadata
        #        frame_metadata = json.loads(message['data'])
        #        monitor = message['channel'][len(DETECTION_CHANNEL_PREFIX):] # monitor follows prefix
        #        self._frame_metadata_by_monitor[monitor].append((recv_time, frame_metadata))
        #except Exception as exc:
        #    logger.error(f'Error parsing message: {message}')
        #    logger.exception(exc)
        #    return

        crossings = {}
        for monitor, frame_metadata in self._frame_metadata_by_monitor.items():
            # send only wires for this monitor
            monitor_wires = {wire_id: wire for wire_id, wire in tripwires.items()
                             if wire['monitor_id'] == monitor}

            # calculate the window length to calculate crossings over;
            # need to overlap some number, and expect filter_len samples left over from last call
            window_len = len(frame_metadata) - self._filter_len + self._overlap_len

            # for very first call or long filter length (=negative window size), send all samples
            if window_len < 2:
                window_len = len(frame_metadata)

            logger.debug(f'Calculating crossings for {len(frame_metadata)} frames in monitor {monitor} from t={frame_metadata[0][0]} to t={frame_metadata[-1][0]}; window_len = {window_len}')

            monitor_crossings = self.calculate_crossings(frame_metadata, self._filter_len, window_len, monitor_wires)
            crossings.update(monitor_crossings) # add to overall record

            frames_to_drop = len(frame_metadata) - self._filter_len
            for _ in range(frames_to_drop):
                frame_metadata.popleft()

        last_ts = ts_frames[-1][0] # TODO: last timestamp per monitor, pass dict to make_alerts
        alerts = self.make_alerts(triggers, tripwires, crossings, last_ts)

        logger.debug(f'Filtered {len(ts_frames)} Detection messages through {len(triggers)} trigger(s), generating {len(alerts)} alert(s)')
        return alerts

    def get_trigger_param(self, trigger, name):
        for param in trigger['params']:
            if param['name'] == name:
                return param['value']

        raise ValueError(f'{name} not in params for trigger {trigger}')

    async def init(self):
        logger.info("Initializing tripwire_analytics...")
        await connect_to_db()


    def start_statistics_task(self):
        self._statistics_process_task = asyncio.create_task(statistics_task())
        return self._statistics_process_task

    def deinit(self):
        statistics_process_task = getattr(self, '_statistics_process_task', None)
        if statistics_process_task and not statistics_process_task.done():
            statistics_process_task.cancel()

        close_db_connection()

    async def run_count_query(self, r : redis.Redis, token, monitor_id, start_time, end_time):
        await run_count_query(r, token, monitor_id, start_time, end_time)

    async def run_heatmap_query(self, r : redis.Redis, token, monitor_id, start_time, end_time):
        await run_heatmap_query(r, token, monitor_id, start_time, end_time)

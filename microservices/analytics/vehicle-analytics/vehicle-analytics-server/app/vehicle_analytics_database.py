# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import os
import sys
import mysql.connector
from datetime import datetime, timezone
import redis.asyncio as redis
import asyncio
import logging
import numpy as np
import json
import vehicle_analytics_core as va_core
from vehicle_analytics_core import logger, HEATMAP_COLS, HEATMAP_ROWS


# for production
MARIADB_PASSWORD = os.environ.get('MARIADB_PASSWORD')
MARIADB_HOST = os.environ.get('MARIADB_HOST')
MARIADB_USER = os.environ.get('MARIADB_USER')

MARIADB_PORT = os.environ.get('MARIADB_PORT', 3306)
MARIADB_DB = os.environ.get('MARIADB_DB', 'iot_solutions')

HEATMAP_TABLE = os.environ.get('DEFAULT_HEATMAP_TABLE', 'heatmap_va_statistics')
COUNT_TABLE = os.environ.get('DEFAULT_COUNT_TABLE', 'count_va_statistics') # entire FOV
COUNT_REGIONS_TABLE = os.environ.get('DEFAULT_COUNT_REGIONS_TABLE', 'count_va_region_statistics') # by region

# MariaDB connector
db_connection = None

def handle_no_db_error():
       logger.info(f"No MariaDB connection!")
       # should fail like api
       sys.exit()

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
                 CREATE TABLE IF NOT EXISTS {COUNT_REGIONS_TABLE} (
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
                ON {COUNT_REGIONS_TABLE}(timestamp, region_id)
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

async def save_count_statistics(count_lock, monitor_id, stats):
    '''
    Save for all monitor_ids. Counts are reset after saving.
    '''
    if not stats:
        return

    logger.info(f'save_count_statistics {datetime.now()}')

    async with count_lock:

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

async def save_regions_count_statistics(count_lock, region_id, stats):
    '''
    Save for all regions. Counts are reset after saving.
    '''

    logger.info(f'save_count_statistics {datetime.now()}')

    async with count_lock:
        timestamp = datetime.now(timezone.utc)
        logger.info(f"Save {COUNT_REGIONS_TABLE}[{region_id}] {timestamp}")

        try:
            conn = await connect_to_db()

            cursor = conn.cursor()

            data_insert = f'''
                INSERT INTO {COUNT_REGIONS_TABLE} (timestamp, region_id, min_count, max_count, avg_count)
                    VALUES (%s, %s, %s, %s, %s)
                '''

            #logger.debug(f'{data_insert}')

            # Insert the data
            vmin = 0 if stats.count_min == float('inf') else stats.count_min
            vmax = 0 if stats.count_max == float('-inf') else stats.count_max
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


async def save_heatmap_statistics(count_lock, monitor_id, stats, visualize=False):
    '''
    Saves the heatmap to the database.
    Resets the heatmap statistics after committing.
    '''

    timestamp = datetime.now(timezone.utc)

    if not stats:
        logger.info(f'No heatmap stats to save')
        return

    async with count_lock:
        # Save for all monitor_ids

        try:
            if visualize:
                va_core.visualize_heatmap(stats.heatmap)

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

async def run_count_query(r : redis.Redis, token, monitor_id, region_id, start_time, end_time, pub_channel):
    '''
    Process a count analystics request.
    '''

    logger.info(f'run_count_query {datetime.fromtimestamp(start_time)} -> {datetime.fromtimestamp(end_time)}')

    try:
        # Save the current count statistics in case it needs to be included
        #await save_count_statistics()

        conn = await connect_to_db()

        cursor = conn.cursor()

        # Query the database for entries within the timestamp range.
        # The max_count is the max count across all returned rows,
        # the min_count is the min_count acrosss all returned row,
        # and the average_count is the average of all the averages across returned rows.

        if region_id:
            logger.info(f'Region Count Request - {region_id}')
            query = f'''
                SELECT
                    MAX(max_count) AS max_of_max_counts,
                    MIN(min_count) AS min_of_min_counts,
                    AVG(avg_count) AS avg_of_avg_counts
                FROM {COUNT_REGIONS_TABLE}
                WHERE region_id = %s
                    AND timestamp BETWEEN %s AND %s
                '''

            cursor.execute(query, (region_id, datetime.fromtimestamp(start_time, timezone.utc), datetime.fromtimestamp(end_time, timezone.utc)))
        else:
            logger.info(f'FOV Count Request - {monitor_id}')
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
        asyncio.ensure_future(r.publish(pub_channel, json.dumps(result)))

    except Exception as e:
        logger.error(f"An error occurred: {e}")


async def run_heatmap_query(r : redis.Redis, token, monitor_id, start_time, end_time, pub_channel):
    '''
    Process a heatmap analytics request.
    '''

    logger.info(f'run_heatmap_query {datetime.fromtimestamp(start_time)} -> {datetime.fromtimestamp(end_time)}')

    try:
        #await save_heatmap_statistics(visualize=True)

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
            va_core.visualize_heatmap(sum_array)
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
        asyncio.ensure_future(r.publish(pub_channel, json.dumps(result)))

    except Exception as e:
        logger.error(f"An error occurred: {e}")
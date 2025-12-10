# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import signal
import redis.asyncio as redis
import asyncio
import os
import logging
import time
import sys
import json
from mysql.connector.constants import _obsolete_option
from types import SimpleNamespace
from tripwire_analytics import TripwireAnalytics, TRIPWIRE_KEY, TRIGGER_KEY, ANALYTICS_CHANNEL, DETECTION_CHANNEL_PREFIX

REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
logger = logging.getLogger(__file__)
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))

REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))

message_list = [] # list of (loop.time(), message)

async def connect_to_redis(r):
    logger.info(f'Pinging redis at {REDIS_HOST}:{REDIS_PORT}..')
    ping_retval = await r.ping()
    logger.info(f'Ping: {ping_retval}')


async def register_pubsub_listener(r: redis.Redis, ta: TripwireAnalytics):

    def detection_message_handler(message):
        # called by the pubsub async runner when a message is received
        global message_list

        now = time.time()
        if message is not None:
            ta.enqueue_message(now, message)

            # create a readable log with first & last SNIP_SIZE characters of message
            data = message["data"]
            data_len = len(data)
            SNIP_SIZE = 20
            data_str = str(data) if data_len < (2*SNIP_SIZE+2) else f'{str(data)[:SNIP_SIZE]}..{str(data)[-SNIP_SIZE:]} ({data_len} bytes)'
            logger.debug(f'Received message on ch "{message["channel"]}" >> "{data_str}"')
        else:
            logger.debug('No message from get_message')

    def tripwires_update_handler(message):
        logger.info(f'Subscribed to region updates on key: {TRIPWIRE_KEY}')
        if message['type'] == 'pmessage':
            logger.info(f'Tripwires update detected: {message}')
            asyncio.create_task(ta.update_tripwires())

    def trigger_update_handler(message):
        logger.info(f'Subscribed to triggers updates on key: {TRIGGER_KEY}')
        if message['type'] == 'pmessage':
            logger.info(f'Triggers update detected: {message}')
            asyncio.create_task(ta.update_triggers())

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
                    asyncio.create_task(ta.run_count_query(r, request.sync_id, request.monitor_id, request.from_time, request.to_time))
                elif( request.analytics_type == 'heatmap'):
                    asyncio.create_task(ta.run_heatmap_query(r, request.sync_id, request.monitor_id, request.from_time, request.to_time))
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
    await pubsub.psubscribe(**{'__keyspace@0__:' + TRIPWIRE_KEY: tripwires_update_handler})
    await pubsub.psubscribe(**{'__keyspace@0__:' + TRIGGER_KEY: trigger_update_handler})

    await pubsub.subscribe(**{ANALYTICS_CHANNEL: analytics_request_handler})
    logger.debug(f'subscribed to {ANALYTICS_CHANNEL} channel')

    return pubsub


async def async_main():
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    await r.config_set('notify-keyspace-events', 'KEA')
    logger.info(f'{await r.config_get("notify-keyspace-events")}')
    ta = TripwireAnalytics(r)
    channel_listener_task = None
    statistics_process_task = None

    try:
        await connect_to_redis(r)

        def quit_handler ():
            if channel_listener_task == None:
                raise RuntimeError('Got SIGTERM but no task to cancel!')
            logging.info('Got SIGTERM, cancelling listener task..')
            channel_listener_task.cancel()

            ta.deInit()

        pubsub = await register_pubsub_listener(r, ta)
        loop = asyncio.get_event_loop()
        loop.add_signal_handler(signal.SIGTERM, quit_handler)

        await ta.init()
        statistics_process_task = ta.start_statistics_task()

        channel_listener_task = asyncio.create_task(pubsub.run()) # runs forever until cancelled

        await channel_listener_task
        await statistics_process_task

    except asyncio.CancelledError:
        logger.info('Got cancelled exception, shutting down..')
    finally:
        logger.info('Closing redis client..')
        await r.aclose()


if __name__ == "__main__":

    # logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(filename)s::%(funcName)s %(message)s')
    logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(message)s')
    asyncio.run(async_main())
    logging.info('Exiting server.')
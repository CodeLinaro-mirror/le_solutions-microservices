# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import signal
import redis.asyncio as redis
import asyncio
import os
import logging
import time

from tripwire_analytics import TripwireAnalytics

REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.rz') + ':'
# e.g., monitor 0 would be "detection.rz:0"

message_list = [] # list of (loop.time(), message)

logger = logging.getLogger(__file__)


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

    # register subscribe pattern handler, return pubsub to be used in caller for .run()
    pubsub = r.pubsub()
    channel_pattern = DETECTION_CHANNEL_PREFIX + '*'
    await pubsub.psubscribe(**{channel_pattern: detection_message_handler})
    return pubsub


async def async_main():
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    ta = TripwireAnalytics(r)
    channel_listener_task = None

    try:
        await connect_to_redis(r)

        def quit_handler ():
            if channel_listener_task == None:
                raise RuntimeError('Got SIGTERM but no task to cancel!')
            logging.info('Got SIGTERM, cancelling listener task..')
            channel_listener_task.cancel()

        pubsub = await register_pubsub_listener(r, ta)
        loop = asyncio.get_event_loop()
        loop.add_signal_handler(signal.SIGTERM, quit_handler)
        channel_listener_task = asyncio.create_task(pubsub.run()) # runs forever until cancelled
        await channel_listener_task

    except asyncio.CancelledError:
        logger.info('Got cancelled exception, shutting down..')
    finally:
        logger.info('Closing redis client..')
        await r.aclose()


if __name__ == "__main__":

    # logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(filename)s::%(funcName)s %(message)s')    
    logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(message)s')
    asyncio.run(async_main())

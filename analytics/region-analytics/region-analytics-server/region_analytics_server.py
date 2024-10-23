# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
from typing import Dict
import redis.asyncio as redis
import asyncio
import os
import json
from types import SimpleNamespace
import logging
import time
from collections import defaultdict

from region_algo import RegionHistory

REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'RAAlerts')
REGION_KEY = os.environ.get('TRIGGER_KEY', 'RARegions')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'RATriggers')
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))
CHANNEL_PREIX = os.environ.get('CHANNEL_PREFIX', 'Detection::YoloV8::RZ::')

TIME_DRIFT_LIMIT_SECS = 3.0 # TODO: pull from environment within loop
seconds_offsets_by_channel : dict[str, float]= {}


# meanings of elements in the ObjectDetection "rectangle" member
RECT_IDX_TOP = 0
RECT_IDX_LEFT = 1
RECT_IDX_BOTTOM = 2
RECT_IDX_RIGHT = 3

message_list = [] # list of (loop.time(), message)
logger = logging.getLogger(__file__)
evt_stop = asyncio.Event()

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

async def get_regions(r):
    # Get regins from Redis
    raw_regions = await r.hgetall(REGION_KEY)
    logger.debug(f'Raw regions from redis: {raw_regions}')

    # redis HGETALL returns in format {k : v} where k = region_id, v = full region in JSON string
    # Decode region JSON info
    regions = {id: json.loads(region_str) for id, region_str in raw_regions.items()}

    # TODO: remove below code when Redis payload has json-decoded inner objects
    # JSON-decode any inner objects
    for region in regions.values():
        region['coordinates'] = json.loads(region['coordinates'])
    
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
    supported_trigger_conditions = ['occupancy_changed']
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

region_histories : Dict[str, RegionHistory] = {}
last_trigger_occupancy : Dict[str, int] = {}

def make_alert(trigger, region, region_history : RegionHistory, alert_time):
    alert = {
        'monitor_id': region['monitor_id'],
        'source_trigger': trigger,
        'time': alert_time,
        'occupants': region_history.get_occupant_data_for_alert()
    }

    return alert

def apply_triggers(triggers, regions, messages):
    messages_by_monitor = defaultdict(list) # list of messages by monitor, in received order
    try:
        for recv_time, message in messages:
            # first element is time message arrived, second element is Redis payload
            msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))
            monitor = message['channel'][len(CHANNEL_PREIX):] # monitor follows prefix
            messages_by_monitor[monitor].append((recv_time, msg_obj))
    except Exception as exc:
        logger.error(f'Error parsing message: {message}')
        logger.exception(exc)
        return

    alerts = []
    for region in regions.values():
        # get existing region history, or create a new one
        region_id = region['region_id']
        monitor_id = region['monitor_id']
        messages = messages_by_monitor[monitor_id]
        if len(messages) == 0:
            continue # no messages on this monitor

        if region_id not in region_histories:
            # first time seeing this region, create the history object
            region_histories[region_id] = RegionHistory(polygon=region['coordinates'])
        rh = region_histories[region_id]
        # TODO: delete stale regions histories?

        # get timestamp of last message in epoch time
        last_recv_time, last_message = messages[-1]
        alert_time = convert_msg_timestamp_to_epoch_time(
            last_message.Parameters.timestamp,
            recv_time,
            monitor_id
            )

        # Format message data into algo's format, skipping messages that would pop
        # out of lookback
        records_by_frame = []
        for _, message in messages[-rh.get_max_lookback():]:
            records = {}
            frame_objects = message.ObjectDetection
            last_idx = len(frame_objects) - 1
            for idx, person in enumerate(frame_objects):
                if not person.label.startswith('person'):
                    continue # top-level non-person object
                id = person.id
                if not hasattr(person, 'landmarks'):
                    continue # this person has no feet!
                if idx == last_idx or frame_objects[idx+1].label != 'feet':
                    continue # need 'feet' to be the next object
                
                feet_bb = frame_objects[idx+1]
                landmarks = person.landmarks
                landmarks.middle_point = vars(landmarks)['feet-middle-point']
                rectangle = person.rectangle
                bounding_box = {
                    'top_left': {
                        'x': rectangle[RECT_IDX_LEFT],
                        'y': rectangle[RECT_IDX_TOP]
                    },
                    'bottom_right': {
                        'x': rectangle[RECT_IDX_RIGHT],
                        'y': rectangle[RECT_IDX_BOTTOM]
                    }
                }
                records[id] = {
                    'id': person.id,
                    'x': feet_bb.rectangle[0], # instead of incorrect landmark x
                    'y': feet_bb.rectangle[1],
                    'bounding_box': bounding_box,
                }
            records_by_frame.append(records)
        
        # Add all messages to this region's history, returns current # of occupants
        current_occupancy = rh.add_detections(records_by_frame)
        logging.debug(f'Region {region_id} occupants: {rh._current_occupants}, frame history buffer: {rh._frame_history}')

        for trigger in [t for t in triggers if t['region_id'] == region_id]:
            trigger_id = trigger['trigger_id']
            last_occupancy = last_trigger_occupancy.get(trigger_id, 0)
            if last_occupancy != current_occupancy:
                alerts.append(make_alert(trigger, region, rh, alert_time))
            last_trigger_occupancy[trigger_id] = current_occupancy
    
    logger.info(f'Filtered {len(messages)} Detection messages through {len(triggers)} trigger(s), generating {len(alerts)} alert(s)')
    return alerts


async def check_for_alerts(r : redis.Redis):
    try:
        global message_list
        if len(message_list) == 0:
            return # no messages
        
        current_messages = message_list
        message_list = [] # empty the message list

        regions = await get_regions(r)
        triggers = await get_triggers(r)
        alerts = apply_triggers(triggers, regions, current_messages)
    
        for alert in alerts:
            logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
            # TODO: add this to task list instead of awaiting?
            await r.publish(ALERT_CHANNEL, json.dumps(alert))

    except Exception as exc:
        logger.error('Exception in check_for_alerts:')
        logger.exception(exc)
        logger.error('Database or received messages likely corrupted, will keep trying..')

async def listen_to_channel(r: redis.Redis, group: asyncio.TaskGroup):

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
            nonlocal group
            group.create_task(check_for_alerts(r))

    # register subscribe pattern handler, add task to run pubsub
    pubsub = r.pubsub()
    channel_pattern = CHANNEL_PREIX + '*'
    await pubsub.psubscribe(**{channel_pattern: detection_message_handler})
    group.create_task(pubsub.run()) # TODO: how to end gracefully on SIGINT / SIGTERM?


async def async_main():
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    try:
        await connect_to_redis(r)
        async with asyncio.TaskGroup() as group:
            # TODO: handle SIGTERM gracefully
            # TODO: handle keyboard interrupt gracefully
            group.create_task(listen_to_channel(r, group))

    finally:
        logger.info('Closing redis client..')
        await r.aclose()

if __name__ == "__main__":

    # logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(filename)s::%(funcName)s %(message)s')    
    logging.basicConfig(level=LOG_LEVEL, format='%(asctime)s %(levelname)s %(message)s')
    asyncio.run(async_main())

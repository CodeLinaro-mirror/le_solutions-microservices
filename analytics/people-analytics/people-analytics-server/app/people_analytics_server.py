# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from collections import deque
from typing import Dict
import redis.asyncio as redis
import asyncio
import os
import json
from types import SimpleNamespace
import logging
import time
import signal

REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
REDIS_PORT = os.environ.get('REDIS_PORT', 6379)
ALERT_PERIOD = os.environ.get('ALERT_PERIOD', 1.0)
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'people-analytics.alerts')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'PATriggers')
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))
LOOKBACK_FRAMES = int(os.environ.get('LOOKBACK_FRAMES', 5))
DETECTION_CHANNEL_PREFIX = os.environ.get('REDIS_DETECTION_CHANNEL_PREFIX', 'detection.ppe') + ':'
# e.g., monitor 0 channel would be "detection.ppe:0"

TIME_DRIFT_LIMIT_SECS = 3.0 # TODO: pull from environment within loop
seconds_offsets_by_channel : dict[str, float]= {}

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


async def get_triggers(r):
    # Get triggers from Redis
    raw_triggers = await r.hgetall(TRIGGER_KEY)
    logger.debug(f'Raw triggers from redis: {raw_triggers}')

    # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
    # Convert to list of 
    trigger_objs = [json.loads(trigger) for trigger in raw_triggers.values()]
    supported_trigger_conditions = ['required_accessories', 'restricted_accessories']

    triggers = []
    for trigger in trigger_objs:
        if trigger['trigger_condition'] not in supported_trigger_conditions:
            logger.warning(f'Ignoring trigger with unsupported condition: "{trigger}"')
            continue
        triggers.append(trigger)
    
    if triggers == []:
        logger.info(f'No triggers found at {TRIGGER_KEY}; adding default')
        triggers = [
             {
                "monitor_id": "0",
                "trigger_id": "0xDEADBEEF",
                "trigger_name": "Person missing vest (hard-coded)",
                "trigger_condition": "required_accessories",
                "params": '[{"name":"accessories","value":"vest"}]' # needs to be re-parsed
            }
        ]
    
    # TODO: remove below code when Redis payload has json-decoded inner objects
    # JSON-decode any inner objects
    for trigger in triggers:
        trigger['params'] = json.loads(trigger['params'])
    
    logger.debug(f'Parsed & validated triggers: {triggers}')
    return triggers 

def convert_msg_timestamp_to_epoch_time(msg_ts, sys_time, channel=None):
    return sys_time # hack to stop drift

frame_history_by_channel : Dict[str, deque] = {}

def apply_triggers(triggers, messages):
    alerts = []
    trigger = None
    message = None # set these for logging in exception handler
    try:
        # sort messages into frame history by monitor
        for recv_time, message in messages:
            channel = message['channel']
            if channel not in frame_history_by_channel:
                frame_history_by_channel[channel] = deque(maxlen=LOOKBACK_FRAMES)
                # TODO: delete stale histories?

            # TODO: optimize by only json-decoding the messages that remain in frame history?
            msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))
            msg_time = convert_msg_timestamp_to_epoch_time(
                msg_obj.parameters.timestamp,
                recv_time,
                channel)
            frame_history_by_channel[channel].append( (msg_time, msg_obj) )
            
        for trigger in triggers:
            id = trigger['monitor_id']
            trigger_channel = DETECTION_CHANNEL_PREFIX + id
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
                case _:
                    raise RuntimeError(f'Unexpected trigger condition \'{trigger_condition}\' in trigger {trigger}')

            # look for violators in the history buffer
            recent_frames = frame_history_by_channel[trigger_channel]

            # If not enough history in buffer (beginning of stream), no alert
            if len(recent_frames) < LOOKBACK_FRAMES:
                continue

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
            msg_time, _ = recent_frames[-1]
            alerts.append(
                {
                    'source_trigger': trigger,
                    'time': msg_time,
                    'causes': causes
                }
            )
        logger.info(f'Filtered {len(messages)} Detection messages through {len(triggers)} trigger(s), generating {len(alerts)} alert(s)')
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

    triggers = await get_triggers(r)
    alerts = apply_triggers(triggers, current_messages)
    
    for alert in alerts:
        logger.debug(f'Publishing to channel {ALERT_CHANNEL}: {alert}')
        asyncio.ensure_future(r.publish(ALERT_CHANNEL, json.dumps(alert)))

async def register_pubsub_listener(r: redis.Redis):

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

    # register subscribe pattern handler, return pubsub to be used in caller for .run()
    pubsub = r.pubsub()
    await pubsub.psubscribe(**{DETECTION_CHANNEL_PREFIX + '*': detection_message_handler})
    return pubsub

async def async_main():
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, decode_responses=True)
    channel_listener_task = None

    try:
        await connect_to_redis(r)

        def quit_handler ():
            if channel_listener_task == None:
                raise RuntimeError('Got SIGTERM but no task to cancel!')
            logging.info('Got SIGTERM, cancelling listener task..')
            channel_listener_task.cancel()

        pubsub = await register_pubsub_listener(r)
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
    logging.basicConfig(level=LOG_LEVEL, format='%(levelname)s %(message)s')
    asyncio.run(async_main())
    logging.info('Exiting server.')

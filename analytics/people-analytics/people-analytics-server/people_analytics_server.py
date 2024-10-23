# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

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
ALERT_CHANNEL = os.environ.get('ALERT_CHANNEL', 'PAAlerts')
TRIGGER_KEY = os.environ.get('TRIGGER_KEY', 'PATriggers')
LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))

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


async def get_triggers(r):
    # Get triggers from Redis
    raw_triggers = await r.hgetall(TRIGGER_KEY)
    logger.debug(f'Raw triggers from redis: {raw_triggers}')

    # redis HGETALL returns in format {k : v} where k = trigger_id, v = full trigger in JSON string
    # Convert to list of 
    triggers = [json.loads(trigger) for trigger in raw_triggers.values()]
    supported_trigger_conditions = ['required_accessories', 'restricted_accessories']

    for trigger in triggers:
        if trigger not in supported_trigger_conditions:
            logger.warning(f'Ignoring trigger with unsupported condition: "{trigger}"')

    triggers = [t for t in triggers if t['trigger_condition'] in supported_trigger_conditions]

    
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


def apply_triggers(triggers, messages):
    alerts = []
    trigger = None
    message = None # set these for logging in exception handler
    try:
        for trigger in triggers:
            id = trigger['monitor_id']

            trigger_channel = f'Detection::YoloV8::PPE::{id}'
            trigger_accessories = None
            for param in trigger['params']:
                if param['name'] == 'accessories':
                    trigger_accessories = param['value'].split(',')
                    trigger_accessories = [a.strip() for a in trigger_accessories] # strip all whitespace
                    break
            
            match trigger['trigger_condition']:
                case 'required_accessories':
                    def accessory_violations(detected_accessories):
                        return sorted(set(trigger_accessories) - set(detected_accessories))
                case 'restricted_accessories':
                    def accessory_violations(detected_accessories):
                        return sorted(set(trigger_accessories) & set(detected_accessories))
                # No "none" case needed, as only supported triggers are in this list

            # TODO: smooth the messages? For now, only look at last message
            for recv_time, message in [messages[-1]]:
                # note: first element is the loop timestamp when subscriber got message,
                # message also contains a timestamp from the source
                if message['channel'] == trigger_channel:
                    # this is the last message matching this channel
                    msg_obj = json.loads(message['data'], object_hook=lambda d: SimpleNamespace(**d))

                    msg_time = convert_msg_timestamp_to_epoch_time(
                        msg_obj.Parameters.timestamp,
                        recv_time,
                        trigger_channel)

                    causes = []

                    for person in msg_obj.ObjectDetection:
                        # skip non-person to-level objects
                        if person.label != 'person' and not person.label.startswith('person'):
                            continue

                        # check for sub-objects aka accessories; if element missing, set it to empty list
                        sub_objects = person.ObjectDetection if hasattr(person, 'ObjectDetection') else []

                        # extract labels -- these are the accessories
                        accessories = [so.label for so in sub_objects]                    

                        # check if all required accessoried are present
                        violations = accessory_violations(accessories)
                        if not violations:
                            continue

                        # some required accessories not there
                        causes.append({
                            'accessories': ', '.join(violations),
                            'violator': {
                                'top_left': {
                                    'x': person.rectangle[RECT_IDX_LEFT],
                                    'y': person.rectangle[RECT_IDX_TOP]
                                },
                                'bottom_right': {
                                    'x': person.rectangle[RECT_IDX_RIGHT],
                                    'y': person.rectangle[RECT_IDX_BOTTOM]
                                }                                
                            }
                        })
                    
                    if causes == []:
                        continue # no alerts triggered

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


async def check_for_alerts(r : redis.Redis, group : asyncio.TaskGroup):
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
        group.create_task(r.publish(ALERT_CHANNEL, json.dumps(alert)))

async def register_pubsub_listener(r: redis.Redis, group: asyncio.TaskGroup):

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
            nonlocal group
            group.create_task(check_for_alerts(r, group))

    # register subscribe pattern handler, return pubsub to be used in caller for .run()
    pubsub = r.pubsub()
    await pubsub.psubscribe(**{'Detection::YoloV8::PPE::*': detection_message_handler})
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

        async with asyncio.TaskGroup() as group:
            pubsub = await register_pubsub_listener(r, group)
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
    logging.info('Exiting server.')

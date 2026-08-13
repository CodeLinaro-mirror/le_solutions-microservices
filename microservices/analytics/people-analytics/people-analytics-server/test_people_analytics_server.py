# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import json
import pytest

import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'app')))
import people_analytics_server as pas

# PAS defaults, reset for each test
DEFAULT_TIME_DRIFT_LIMIT = 3.0

@pytest.fixture(scope='function', autouse=True)
def set_pas_context_to_default():
    pas.TIME_DRIFT_LIMIT_SECS = DEFAULT_TIME_DRIFT_LIMIT
    pas.seconds_offsets_by_channel = {}
    pas.LOOKBACK_FRAMES=1
    pas.frame_history_by_channel = {}

@pytest.fixture
async def triggers():
    class FakeRedis:
        async def hgetall(self, *args):
            return {}
    return await pas.get_triggers(FakeRedis())

async def test_get_triggers_returns_fake_trigger(triggers: Any):
    assert len(triggers) == 1, "Expect one hard-coded trigger with faked Redis"

def make_message(type : str):
    MESSAGE_TIMESTAMP = "1234567890" # in nanoseconds; 1.234567890 in seconds
    frame_message = {
                'object_detection': [
                    {
                        'label': 'person_1',
                        'rectangle': {
                            # Make x0, y0, x1, y1 = 0.1, 0.2, 0.3, 0.4
                            'x': 0.1,
                            'y': 0.2,
                            'width': (0.3 - 0.1),
                            'height': (0.4 - 0.2)
                        }
                        
                    }
                ],
                'parameters': {
                    'timestamp': MESSAGE_TIMESTAMP # source timestamp from monitor in nsec
                }
            }

    if type == 'one_person_no_accessories':
        pass # all set up above

    elif type == 'nobody_and_nothing':
        del(frame_message['object_detection']) # no object_detection object in case of no people

    elif type == 'only_vest_in_room':
        frame_message['object_detection'][0]['label'] = 'vest'

    elif type == 'person_with_vest_in_frame_but_not_worn':
        # create new top-level object which is top level vest only
        frame_message['object_detection'].append(
            {
                'label': 'vest' # top-level vest, e.g. placed on a chair
            }
        )
    
    elif type == 'one_person_with_vest':
        frame_message['object_detection'][0]['object_detection'] = [{'label': 'vest'}]
    elif type == 'one_person_with_cellphone':
        frame_message['object_detection'][0]['object_detection'] = [{'label': 'cellphone'}]
    elif type == 'one_person_with_vest_with_extra_whitespace':
        frame_message['object_detection'][0]['object_detection'] = [{'label': '  vest  '}]
        
    return frame_message


def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (loop_timestamp, message)
        (0, # loop timestamp
            {
                'channel': 'detection.ppe:' + channel,
                'data': json.dumps(message)
            }
        )
        for message in messages
    ]
    return message_list

async def test_one_person_no_accessories_generates_one_alert(triggers: Any):
    message_list = make_message_list(
        # Note: see "make_message" for the defaults which are checked below
        [make_message('one_person_no_accessories')]                
    )
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1
    alert = alerts[0]
    assert alert['source_trigger'] == triggers[0], 'Expected the test trigger to be the source_trigger'
    # TODO: uncomment this if not compensating for system time
    # assert alert['time'] == 1.234567890, 'Expected the message timestamp converted from ns to sec'
    causes = alert['causes']
    assert len(causes) == 1, 'Should have one person causing trigger'
    cause = causes[0]
    assert cause['accessories'] == 'vest'
    assert cause['violator']['top_left']['x'] == 0.1
    assert cause['violator']['top_left']['y'] == 0.2
    assert cause['violator']['bottom_right']['x'] == 0.3 
    assert cause['violator']['bottom_right']['y'] == 0.4


async def test_one_message_all_accessories_present_generates_no_alerts(triggers: Any):
    message_list = make_message_list(
        [make_message('one_person_with_vest')]
    )
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts because all accessories present; alert {alerts[0]}'

async def test_missing_accessory_on_other_channel_generates_no_alerts(triggers: Any):
    message_list = make_message_list(
        [make_message('one_person_no_accessories')], channel='other'
    )
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts because violation was on other channel; alert {alerts[0]}'


async def test_top_level_vest_without_person_triggers_alert(triggers: Any):
    message_list = make_message_list(
        [make_message('person_with_vest_in_frame_but_not_worn')]
    )
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1, f'Expected alert because vest in room but not on person'

async def test_vest_alone_with_no_people_in_room_has_no_alert(triggers: Any):
    message_list = make_message_list(
        [make_message('only_vest_in_room')]
    )
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts; vest in room but no person'


async def test_wrong_accessory_causes_alert(triggers: Any):
    msg = make_message('one_person_with_vest')
    msg['object_detection'][0]['object_detection'][0]['label'] = 'skateboard' # not a vest
    message_list = make_message_list([msg])
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1, f'Expected alert because no vest, only skateboard'


async def test_two_messages_only_looks_at_second_for_alert(triggers: Any):
    message_list = make_message_list([
        make_message('one_person_no_accessories'),
        make_message('one_person_with_vest')
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected only last message to be viewed and no alert, alert: {alerts[0]}'

async def test_message_with_no_people_gives_no_alert(triggers: Any, caplog: pytest.LogCaptureFixture):
    message_list = make_message_list([
        make_message('nobody_and_nothing'),
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts when no objects in view, alert: {alerts[0]}'
    assert len(caplog.records) == 0, 'Unexpected log message from person-less frame'


async def test_can_register_restricted_accessories_from_redis(caplog: pytest.LogCaptureFixture):
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "Person with cellphone",
                        "trigger_condition": "restricted_accessories",
                        "params": [{"name":"accessories","value":"cellphone"}]
                    }
                )
            }    
    triggers = await pas.get_triggers(FakeRedis())
    assert len(caplog.records) == 0, 'Expect no errors from restricted_accessories'
    assert len(triggers) == 1
    assert triggers[0]['trigger_condition'] == 'restricted_accessories'


async def test_trigger_accessory_whitespace_is_stripped():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "Person missing vest or hardhat",
                        "trigger_condition": "required_accessories",
                        # "accessories" value has extra spaces the alert should strip
                        "params": [{"name":"accessories","value":"    vest  ,  hardhat   "}]
                    }
                )
            }
    triggers = await pas.get_triggers(FakeRedis())
    message_list = make_message_list([
        make_message('one_person_no_accessories'),
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    causes = alerts[0]['causes']
    assert causes[0]['accessories'] == 'hardhat, vest', 'Expected spaces to be removed from required accessories'


async def test_redis_message_accessory_whitespace_is_stripped():
    message_list = make_message_list([
        make_message('one_person_with_vest_with_extra_whitespace')
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected only last message to be viewed and no alert, alert: {alerts[0]}'


async def test_malformed_message_logs_error_and_returns_no_alerts(triggers: Any, caplog: pytest.LogCaptureFixture):
    message_list = make_message_list([
        { 'mlmeta': [ { 'name': 'this is not the schema we agreed to!!' } ] }
    ])
    try:
        alerts =  pas.apply_triggers(triggers, message_list)
    except:
        assert False, f'Exception not caught in apply_triggers; was a "raise(exc)" left in?'
    
    assert len(caplog.records) == 2, 'Expected one error log followed by an exception log'
    errlog = caplog.records[0]
    assert errlog.levelname == 'ERROR'
    assert 'not the schema we agreed to' in errlog.getMessage()

    assert len(alerts) == 0, f'Expected no alerts with a parsing error, alert: {alerts[0]}'


async def test_restricted_accessories_works():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "Person has cellphone",
                        "trigger_condition": "restricted_accessories",
                        "params": [{"name":"accessories","value":"cellphone"}]
                    }
                )
            }
    triggers = await pas.get_triggers(FakeRedis())
    message_list = make_message_list([
        make_message('one_person_with_cellphone'),
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    assert alerts[0]['source_trigger']['trigger_name'] == "Person has cellphone"
    causes = alerts[0]['causes']
    assert causes[0]['accessories'] == 'cellphone', 'Expected alert due to restricted cellphone'
        

async def test_unknown_trigger_condition_fails_correctly(caplog: pytest.LogCaptureFixture):
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                '12345': json.dumps(
                    {
                        "monitor_id": "0",
                        "trigger_id": "0xDEADBEEF",
                        "trigger_name": "Person has cellphone",
                        "trigger_condition": "fake condition",
                        "params": [{"name":"accessories","value":"cellphone"}]
                    }
                )
            }
    triggers = await pas.get_triggers(FakeRedis())
    message_list = make_message_list([
        make_message('one_person_with_cellphone'),
    ])
    alerts =  pas.apply_triggers(triggers, message_list)
    
    assert len(caplog.records) == 1, 'Expected one warning log'
    log = caplog.records[0]
    assert log.levelname == 'WARNING'
    assert 'Ignoring trigger with unsupported condition' in log.getMessage()
    assert 'fake condition' in log.getMessage()


def test_no_alerts_if_lookback_buffer_if_room_in_lookback_buffer(triggers: Any):
    pas.LOOKBACK_FRAMES=5
    message_list = make_message_list([make_message('one_person_no_accessories')] * 4)
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Expected no alerts if only 4 frames of violations: {alerts[0]}'


def test_multiple_frames_needed_for_trigger(triggers: Any):
    pas.LOOKBACK_FRAMES=5
    message_list = make_message_list([make_message('one_person_no_accessories')] * 5)
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 1, f'Should have one alert with 5 violation frames'

def test_message_on_different_channel_ignored(triggers: Any, caplog: pytest.LogCaptureFixture):
    message_list = make_message_list([make_message('one_person_no_accessories')], channel="not 0")
    alerts =  pas.apply_triggers(triggers, message_list)
    assert len(alerts) == 0, f'Should be no alerts because message was for different channel'
    assert len(caplog.records) == 0, 'Should be no logged errors'


# Time conversion tests

# TODO: when real time comes from camera, can remove the sys time override
def test_msg_and_system_time_same():
    msg_time = 111.123456789
    msg_ts = str(msg_time * 1e9) # timestamp a string of nanoseconds since 0-time
    sys_time = 987654321

    assert sys_time == pas.convert_msg_timestamp_to_epoch_time(
        msg_ts=msg_ts, sys_time=sys_time), 'Function just takes system time and ignores msg_ts'

# TODO: channels clean up stats if no messages received for some amount of time?

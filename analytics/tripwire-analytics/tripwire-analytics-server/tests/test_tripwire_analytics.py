# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import json
import pytest
import logging
from app.tripwire_analytics_server import TripwireAnalytics
from app.tripwire_crossings import Tripwire


# constants used in tests
FILTER_LEN = 2

@pytest.fixture
def ta():
    class FakeRedis:
        pass
    return TripwireAnalytics(FakeRedis(), filter_len=FILTER_LEN)

async def test_get_tripwires_returns_fake_tripwire():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                "42": "{ \"monitor_id\": \"0\",  \"tripwire_id\": \"42\", \"tripwire_name\": \"Entrance to lobby\", \"wire\": [ { \"x\": 0.1, \"y\": 0.6 }, { \"x\": 0.4, \"y\": 0.7 }, { \"x\": 0.7, \"y\": 0.65 }, { \"x\": 0.9, \"y\": 0.9 } ], \"direction\": { \"entry\": { \"x\": 0.5, \"y\": 0.7 },  \"exit\": { \"x\": 0.5, \"y\": 0.9 } } }"
            }
    
    ta = TripwireAnalytics(FakeRedis())
    
    tripwires = await ta.get_tripwires()

    assert tripwires['42']['direction'] == { 'entry': {'x': 0.5, 'y': 0.7}, 'exit': {'x': 0.5, 'y': 0.9} }
    assert len(tripwires['42']['wire']) == 4


async def test_get_triggers_returns_fake_trigger():
    class FakeRedis:
        async def hgetall(self, *args):
            return {
                "84": "{  \"tripwire_id\": \"{unique-tripwire-id}\",  \"trigger_id\": \"84\",  \"trigger_name\": \"More than 2 ppl/sec entering\",  \"trigger_direction\": \"entry\",  \"trigger_condition\": \"flowrate\",  \"params\": [    {      \"name\": \"threshold\",      \"value\": \"2\"    },    {      \"name\": \"duration\",      \"value\": \"1\"    }  ]}"
            }
    
    ta = TripwireAnalytics(FakeRedis())
    triggers = await ta.get_triggers()

    t=triggers[0]

    assert t['trigger_id'] == '84'
    assert t['tripwire_id'] == '{unique-tripwire-id}'
    assert t['trigger_name'] == 'More than 2 ppl/sec entering'
    assert t['trigger_condition'] == 'flowrate'


def make_message(type : str):
    redis_message_from_aug_19_build = '''
        {\"object_detection\":[
            {\"tracking_id\":2,\"label\":\"person_2\",\"confidence\":87.233505249023438,\"color\":4278190335,
                \"rectangle\":[0.26250000000000001,0.29999999999999999,0.09947916666666666,0.53240740740740744],
                \"landmarks\":{
                    \"feet-middle-point\":{\"x\":0.41361256544502617,\"y\":0.84869565217391307}},
                    \"object_detection\":[{\"label\":\"safety vest\",\"confidence\":93.361564636230469,\"color\":16711935,\"rectangle\":[0.29375000000000001,0.37314814814814817,0.061979166666666669,0.16111111111111112]},{\"label\":\"helmet\",\"confidence\":93.097320556640625,\"color\":16711935,\"rectangle\":[0.31510416666666669,0.29999999999999999,0.04010416666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.30156250000000001,0.74907407407407411,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":3,\"label\":\"person_3\",\"confidence\":80.210472106933594,\"color\":4278190335,\"rectangle\":[0.34999999999999998,0.29999999999999999,0.086979166666666663,0.53240740740740744],\"landmarks\":{\"feet-middle-point\":{\"x\":0.32934131736526945,\"y\":0.84173913043478266}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":94.599288940429688,\"color\":16711935,\"rectangle\":[0.35052083333333334,0.38703703703703701,0.071354166666666663,0.16296296296296298]},{\"label\":\"helmet\",\"confidence\":91.813453674316406,\"color\":16711935,\"rectangle\":[0.36822916666666666,0.31296296296296294,0.03229166666666667,0.075925925925925924]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":4278190335,\"rectangle\":[0.36197916666666669,0.73796296296296293,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":4,\"label\":\"person_4\",\"confidence\":90.560211181640625,\"color\":16711935,\"rectangle\":[0.42499999999999999,0.36666666666666664,0.086979166666666663,0.44351851851851853],\"landmarks\":{\"feet-middle-point\":{\"x\":0.42514970059880242,\"y\":0.85803757828810023}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":94.571319580078125,\"color\":16711935,\"rectangle\":[0.43906250000000002,0.43333333333333335,0.066145833333333334,0.1648148148148148]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.45989583333333334,0.75277777777777777,0.0041666666666666666,0.0055555555555555558]},{\"tracking_id\":1,\"label\":\"person_1\",\"confidence\":93.14764404296875,\"color\":16711935,\"rectangle\":[0.50624999999999998,0.25555555555555554,0.09947916666666666,0.59907407407407409],\"landmarks\":{\"feet-middle-point\":{\"x\":0.43979057591623039,\"y\":0.87326120556414222}},\"object_detection\":[{\"label\":\"safety vest\",\"confidence\":93.683395385742188,\"color\":16711935,\"rectangle\":[0.53281250000000002,0.37592592592592594,0.063541666666666663,0.16851851851851851]}]},{\"label\":\"feet\",\"confidence\":100.0,\"color\":16711935,\"rectangle\":[0.55989583333333337,0.77870370370370368,0.0041666666666666666,0.0055555555555555558]}],\"Parameters\":{\"timestamp\":\"4270933333\"}}
        '''
    # NOTE: changed tracking_id on first person to '2' to match above, and duplicated
    # both people since tests expect 4 people.
    redis_message_from_sep_30_build = '''
        {
        \"object_detection\":[
            {
                \"tracking_id\":2,
                \"label\":\"person\",
                \"confidence\":93.14764404296875,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.41354166666666664,\"y\":0.30092592592592593,\"width\":0.10885416666666667,\"height\":0.59629629629629632},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.46302083333333333,\"y\":0.36851851851851852},
                    \"left_ankle\":{\"x\":0.47968749999999999,\"y\":0.83888888888888891},
                    \"right_ankle\":{\"x\":0.44635416666666666,\"y\":0.81111111111111112}
                }
        },
        {
            \"tracking_id\":13,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        },
        {
            \"tracking_id\":14,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        },
        {
            \"tracking_id\":15,
            \"label\":\"person\",
            \"confidence\":94.256546020507812,
            \"color\":4294902015,
            \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
            \"landmarks\":
            {
                \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
            }
        }
        ],
        \"parameters\":{\"timestamp\":\"8375033333\"}
        }
        '''
    # frame_message = json.loads(redis_message_from_aug_19_build)
    frame_message = json.loads(redis_message_from_sep_30_build)

    if type == 'default':
        return frame_message
    
    if type == 'empty':
        return json.loads("{\"parameters\":{\"timestamp\":\"433766666\"}}")
    
    if type == 'person_2_outside':
        # same string but person 2 is in the top-left corner {0.01, 0.01}
        redis_message_from_sep_30_build_with_person_2_moved = '''
            {
            \"object_detection\":[
                {
                    \"tracking_id\":2,
                    \"label\":\"person\",
                    \"confidence\":93.14764404296875,
                    \"color\":4294902015,
                    \"rectangle\":{\"x\":0.41354166666666664,\"y\":0.30092592592592593,\"width\":0.10885416666666667,\"height\":0.59629629629629632},
                    \"landmarks\":
                    {
                        \"nose\":{\"x\":0.46302083333333333,\"y\":0.36851851851851852},
                        \"left_ankle\":{\"x\":0.01,\"y\":0.01},
                        \"right_ankle\":{\"x\":0.01001,\"y\":0.01}
                    }
            },
            {
                \"tracking_id\":13,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            },
            {
                \"tracking_id\":14,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            },
            {
                \"tracking_id\":15,
                \"label\":\"person\",
                \"confidence\":94.256546020507812,
                \"color\":4294902015,
                \"rectangle\":{\"x\":0.31197916666666664,\"y\":0.23333333333333334,\"width\":0.11197916666666667,\"height\":0.6425925925925926},
                \"landmarks\":
                {
                    \"nose\":{\"x\":0.37656250000000002,\"y\":0.29444444444444445},
                    \"left_ankle\":{\"x\":0.36302083333333335,\"y\":0.82222222222222219},
                    \"right_ankle\":{\"x\":0.34895833333333331,\"y\":0.7944444444444444}
                }
            }
            ],
            \"parameters\":{\"timestamp\":\"8375033333\"}
            }
            '''
            
        return json.loads(redis_message_from_sep_30_build_with_person_2_moved)
    
    if type == 'person_no_feet':
        del frame_message['object_detection'][0]['landmarks'] # person 0 has no feet
        return frame_message

    raise RuntimeError('Should not have gotten here')

def make_message_list(messages, channel='0'):
    message_list = [
        # messages are tuples of (system time, message)
        (idx, # system time -> 1 msg/sec
            {
                'channel': 'detection.rz:' + channel,
                'data': json.dumps(message)
            }
        )
        for idx, message in enumerate(messages)
    ]
    return message_list


# dict of tripwires, list of triggers
@pytest.fixture
def tripwires():
    return { 'test-tripwire':
            {
                "monitor_id": "0",
                "tripwire_id": "test-tripwire",
                "tripwire_name": "Entrance to lobby",
                "wire": [
                    {
                    "x": 0.5,
                    "y": 0.5
                    }
                ],
                "direction": {
                    "entry": {
                    "x": 0.5,
                    "y": 0.5
                    },
                    "exit": {
                    "x": 0.5,
                    "y": 0.5
                    }
                }
            }
        }


@pytest.fixture
def triggers():
    return [{
        "tripwire_id": "test-tripwire",
        "trigger_id": "test-trigger",
        "trigger_name": "More than 0 ppl/sec entering",
        "trigger_direction": "entry",
        "trigger_condition": "flowrate",
        "params": [
            {
            "name": "threshold",
            "value": "0"
            },
            {
            "name": "duration",
            "value": "1"
            }
        ]
    }]

async def test_region_histories_populated(ta, triggers, tripwires):
    ta._filter_len = 999 # disable the truncation down to filter length after processing
    messages = make_message_list([make_message('default') ] * 3)
    alerts = ta.apply_triggers(triggers, tripwires, messages)
    assert len(alerts) == 0, 'Should be no alerts yet'
    assert len(ta._frame_metadata_by_monitor['0']) == 3, f'Expected the 3 messages to be in monitor 0 history.'


async def test_only_tripwires_for_this_monitor_sent(ta, triggers, tripwires, caplog):
    messages = make_message_list([make_message('default') ] * 1)
    tripwires['other-monitor'] = tripwires['test-tripwire'].copy()
    tripwires['other-monitor']['monitor_id'] = 'other-monitor'

    calculate_calls = 0
    def mock_calculate_crossings(frames, filter_len, window_len, wires):
        nonlocal calculate_calls
        calculate_calls += 1
        assert len(wires) == 1, 'Expect only one wire per monitor'
        return {}
    ta.calculate_crossings = mock_calculate_crossings

    ta.apply_triggers(triggers, tripwires, messages)

    assert calculate_calls == 1, 'Expected one calculate call since only samples on one monitor'


async def test_first_frame_sends_partial_window(ta, triggers, tripwires, caplog):
    caplog.set_level(logging.INFO)
    mock_called = False
    def mock_calculate_crossings(frames, filter_len, window_len, wires):
        nonlocal mock_called
        mock_called = True
        assert len(frames)== 5
        assert filter_len == FILTER_LEN
        assert window_len == 5 - FILTER_LEN + 1, f'Expect window length to be len(frames) (=5) - FILTER_LEN + overlap_len (=1)'
        return {}
    ta.calculate_crossings = mock_calculate_crossings
    messages = make_message_list([make_message('default') ] * 5)

    ta.apply_triggers(triggers, tripwires, messages) # mock func may raise assertion here

    assert mock_called
    logtext = caplog.records[0].message
    assert logtext == 'Calculating crossings for 5 frames in monitor 0 from t=0 to t=4; window_len = 4'
    frame_metadata = ta._frame_metadata_by_monitor['0']
    assert len(frame_metadata) == 2, 'After analysis, frame history should be truncated to filter length'
    

async def test_second_frame_sends_full_window_with_1_overlap(ta, triggers, tripwires, caplog):
    caplog.set_level(logging.INFO)
    mock_called_times = 0
    def mock_calculate_crossings(frames, filter_len, window_len, wires):
        nonlocal mock_called_times
        mock_called_times += 1
        if mock_called_times == 2:
            assert len(frames)== 5 + FILTER_LEN, 'Expect # of new frames (=5) + FILTER_LEN'
            assert window_len == 6, 'Expect # of new frames (=5) + overlap (=1)'
        
        return {}

    ta.calculate_crossings = mock_calculate_crossings
    messages = make_message_list([make_message('default') ] * 10)

    ta.apply_triggers(triggers, tripwires, messages[0:5]) # send first 5 messages
    ta.apply_triggers(triggers, tripwires, messages[5:10]) # send next 5 messages

    assert mock_called_times == 2
    calc_logs = [r.message for r in caplog.records if r.message.startswith('Calculating crossings for')]
    assert calc_logs[0] == 'Calculating crossings for 5 frames in monitor 0 from t=0 to t=4; window_len = 4'
    assert calc_logs[1] == 'Calculating crossings for 7 frames in monitor 0 from t=3 to t=9; window_len = 6'
    frame_metadata = ta._frame_metadata_by_monitor['0']
    assert len(frame_metadata) == 2, 'After analysis, frame history should be truncated to filter length'
    

def test_calculate_crossings_returns_crossings_by_wire_id(ta, tripwires):
    # add more tripwires to see different results allocated to each; build off the first tripwire
    other_tripwire = list(tripwires.values())[0].copy()
    other_tripwire['tripwire_id'] = 'other-tripwire'
    tripwires['other-tripwire'] = other_tripwire
    another_tripwire = other_tripwire.copy()
    another_tripwire['tripwire_id'] = 'another-tripwire'
    tripwires['another-tripwire'] = another_tripwire

    # stub out the inner function to return 0, 1, 2 crossings for each (entries, exits)
    def mock_calculate_crossings_impl(raw_data, filter_size, window_size, tripwires):
        crossings = []
        for idx, _ in enumerate(tripwires):
            crossings.append( (idx, idx) ) # impl returns list of (entries, exits) for each tripwire
        return crossings
    
    ta._calculate_tripwires_impl = mock_calculate_crossings_impl
    messages = make_message_list([make_message('default') ] * 5)

    results = ta.calculate_crossings(messages, 0, 0, tripwires=tripwires)

    for expected_crossings, id in enumerate(tripwires.keys()):
        assert results[id]['entries'] == expected_crossings, 'Expected entries to be attached to tripwire id'
        assert results[id]['exits'] == expected_crossings, 'Expected exits to be attached to tripwire id'
    
def test_make_alerts_for_simple_trigger(ta, triggers, tripwires):
    # fake result from calculate_crossings
    crossings = {'test-tripwire': { 'entries': 1, 'exits': 0} }

    alerts = ta.make_alerts(triggers, tripwires, crossings, alert_time=42)

    assert len(alerts) == 1


def test_make_alerts_returns_none_for_no_entries(ta, triggers, tripwires):
    # fake result from calculate_crossings
    crossings = {'test-tripwire': { 'entries': 0, 'exits': 0} }

    alerts = ta.make_alerts(triggers, tripwires, crossings, alert_time=42)

    assert len(alerts) == 0

def test_make_alerts_returns_none_for_only_exits_with_entry_tripwire(ta, triggers, tripwires):
    # fake result from calculate_crossings
    crossings = {'test-tripwire': { 'entries': 0, 'exits': 1000} }

    alerts = ta.make_alerts(triggers, tripwires, crossings, alert_time=42)

    assert len(alerts) == 0

def test_make_alerts_returns_correct_alert_info(ta, triggers, tripwires):
    # fake result from calculate_crossings
    crossings = {'test-tripwire': { 'entries': 3, 'exits': 0} }
    expected_trigger = next(trigger for trigger in triggers if trigger['tripwire_id'] == 'test-tripwire')

    alerts = ta.make_alerts([expected_trigger], tripwires, crossings, alert_time=55)


    # alert example:
    # {
    #     "monitor_id": "camera1",
    #     "source_trigger": {
    #       "tripwire_id": "{unique-tripwire-id}",
    #       "trigger_id": "{unique-trigger-id}",
    #       "trigger_name": "More than 2 ppl/sec entering",
    #       "trigger_direction": "entry",
    #       "trigger_condition": "flowrate",
    #       "params": [
    #         {
    #           "name": "threshold",
    #           "value": "2"
    #         },
    #         {
    #           "name": "duration",
    #           "value": "1"
    #         }
    #        ]
    #     },
    #     "time": 1729118760,
    #     "crossings": {
    #       "count": 3,
    #       "duration": 1
    #     }
    # }

    assert len(alerts) == 1
    alert = alerts[0]
    assert alert['monitor_id'] == '0'
    assert expected_trigger == alert['source_trigger']
    assert alert['time'] == 55
    assert alert['crossings'] == {'count': 3, 'duration': 1}
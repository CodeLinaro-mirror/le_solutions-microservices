# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import random
import pandas as pd
import pytest
import cv2 as cv
import numpy as np

from region_algo import RegionHistory, point_in_polygon

@pytest.fixture
def rh_center_area():
    pts = [
        (0.3,0.3),
        (0.7,0.3),
        (0.7,0.7),
        (0.3,0.7)
        ]
    polygon = [{'x': x, 'y': y} for x,y in pts]
    return RegionHistory(polygon)

def csv_to_frame_list(csv_file):
    csv_types = {
        'frame_id': int,
        'track_id': int,
        'foot_x': float,
        'foot_y': float,
        'in_zone': bool,
        'distance': float
    }
    df = pd.read_csv(csv_file, dtype=csv_types)
    df = df.head(200) # TODO: remove this when first 100 are validated
    row_max = max(df['frame_id'])
    frames = [{} for _ in range(row_max+1)] # each frame is dict{k:v} where k=track_id, v=(x, y, in_zone)
    for index, row in df.iterrows():
        record = { 
            'frame': row['frame_id'], # only used for debugging
            'x': row['foot_x'],
            'y': row['foot_y'],
            'expected_in_zone': row['in_zone'],
            'expected_distance': row['distance'],
            }
        frame = row['frame_id']
        track_id = row['track_id']
        frames[frame][track_id] = record
        frames_str = '\n' + '\n'.join([str(f) for f in frames])
        # print(f'At idx {index}, frames: {frames_str}')
    # delete blank frames
    frames = [f for f in frames if f != {}]
    return iter(frames)

@pytest.fixture
def one_person_in_center_frame():
    return csv_to_frame_list('data/test_vector_1.csv')

def test_point_in_polygon_matches_opencv():
    random.seed(0)
    NUM_TESTS=10000
    for n in range(NUM_TESTS):
        def rand_point():
            x = random.uniform(0.0, 1.0)
            y = random.uniform(0.0, 1.0)
            return (x,y)
        contour = [rand_point() for _ in range(3)]
        pt = rand_point()
        cv_in_poly = cv.pointPolygonTest(
            np.array(contour, dtype=np.float32), 
            np.array(pt, dtype=np.float32), measureDist=False) >= 0
        test_in_poly = point_in_polygon(contour, pt[0], pt[1])

        assert  cv_in_poly == test_in_poly, f'Mismatch on iteration {n}, {pt} is/is not in {contour}'
    

def test_read_test1(one_person_in_center_frame):
    frames = one_person_in_center_frame
    people = next(frames) # first frame
    assert len(people) == 2, 'Two people in the very first frame only'
    assert set(people.keys()) == set([1, 42]), 'Expected track_ids 1 & 42 in first real frame'
    assert people[1]['x'] == 0.5, 'Expected person "1" foot at center x'
    assert people[42]['x'] == 0.0, 'Expected person "42" foot at center y'


def test_own_point_in_polygon():
    
    contour = [ (0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
    assert point_in_polygon(contour, 0.25, 0.25) == True
    assert point_in_polygon(contour, 0.75, 0.75) == False

def test_third_frame_returns_in_zone(one_person_in_center_frame, rh_center_area):
    pytest.skip(reason="Update to algorithm: >= 4 in zone required to go from out to in")
    frame_it = one_person_in_center_frame
    rh = rh_center_area
    assert rh.detect_frame(next(frame_it)) == [], 'Expected not-in-zone for first frame'
    assert rh.detect_frame(next(frame_it)) == [], 'Expected not-in-zone for second frame'
    in_zone_flags = rh.detect_frame(next(frame_it))
    assert in_zone_flags == [1], 'Expected ID 1 to be in-zone'


def test_all_frames(one_person_in_center_frame, rh_center_area):
    pytest.skip(reason="Need to update the fake test vector to account for >=4 frames to enter, 0 of 5 frames to leave, and matching logic")
    frame_it = one_person_in_center_frame
    rh = rh_center_area

    while True:
        frame = next(frame_it, None)
        print(frame)
        if frame == None:
            break
        people_in_zones = rh.detect_frame(frame)
        id_to_check = 1
        if frame[id_to_check]['expected_in_zone'] == False:
            assert people_in_zones == [], f'Expected empty for frame: {frame}'
        else:
            assert people_in_zones == [1], f'Expected id 1 in zone for frame: {frame}, people_in_zones = {people_in_zones}'

def test_target_vector():
    pytest.skip(reason="Need up update logic to account for >=4 frames to enter, 0 of 5 frames to leave")
    rh1 = RegionHistory([(499.2,871.2),(1548.0001,640.80005),(1771.2001,1207.2001),(1908.0001,1612.8),(1236.0,1785.6001),(499.2,1922.4),(475.2,1456.8)])
    rh2 = RegionHistory([(2083.2002,552.0),(2258.4001,820.80005),(2388.0,547.2)])

    frame_it = csv_to_frame_list('data/restricted_zone_test_vector.csv')
    while (frame := next(frame_it)) == {}:
        continue

    while True:
        print(frame)
        if frame == None:
            break
        
        in_zones = [rh.detect_frame(frame) for rh in [rh1, rh2]]
        in_zones = set(in_zones[0] + in_zones[1])
        for id, frame_metadata in frame.items():
            if frame_metadata['expected_in_zone']:
                assert id in in_zones, f'Did not find expected person at frame {frame}'
        frame = next(frame_it)




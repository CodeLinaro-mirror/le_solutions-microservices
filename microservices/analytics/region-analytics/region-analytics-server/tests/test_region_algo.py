# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
import random
import pandas as pd
import pytest
import cv2 as cv
import numpy as np

from app.region_algo import RegionHistory, point_in_polygon

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
    

def test_own_point_in_polygon():
    
    contour = [ (0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
    assert point_in_polygon(contour, 0.25, 0.25) == True
    assert point_in_polygon(contour, 0.75, 0.75) == False

# TODO: validate vs restricted_zone_test_vector.csv

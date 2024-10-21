#!/bin/bash
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e -v --gst-debug=2 \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimlqnn name=stage_01_inference model=/opt/data/libqnn2.24_PD.so backend=/usr/lib/libQnnHtp.so \
qtimlvdetection name=stage_01_postproc threshold=90.0 stabilization=true results=10 module=qpd labels=/opt/data/qpd.labels  \
rtspsrc location=$1 ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=5 output-io-mode=5 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue  ! qtiobjtracker ! queue ! qtirestrictedzonedbg zone-config="params,zone1=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,zone2=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>;" ! queue ! qtivoverlay masks="{(structure)\"Zone1,polygon=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,color=0x7F00007F;\",(structure)\"Zone2,polygon=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>,color=0x7F00007F;\"}" ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! waylandsink async=false name=display sync=false fullscreen=true \
t_split_2. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel="Detection::YoloV8::RZ::0" host="172.17.0.1" port=6379 \
t_split_2. ! queue ! v4l2h264enc name=encoder capture-io-mode=5 output-io-mode=5 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=8900

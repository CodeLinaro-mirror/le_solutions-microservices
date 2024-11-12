# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
if [[ $INPUT_TYPE == "rtsp" ]]; then

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimlqnn name=stage_01_inference model=/opt/data/libqnn2.24_FD.so backend=/usr/lib/libQnnHtp.so \
qtimlvdetection name=stage_01_postproc stabilization=false threshold=51.0 results=6 module=qfd labels=/opt/data/qfd.labels \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimlqnn name=stage_02_inference model=/opt/data/libresnet_wd2_weak_score_1205_3ch.so backend=/usr/lib/libQnnHtp.so \
qtimlvpose name=stage_02_postproc threshold=51.0 results=6 module=lite-3dmm labels=/opt/data/3dmm.labels \
qtimlvconverter name=stage_03_preproc mode=roi-batch-cumulative \
qtimlqnn name=stage_03_inference model=/opt/data/libqnn2.24_FR.so backend=/usr/lib/libQnnHtp.so \
qtimlvclassification name=stage_03_postproc threshold=60.0 results=6 module=qfr labels=/opt/data/qfr.labels \
rtspsrc location=$INPUT_URL ! queue ! rtpptdemux ! rtph264depay ! h264parse ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=5 output-io-mode=5 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! qtiobjtracker ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! metamux_3. \
t_split_3. ! queue ! stage_03_preproc. stage_03_preproc. ! queue ! stage_03_inference. stage_03_inference. ! queue ! stage_03_postproc. stage_03_postproc. ! text/x-raw ! queue ! metamux_3. \
qtimetamux name=metamux_3 ! queue ! qtimetatransform module=roi-label-moving-average ! queue ! qtivoverlay engine=gles ! queue ! tee name=t_split_4 \
t_split_4. ! queue ! waylandsink name=display sync=false async=false fullscreen=true \
t_split_4. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false  channel=$REDIS_DETECTION_CHANNEL host="172.17.0.1" port=6379 \
t_split_4. ! queue ! v4l2h264enc name=encoder capture-io-mode=5 output-io-mode=5 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=8900

then

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimlqnn name=stage_01_inference model=/opt/data/libqnn2.24_FD.so backend=/usr/lib/libQnnHtp.so \
qtimlvdetection name=stage_01_postproc stabilization=false threshold=51.0 results=6 module=qfd labels=/opt/data/qfd.labels \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimlqnn name=stage_02_inference model=/opt/data/libresnet_wd2_weak_score_1205_3ch.so backend=/usr/lib/libQnnHtp.so \
qtimlvpose name=stage_02_postproc threshold=51.0 results=6 module=lite-3dmm labels=/opt/data/3dmm.labels \
qtimlvconverter name=stage_03_preproc mode=roi-batch-cumulative \
qtimlqnn name=stage_03_inference model=/opt/data/libqnn2.24_FR.so backend=/usr/lib/libQnnHtp.so \
qtimlvclassification name=stage_03_postproc threshold=60.0 results=6 module=qfr labels=/opt/data/qfr.labels \
filesrc location=$INPUT_URL ! qtdemux ! h264parse config-interval=1 ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=5 output-io-mode=5 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! qtiobjtracker ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! metamux_3. \
t_split_3. ! queue ! stage_03_preproc. stage_03_preproc. ! queue ! stage_03_inference. stage_03_inference. ! queue ! stage_03_postproc. stage_03_postproc. ! text/x-raw ! queue ! metamux_3. \
qtimetamux name=metamux_3 ! queue ! qtimetatransform module=roi-label-moving-average ! queue ! qtivoverlay engine=gles ! queue ! tee name=t_split_4 \
t_split_4. ! queue ! waylandsink name=display sync=false async=false fullscreen=true \
t_split_4. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false  channel=$REDIS_DETECTION_CHANNEL host="172.17.0.1" port=6379 \
t_split_4. ! queue ! v4l2h264enc name=encoder capture-io-mode=5 output-io-mode=5 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=8900

fi
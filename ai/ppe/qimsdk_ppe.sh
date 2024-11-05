# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e --gst-debug=2 \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/foot_track_net_quantized.tflite \
qtimlvdetection name=stage_01_postproc threshold=90.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=<0.0,25.0,114.0,0.0>,q-scales=<0.0035636962857097387,1.2998489141464233,2.1744511127471924,0.00390625>;" labels=/opt/ppe/foot_track_net.labels \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/gear_guard_net_quantized.tflite \
qtimlvdetection name=stage_02_postproc threshold=51.0 stabilization=true results=10 module=yolov5 constants="YoloV5,q-offsets=<180.0,178.0,170.0>,q-scales=<0.09695824235677719,0.08805476874113083,0.08215426653623581>;" labels=/opt/ppe/gear_guard_net.labels \
rtspsrc location=$1 ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=5 output-io-mode=5 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay engine=gles ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false  channel="Detection::YoloV8::PPE::0" host="172.17.0.1" port=6379 \
t_split_3. ! queue ! waylandsink name=display x=0 y=540 width=960 height=540 sync=false async=false fullscreen=true \
t_split_3. ! queue ! v4l2h264enc name=encoder capture-io-mode=5 output-io-mode=5 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=8900
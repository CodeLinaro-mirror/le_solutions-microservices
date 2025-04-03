# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if [[ $INPUT_TYPE == "rtsp" ]]; then

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlvdetection name=stage_01_postproc threshold=90.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=${MODEL_OFFSETS_PERSON},q-scales=${MODEL_SCALES_PERSON};" labels=${LABELS_PATH_PERSON} \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PPE} \
qtimlvdetection name=stage_02_postproc threshold=75.0 stabilization=true results=10 module=yolov5 constants="YoloV5,q-offsets=${MODEL_OFFSETS_PPE},q-scales=${MODEL_SCALES_PPE};" labels=${LABELS_PATH_PPE} \
rtspsrc location=${INPUT_URL} ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay engine=gles ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=6379 \
t_split_3. ! queue ! waylandsink sync=false async=false fullscreen=true \
t_split_3. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

else

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlvdetection name=stage_01_postproc threshold=90.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=${MODEL_OFFSETS_PERSON},q-scales=${MODEL_SCALES_PERSON};" labels=${LABELS_PATH_PERSON} \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PPE} \
qtimlvdetection name=stage_02_postproc threshold=75.0 stabilization=true results=10 module=yolov5 constants="YoloV5,q-offsets=${MODEL_OFFSETS_PPE},q-scales=${MODEL_SCALES_PPE};" labels=${LABELS_PATH_PPE} \
filesrc location=${INPUT_URL} ! qtdemux ! h264parse config-interval=1 ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay engine=gles ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=6379 \
t_split_3. ! queue ! waylandsink sync=true async=false fullscreen=true \
t_split_3. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

fi

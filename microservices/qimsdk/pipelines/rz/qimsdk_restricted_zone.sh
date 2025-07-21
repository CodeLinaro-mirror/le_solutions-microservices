#!/bin/bash
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if [[ $INPUT_TYPE == "rtsp" ]]; then

ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlvdetection name=stage_01_postproc threshold=80.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=${MODEL_OFFSETS_PERSON},q-scales=${MODEL_SCALES_PERSON};" labels=${LABELS_PATH_PERSON} \
rtspsrc location=${INPUT_URL} ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue  ! qtiobjtracker ! queue ! qtirestrictedzonedbg zone-config="params,zone1=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,zone2=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>;" ! queue ! qtivoverlay masks="{(structure)\"Zone1,polygon=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,color=0x7F00007F;\",(structure)\"Zone2,polygon=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>,color=0x7F00007F;\"}" ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! waylandsink sync=false async=false fullscreen=true \
t_split_2. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_2. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

elif [[ $INPUT_TYPE == "on-device-camera" ]]; then

ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlvdetection name=stage_01_postproc threshold=80.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=${MODEL_OFFSETS_PERSON},q-scales=${MODEL_SCALES_PERSON};" labels=${LABELS_PATH_PERSON} \
qtiqmmfsrc name=camsrc camera=${INPUT_URL} ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1,interlace-mode=progressive,colorimetry=bt601 ! identity sync=true ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue  ! qtiobjtracker ! queue ! qtirestrictedzonedbg zone-config="params,zone1=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,zone2=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>;" ! queue ! qtivoverlay masks="{(structure)\"Zone1,polygon=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,color=0x7F00007F;\",(structure)\"Zone2,polygon=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>,color=0x7F00007F;\"}" ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! waylandsink sync=true async=false fullscreen=true \
t_split_2. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_2. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

else

demuxer=qtdemux
source_sequence="filesrc location=${INPUT_URL} ! ${demuxer}"

if [[ "${INPUT_URL##*.}" == "ts" ]]; then
    demuxer=tsdemux
    source_sequence="multifilesrc location=${INPUT_URL} ! ${demuxer}"
fi

ulimit -n 4096 && gst-launch-1.0  -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlvdetection name=stage_01_postproc threshold=80.0 stabilization=true results=10 module=qpd constants="qpd,q-offsets=${MODEL_OFFSETS_PERSON},q-scales=${MODEL_SCALES_PERSON};" labels=${LABELS_PATH_PERSON} \
${source_sequence} ! h264parse config-interval=1 ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue  ! qtiobjtracker ! queue ! qtirestrictedzonedbg zone-config="params,zone1=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,zone2=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>;" ! queue ! qtivoverlay masks="{(structure)\"Zone1,polygon=<<100,700>,<750,700>,<750,1000>,<550,1050>,<100,900>>,color=0x7F00007F;\",(structure)\"Zone2,polygon=<<1200,700>,<1850,700>,<1850,1000>,<1350,1050>,<1200,900>>,color=0x7F00007F;\"}" ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! waylandsink sync=true async=false fullscreen=true \
t_split_2. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_2. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

fi

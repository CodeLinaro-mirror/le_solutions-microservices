# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
if [[ $INPUT_TYPE == "rtsp" ]]; then

ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlpostprocess name=stage_01_postproc results=10 module=qpd labels=${LABELS_PATH_PERSON} settings=${SETTINGS_PATH_PERSON} \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PPE} \
qtimlpostprocess name=stage_02_postproc results=10 module=yolov5 labels=${LABELS_PATH_PPE} settings="{\"confidence\": 50.0}" \
rtspsrc location=${INPUT_URL} ! queue ! rtpptdemux ! rtph264depay ! h264parse ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! qtiobjtracker ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_3. ! queue ! waylandsink sync=false async=false fullscreen=true \
t_split_3. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}
elif [[ $INPUT_TYPE == "on-device-camera" ]]; then

ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlpostprocess name=stage_01_postproc results=10 module=qpd labels=${LABELS_PATH_PERSON} settings=${SETTINGS_PATH_PERSON} \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PPE} \
qtimlpostprocess name=stage_02_postproc results=10 module=yolov5 labels=${LABELS_PATH_PPE} settings="{\"confidence\": 50.0}" \
qtiqmmfsrc name=camsrc camera=${INPUT_URL} ! video/x-raw,format=NV12,width=1280,height=720,framerate=30/1,interlace-mode=progressive,colorimetry=bt601 ! identity sync=true  ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! qtiobjtracker ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_3. ! queue ! waylandsink sync=true async=false fullscreen=true \
t_split_3. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

else

demuxer=qtdemux
source_sequence="filesrc location=${INPUT_URL} ! ${demuxer}"

if [[ "${INPUT_URL##*.}" == "ts" ]]; then
    demuxer=tsdemux
    source_sequence="multifilesrc location=${INPUT_URL} ! ${demuxer}"
fi

ulimit -n 4096 && gst-launch-1.0 -e \
qtimlvconverter name=stage_01_preproc mode=image-batch-non-cumulative \
qtimltflite name=stage_01_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PERSON} \
qtimlpostprocess name=stage_01_postproc results=10 module=qpd labels=${LABELS_PATH_PERSON} settings=${SETTINGS_PATH_PERSON} \
qtimlvconverter name=stage_02_preproc mode=roi-batch-cumulative \
qtimltflite name=stage_02_inference delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=${MODEL_PATH_PPE} \
qtimlpostprocess name=stage_02_postproc results=10 module=yolov5 labels=${LABELS_PATH_PPE} settings="{\"confidence\": 50.0}" \
${source_sequence} ! h264parse config-interval=1 ! v4l2h264dec capture-io-mode=4 output-io-mode=4 ! video/x-raw,format=NV12 ! queue ! tee name=t_split_1 \
t_split_1. ! queue ! metamux_1. \
t_split_1. ! queue ! stage_01_preproc. stage_01_preproc. ! queue ! stage_01_inference. stage_01_inference. ! queue ! stage_01_postproc. stage_01_postproc. ! text/x-raw ! queue ! metamux_1. \
qtimetamux name=metamux_1 ! queue ! qtiobjtracker ! queue ! tee name=t_split_2 \
t_split_2. ! queue ! metamux_2. \
t_split_2. ! queue ! stage_02_preproc. stage_02_preproc. ! queue ! stage_02_inference. stage_02_inference. ! queue ! stage_02_postproc. stage_02_postproc. ! text/x-raw ! queue ! metamux_2. \
qtimetamux name=metamux_2 ! queue ! qtivoverlay ! queue ! tee name=t_split_3 \
t_split_3. ! queue ! qtimlmetaparser module=json ! qtiredissink sync=false async=false channel=${REDIS_DETECTION_CHANNEL} host="172.17.0.1" port=${REDIS_OUT_PORT} \
t_split_3. ! queue ! waylandsink sync=true async=false fullscreen=true \
t_split_3. ! queue ! identity sync=true ! v4l2h264enc capture-io-mode=4 output-io-mode=4 ! queue ! h264parse config-interval=1 ! queue ! qtirtspbin address=0.0.0.0 port=${OUT_PORT}

fi

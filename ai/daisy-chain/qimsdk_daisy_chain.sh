# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

export XDG_RUNTIME_DIR=/dev/socket/weston && export WAYLAND_DISPLAY=wayland-1 && ulimit -n 4096 && gst-launch-1.0 -e \
qtimltflite name=tflite_yolov5 delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/yolov5m-320x320-int8.tflite \
qtimltflite name=tflite_Mobilenet_1 delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/mobilenet_v3_large_quantized.tflite \
qtimltflite name=tflite_Mobilenet_2 delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/mobilenet_v3_large_quantized.tflite \
qtimltflite name=tflite_Mobilenet_3 delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/mobilenet_v3_large_quantized.tflite \
qtimltflite name=tflite_Mobilenet_4 delegate=external external-delegate-path=libQnnTFLiteDelegate.so external-delegate-options="QNNExternalDelegate,backend_type=htp;" model=/opt/data/mobilenet_v3_large_quantized.tflite \
qtimlvconverter name=ml_convert_0 ! queue ! tflite_yolov5. tflite_yolov5. ! queue ! tee name=t_split_1 qtivcomposer name=mixer \
sink_0::position="<0, 0>" sink_0::dimensions="<1280, 720>" \
sink_1::position="<0, 0>" sink_1::dimensions="<1280, 720>" \
sink_2::position="<0, 0>" sink_2::dimensions="<384, 216>" \
sink_3::position="<896, 0>" sink_3::dimensions="<384, 216>" \
sink_4::position="<0, 504>" sink_4::dimensions="<384, 216>" \
sink_5::position="<896, 504>" sink_5::dimensions="<384, 216>" \
sink_6::position="<0, 0>" sink_6::dimensions="<384, 40>" \
sink_7::position="<896, 0>" sink_7::dimensions="<384, 40>" \
sink_8::position="<0, 504>" sink_8::dimensions="<384, 40>" \
sink_9::position="<896, 504>" sink_9::dimensions="<384, 40>" \
mixer. ! waylandsink sync=true async=false fullscreen=true \
filesrc location=/opt/data/Animals_000_720p_180s_30FPS.mp4 ! qtdemux ! queue ! h264parse ! v4l2h264dec capture-io-mode=5 output-io-mode=5 ! queue ! tee name=v_split_1 ! queue ! metamux1. \
v_split_1. ! queue ! ml_convert_0. \
t_split_1. ! queue ! qtimlvdetection threshold=75.0 results=4 module=yolov5 labels=/opt/data/yolov5m.labels constants="YoloV5,q-offsets=<3.0>,q-scales=<0.005047998391091824>;" ! text/x-raw ! queue ! \
qtimetamux name=metamux1 ! queue ! tee name=t_split_2 ! queue ! mixer. \
t_split_1. ! queue ! qtimlvdetection threshold=75.0 results=5 module=yolov5 labels=/opt/data/yolov5m.labels constants="YoloV5,q-offsets=<3.0>,q-scales=<0.005047998391091824>;" ! video/x-raw,width=512,height=288 ! queue ! \
mixer. t_split_2. ! queue ! qtivsplit name=vsplit1 src_0::mode=single-roi-meta src_1::mode=single-roi-meta src_2::mode=single-roi-meta src_3::mode=single-roi-meta \
vsplit1. ! queue ! tee name=split_1 ! queue ! ml_convert_1. split_1. ! queue ! mixer. \
vsplit1. ! queue ! tee name=split_2 ! queue ! ml_convert_2. split_2. ! queue ! mixer. \
vsplit1. ! queue ! tee name=split_3 ! queue ! ml_convert_3. split_3. ! queue ! mixer. \
vsplit1. ! queue ! tee name=split_4 ! queue ! ml_convert_4. split_4. ! queue ! mixer. \
qtimlvconverter name=ml_convert_1 ! queue ! tflite_Mobilenet_1. tflite_Mobilenet_1. ! queue ! mlclass_1. \
qtimlvconverter name=ml_convert_2 ! queue ! tflite_Mobilenet_2. tflite_Mobilenet_2. ! queue ! mlclass_2. \
qtimlvconverter name=ml_convert_3 ! queue ! tflite_Mobilenet_3. tflite_Mobilenet_3. ! queue ! mlclass_3. \
qtimlvconverter name=ml_convert_4 ! queue ! tflite_Mobilenet_4. tflite_Mobilenet_4. ! queue ! mlclass_4. \
qtimlvclassification name=mlclass_1 threshold=51.0 results=2 module=mobilenet labels=/opt/data/mobilenet.labels extra-operation=softmax constants="Mobilenet,q-offsets=<-29.0>,q-scales=<0.18705224990844727>;" ! video/x-raw,width=384,height=40 ! queue ! mixer. \
qtimlvclassification name=mlclass_2 threshold=51.0 results=2 module=mobilenet labels=/opt/data/mobilenet.labels extra-operation=softmax constants="Mobilenet,q-offsets=<-29.0>,q-scales=<0.18705224990844727>;" ! video/x-raw,width=384,height=40 ! queue ! mixer. \
qtimlvclassification name=mlclass_3 threshold=51.0 results=2 module=mobilenet labels=/opt/data/mobilenet.labels extra-operation=softmax constants="Mobilenet,q-offsets=<-29.0>,q-scales=<0.18705224990844727>;" ! video/x-raw,width=384,height=40 ! queue ! mixer. \
qtimlvclassification name=mlclass_4 threshold=51.0 results=2 module=mobilenet labels=/opt/data/mobilenet.labels extra-operation=softmax constants="Mobilenet,q-offsets=<-29.0>,q-scales=<0.18705224990844727>;" ! video/x-raw,width=384,height=40 ! queue ! mixer.
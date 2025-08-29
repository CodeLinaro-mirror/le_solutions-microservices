# Instructions on how to build Docker image and Run
DOCKER_BUILDKIT=1 docker build --progress=plain --target llm_device_image . -t llm-device-image

# After Docker is built on local machine. It must be saved
docker save -o llm-device-image.tar llm-device-image

# Push the tar file to Device
adb push llm-device-image.tar /opt/docker

# Load the Docker image on device
docker load -i llm-device-image.tar

# Push all Genie Configs and model files to /opt/data
adb push genie_bundle /opt/data

# Now run the docker image using either the run command or a docker_run.sh
# Ensure right mappings are present
docker run -d --device /dev/dma_heap/system --device /dev/dma_heap/qcom,system --device /dev/fastrpc-cdsp --device /dev/fastrpc-cdsp1 -v /usr/lib/libdmabufheap.so.0:/usr/lib/libdmabufheap.so.0 -v /usr/lib/libcdsprpc.so:/usr/lib/libcdsprpc.so -v /etc/models/genie_bundle_llama3_1_8B:/etc/models/genie_bundle_llama3_1_8B -v /etc/models/genie_bundle_llama3_2_3B:/etc/models/genie_bundle_llama3_2_3B -v /etc/models/genie_bundle_qwen2_5_7B:/etc/models/genie_bundle_qwen2_5_7B  -h llm-device-image --name llm -it -d llm-device-image

# Go into the docker shell
docker exec -it llm bash

# Run the LLM C Sample App inside the Container shell
cd /opt/data/genie_bundle
llm-service-c-example

# GENAI Microservice

## Docker commands

### Build genai-builder

```bash
time DOCKER_BUILDKIT=1 docker build --progress=plain --target genai-builder . -t genai-builder
```

### Run genai-builder on PC for development

```bash
docker run -it -d --net host -h genai-builder --name genai-builder genai-builder bash
```

### Build genai-service

```bash
time DOCKER_BUILDKIT=1 docker build --platform linux/arm64 --progress=plain --target genai-service . -t genai-service
```

### Build docker-compose

```bash
# Build with host x86
docker compose --env-file tests/.env build

# Build with host arm64
docker compose --env-file tests/.env_device build
```

## Simple steps to create docker environment for device

### Docker compose

```bash
# Only if occures the following error:
# ERROR [genai internal] load metadata for docker.io/library/ubuntu:24.04
docker buildx use default

# Build docker-compose
docker compose --env-file tests/.env build

# Save the image
docker save -o genai-service.tar genai-service

# Deploy the image, yaml file and .env files
adb shell "mkdir -p /opt/docker"
adb push genai-service.tar /opt/docker
adb push docker-compose.yaml /opt/docker
adb push tests/.env /opt/docker

# Load the image
adb shell "docker load -i /opt/docker/genai-service.tar"

# Run the docker compose service inside the device
adb shell
cd /opt/docker/
docker-compose -f docker-compose.yaml --env-file .env up genai
```

### Docker compose (chatcompletion)

```bash
# Only if occures the following error:
# ERROR [genai internal] load metadata for docker.io/library/ubuntu:24.04
docker buildx use default

# Build docker-compose (chatcompletion)
docker compose -f docker-compose.chatcompletion.yaml --env-file tests/.env build

# Save the image (chatcompletion)
docker save -o genai_chatcompletion_service.tar genai_chatcompletion_service

# Deploy the image, yaml file and .env files (chatcompletion)
adb shell "mkdir -p /opt/docker"
adb push genai_chatcompletion_service.tar /opt/docker
adb push docker-compose.chatcompletion.yaml /opt/docker
adb push tests/.env /opt/docker

# Load the image (chatcompletion)
adb shell "docker load -i /opt/docker/genai_chatcompletion_service.tar"

# Run the docker compose service inside the device (chatcompletion)
adb shell
cd /opt/docker/
docker-compose -f docker-compose.chatcompletion.yaml --env-file .env up genai-vlm
```

### Development (on Host)

Edit the source code on the Host machine
Attach to genai-builder-chatcompletion container
Use Incremental build and push functions to update device's container

#### Docker Compose Build cmd (on Host):
```bash
docker compose -f docker-compose.chatcompletion.yaml --env-file tests/.env build genai-builder
```

#### Docker Compose Start cmd (on Host):
```bash
docker compose -f docker-compose.chatcompletion.yaml --env-file tests/.env up -d genai-builder
```

#### Docker Compose Run cmd (on Host):
```bash
docker compose -f docker-compose.chatcompletion.yaml --env-file tests/.env run --rm genai-builder bash
```

#### Docker Compose Execute cmd (on Host):
```bash
docker compose -f docker-compose.chatcompletion.yaml --env-file tests/.env exec genai-builder bash
```

#### Incremental Build (inside genai-builder-chatcompletion container (C/C++))
````bash
VLM-build
````

#### Clean Build (inside genai-builder-chatcompletion container (C/C++))
````bash
VLM-clean && VLM-build
````

#### Push artifacts to device (inside genai-builder-chatcompletion container (C/C++))
````bash
VLM-push-artifacts
````

#### Incremental Build (inside genai-builder-chatcompletion container (Python))
```bash
VLM-install-openapi-server
```

#### Push venv to device (inside genai-builder-chatcompletion container (Python))
```bash
VLM-push-venv
```

#### Copy venv from genai-builder-chatcompletion to genai_chatcompletion_service (inside device)
```bash
docker cp genai-builder-chatcompletion:/mnt/work/venv /tmp/qti/development/.
docker cp /tmp/qti/development/venv genai_chatcompletion_service:/mnt/work/.
```

#### Copy artifacts from genai-builder-chatcompletion to genai_chatcompletion_service (inside device)
```bash
docker cp genai-builder-chatcompletion:/mnt/work/deploy/usr /tmp/qti/development/.
docker cp /tmp/qti/development/usr genai_chatcompletion_service:/.
```

# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

docker rmi paapi raapi cameraapi ras pas redis nginx || true # OK to fail if first run, etc.

if [ "$RB2" = "true" ]; then
    echo "building for RB2"
    BUILD_CMD="buildx build --platform=linux/arm64/v8 --load --output type=docker"
else
    echo "building local for amd64"
    BUILD_CMD="build"
fi

# PAAPI
docker $BUILD_CMD -t paapi ../people-analytics/people-analytics-api

# RAAPI
docker $BUILD_CMD -t raapi ../region-analytics/ra-api

# PAS
docker $BUILD_CMD -t pas ../people-analytics/people-analytics-server

# RAS
docker $BUILD_CMD -t ras ../region-analytics/region-analytics-server

# NGINX
docker $BUILD_CMD -t nginx ../util/nginx

# Redis
docker $BUILD_CMD -t redis ../util/redis

# camera api
docker $BUILD_CMD -t cameraapi ../camera-api

if [ "$RB2" = "true" ]; then
    # Mariadb
    docker pull --platform=linux/arm64 mariadb

    # Save images to transfer to device
    docker save redis nginx mariadb -o ../baseImages
    docker save raapi paapi cameraapi -o ../webServerImages
    docker save ras pas -o ../analyticsImages
else
    # Mariadb
    docker pull mariadb
fi

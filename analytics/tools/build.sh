# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

docker rmi people-analytics-api region-analytics-api camera-api people-analytics-server region-analytics-server redis nginx || true # OK to fail if first run, etc.

if [ "$RB2" = "true" ]; then
    echo "building for RB2"
    BUILD_CMD="buildx build --platform=linux/arm64/v8 --load --output type=docker"
else
    echo "building local for amd64"
    BUILD_CMD="build"
fi

# PAAPI
docker $BUILD_CMD -t people-analytics-api ../people-analytics/people-analytics-api

# RAAPI
docker $BUILD_CMD -t region-analytics-api ../region-analytics/region-analytics-api

# PAS
docker $BUILD_CMD -t people-analytics-server ../people-analytics/people-analytics-server

# RAS
docker $BUILD_CMD -t region-analytics-server ../region-analytics/region-analytics-server

# NGINX
docker $BUILD_CMD -t nginx ../util/nginx

# Redis
docker $BUILD_CMD -t redis ../util/redis

# camera api
docker $BUILD_CMD -t camera-api ../camera-api

if [ "$RB2" = "true" ]; then
    # Mariadb
    docker pull --platform=linux/arm64 mariadb

    # Save images to transfer to device
    docker save redis nginx mariadb -o ../baseImages
    docker save region-analytics-api people-analytics-api camera-api -o ../webServerImages
    docker save region-analytics-server people-analytics-server -o ../analyticsImages
else
    # Mariadb
    docker pull mariadb
fi

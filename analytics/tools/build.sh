# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#!/bin/bash 

docker rmi paapi raapi ras pas redis nginx || true # OK to fail if first run, etc.

if [ $RB2 = 'true' ]; then
    echo 'building for RB2'

    # PAAPI
    cd people-analytics-api
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t paapi

    # RAAPI
    cd ../ra-api
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t raapi

    # NGINX
    cd ../nginx
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t nginx

    # PA Analytics
    cd ../people-analytics-server
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t pas

    cd ../region-analytics-server
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t ras

    # Redis
    cd ../redis
    docker buildx build --platform=linux/arm64/v8 --load --output type=docker . -t redis

    # Mariadb
    docker pull --platform=linux/arm64 docker-registry.qualcomm.com/library/mariadb

    cd ../
    docker save redis nginx docker-registry.qualcomm.com/library/mariadb -o baseImages
    docker save raapi paapi -o webServerImages
    docker save ras pas -o analyticsImages
else
    echo 'building local for amd64'

    # PAAPI
    cd people-analytics-api
    docker build . -t paapi

    # RAAPI
    cd ../ra-api
    docker build . -t raapi

    # NGINX
    cd ../nginx
    docker build . -t nginx

    # PA Analytics
    cd ../people-analytics-server
    docker build . -t pas

    # Redis
    cd ../redis
    docker build . -t redis

    # Mariadb
    docker pull docker-registry.qualcomm.com/library/mariadb

    # Fake Data
    cd ../fake-object-detect
    docker build . -t fakedata
fi

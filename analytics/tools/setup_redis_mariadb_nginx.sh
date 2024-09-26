# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#!/bin/bash 
./check_date.sh || exit 1

# Start Redis 
docker run --restart always --name redis --net host --hostname redis -d redis redis-server --loglevel notice

# Start Mariadb with or without initial data
mkdir -p /opt/data/mariadb
echo "Creating database with initial data from init.sql"
docker run --restart always --name mariadb --net host --hostname mariadb -v /opt/init.sql:/docker-entrypoint-initdb.d/init.sql -v /opt/data/mariadb:/var/lib/mysql -e MYSQL_ROOT_PASSWORD=secretpw -d docker-registry.qualcomm.com/library/mariadb

# Start nginx
docker run --restart always --name nginx --net host -d nginx

# Start cameraapi
docker run --restart always --name cameraapi --net host -e redisHost=localhost -e mariadbHost=localhost --expose 3000 -d cameraapi

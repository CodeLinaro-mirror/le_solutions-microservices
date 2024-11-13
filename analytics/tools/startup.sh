# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#!/bin/bash 

# Create Docker network for each container to access each other
# docker network create ppeNetwork

# Start Redis 
docker run --name redis --net host --hostname redis -d redis redis-server --loglevel notice

# If RB2 isn't set as an environment variable, set it to "true" if the platform machine name starts with qcp,
# (e.g., RB2 is qcm6490)
if [ -z ${RB2+unset} ]; then
    if [[ `uname -n` == "qcm"* ]]; then
        RB2=true
    elif [[ `uname -n` == "qcs"* ]]; then
        RB2=true
    else
        RB2=false
    fi
fi

POPULATEMARIADB="${POPULATEMARIADB:-true}" # set to true unless set otherwise

# Start Mariadb with or without initial data
mkdir -p /opt/data/mariadb
if [ $POPULATEMARIADB = "true" ]; then
    echo "Creating database with initial data from init.sql"
    docker run --name mariadb --net host --hostname mariadb -v /opt/init.sql:/docker-entrypoint-initdb.d/init.sql -v /opt/data/mariadb:/var/lib/mysql -e MYSQL_ROOT_PASSWORD=secretpw -d mariadb
else
    docker run --name mariadb --net host --hostname mariadb -v /opt/data/mariadb:/var/lib/mysql -e MYSQL_ROOT_PASSWORD=secretpw -d mariadb
fi


# Run the People Analytics Service; 172.17.0.1 is the default docker bridge IP to the host
docker run --name people-analytics-server --net host -e REDIS_HOST=172.17.0.1 -d people-analytics-server

# Run the Region Analytics Service;
docker run --name region-analytics-server --net host -e REDIS_HOST=172.17.0.1 -d region-analytics-server

# Run the People Analytics Web API
docker run --name people-analytics-api --net host -e redisHost=localhost -e mariadbHost=localhost -e mariadbPass=secretpw --expose 8080 -d people-analytics-api

# Run the Region of Interest Web API
docker run --name region-analytics-api --net host -e redisHost=localhost -e mariadbHost=localhost -e mariadbPass=secretpw --expose 8081 -d region-analytics-api

docker run --name camera-api --net host -e redisHost=localhost -e mariadbHost=localhost -e mariadbPass=secretpw --expose 3000 -d camera-api

docker run --name nginx --net host -d nginx

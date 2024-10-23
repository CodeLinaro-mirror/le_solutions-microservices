# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#!/bin/bash 

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

# Start Redis 
docker run --name redis --net host --hostname redis -d redis redis-server --loglevel notice

# Start Mariadb with or without initial data
mkdir -p /opt/data/mariadb
if [ $POPULATEMARIADB = "true" ]; then
    echo "Creating database with initial data from init.sql"
    docker run --name mariadb --net host --hostname mariadb -v /opt/init.sql:/docker-entrypoint-initdb.d/init.sql -v /opt/data/mariadb:/var/lib/mysql -e MYSQL_ROOT_PASSWORD=secretpw -d docker-registry.qualcomm.com/library/mariadb
else
    docker run --name mariadb --net host --hostname mariadb -v /opt/data/mariadb:/var/lib/mysql -e MYSQL_ROOT_PASSWORD=secretpw -d docker-registry.qualcomm.com/library/mariadb
fi

# Start nginx
docker run --name nginx --net host -d nginx

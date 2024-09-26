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

# Run the Region Analytics Service;
docker run --restart always --name ras --net host -e REDIS_HOST=172.17.0.1 -d ras

# Run the Region of Interest Web API
docker run --restart always --name raapi --net host -e redisHost=localhost -e mariadbHost=localhost -e mariadbPass=secretpw --expose 8081 -d raapi

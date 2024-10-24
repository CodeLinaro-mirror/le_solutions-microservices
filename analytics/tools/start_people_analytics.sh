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

# Run the People Analytics Service; 172.17.0.1 is the default docker bridge IP to the host
docker run --restart always --name pas --net host -e REDIS_HOST=172.17.0.1 -d pas

# Run the People Analytics Web API
docker run --restart always --name paapi --net host -e redisHost=localhost -e mariadbHost=localhost -e mariadbPass=secretpw --expose 8080 -d paapi

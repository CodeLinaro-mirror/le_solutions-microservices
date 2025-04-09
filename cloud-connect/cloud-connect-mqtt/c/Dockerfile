#######################
# Stage 1: Build Stage
#######################

FROM alpine:3.18 AS build
WORKDIR /app

# Set a build argument
ARG BUILD_TYPE=release

# Set an environment variable based on the build argument
ENV BUILD_TYPE=${BUILD_TYPE}

# Install all the dependent packages to compile cloud connect mqtt application.
RUN apk add --no-cache \
    build-base=0.5-r3 \
    cjson-dev=1.7.17-r0 \
    hiredis-dev=1.1.0-r2 \
    mosquitto-dev=2.0.18-r0


# Copy current directory to the docker /app directory.
COPY . /app

RUN make clean

# Compilation of cloud connect service
RUN if [ "$BUILD_TYPE" = "debug" ]; then \
      echo "Coverage is enabled"; \
      # Run make command to generate executable file
      make coverage; \
    else \
      echo "Production environment"; \
      make; \
    fi

 

#########################
# Stage 2: Runtime Stage
#########################

FROM alpine:3.18

WORKDIR /app

# Copy build folder from Build stage.
COPY --from=build /app/build /app/build

# Copy src folder from Build stage.
COPY --from=build /app/src /app/src

# Default value of BUILD_TYPE will be release
ARG BUILD_TYPE=release

# From environment variable, update the value of BUILD_TYPE
ENV BUILD_TYPE=${BUILD_TYPE}

# By default no command for coverage generation
ENV COV_CMD=""
ENV COPY_CMD=""

# The command for coverage report generation will be dynamicaly generated
# and will be used in CMD[] section. To make it available there we have
# to make this environment variable part of profile.
RUN echo "export COV_CMD=${COV_CMD}" >> /etc/profile.d/cov_cmd.sh;

# If BUILD_TYPE value is "debug" then install necessary packages for coverage and
# form the coverage command and make it part of profile again
RUN if [ "$BUILD_TYPE" = "debug" ]; then \
        apk update && apk add --no-cache gcc g++ python3 py3-pip; \
        pip install --upgrade pip; \
        pip install gcovr; \
        echo "export COV_CMD=\"gcovr -r . --html --html-details -o mqtt_coverage/c_mqtt_coverage.html --gcov-ignore-parse-errors\"" >> /etc/profile.d/cov_cmd.sh; \
        echo "export COPY_CMD=\"cp -r build mqtt_coverage\"" >> /etc/profile.d/cov_cmd.sh; \
    else \
        rm -rf build; \
        rm -rf src; \
    fi

#copy libs from build stage
COPY --from=build /usr/lib/ /usr/lib/

# Copy built executable file from Build stage.
COPY --from=build /app/cloud_connect_mqtt /app/cloud_connect_mqtt


# Set the CMD to execute the shell script
#Copy build to mqtt_coverage
CMD ["/bin/sh", "-c", \
    ". /etc/profile.d/cov_cmd.sh && echo $COV_CMD; \
    ./cloud_connect_mqtt; \
    mkdir -p mqtt_coverage; \
    rm -rf mqtt_coverage/*; \
    $COV_CMD; \
	#Copy build to mqtt_coverage
    $COPY_CMD; \
    chmod 777 -R mqtt_coverage;"]
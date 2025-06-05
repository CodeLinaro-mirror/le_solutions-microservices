#######################
# Stage 1: Build Stage
#######################

FROM alpine:3.18 AS build
WORKDIR /app

# Install all the dependent packages to compile cloud connect mqtt application.
RUN apk add --no-cache \
    build-base=0.5-r3 \
    cjson-dev=1.7.17-r0 \
    cpputest=4.0-r1

# Copy current directory to the docker /app directory.
COPY . /app

RUN make clean

# clean UT and IT
RUN cd test && make clean

# Run command to generate UT executable file
RUN cd test && make coverage

#########################
# Stage 2: Runtime Stage
#########################

FROM alpine:3.18

WORKDIR /app

# Copy build folder from Build stage.
COPY --from=build /app/build_ut /app/build_ut

# Copy src folder from Build stage.
COPY --from=build /app/src /app/src

# Copy src folder from Build stage.
COPY --from=build /app/test /app/test

# If coverage flag is enabled then install necessary packages for coverage and
# form the coverage command and make it part of profile again
RUN apk update && apk add --no-cache gcc g++ python3 py3-pip; \
    pip install --upgrade pip; \
    pip install gcovr

#copy libs from build stage
COPY --from=build /usr/lib/ /usr/lib/


# make folder if does not exist
RUN mkdir -p mqtt_ut_coverage
RUN mkdir -p mqtt_ut_report

CMD ["/bin/sh", "-c", \
    "rm -rf mqtt_ut_coverage/*; \
    rm -rf mqtt_ut_report/*; \
    ./test/run_tests -ojunit; \
    cp *.xml mqtt_ut_report; \
    rm -rf build_ut/*test*; rm -rf build_ut/*mock*; rm -rf build_ut/*main*; \
    gcovr -r . --html --html-details -o mqtt_ut_coverage/index.html --gcov-ignore-parse-errors; \
    cp -r build_ut mqtt_ut_coverage; \
    chmod 777 -R mqtt_ut_report; \
    chmod 777 -R mqtt_ut_coverage;"]
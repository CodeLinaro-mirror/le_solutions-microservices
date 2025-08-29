## Microservices present in compose
There are following microservices present in compose
1. **c-ccm**: A C based MQTT client that grabs data from the Redis channel and sends it to the MQTT broker.
2. **c-cck**: A C based KAFKA client that grabs data from the Redis channel and sends it to the KAFKA broker.
3. **node-ccm**: A Node js based MQTT client that grabs data from the Redis channel and sends it to the MQTT broker.
4. **node-cck**: A Node js based KAFKA client that grabs data from the Redis channel and sends it to the KAFKA broker.
5. **c-ccm-ut**: Unit tests for c-ccm microservice.
6. **c-cck-ut**: Unit tests for c-cck microservice..
7. **node-ccm-ut**: Unit tests for node-ccm microservice.
8. **node-cck-ut**: Unit tests for node-cck microservice.

### Setting up environment Variables
Update all environment variables present in the `.env` file with correct values. Here is a quick overview of all environment variables -
* **CERTIFICATES_DIR** : In case of secure communication you need to mount the path of certificate directory from your host machine using this environment variable
* **CONFIG_DIR** : To provide configuration file to Cloud Connect microservice you need to mount the directory where config.json is placed on your host machine.
* **TARGET_ARCH** : Based on the platform you are going to deploy the cloud connect microservice you need to set this environment variable as linux/arm64 or linux/amd64
* **BUILD_TYPE** : To generate a release build image you need to set the value as "release". To generate the debug build image in which the code-coverage will be enabled you need to set value as "debug".
* **IT_COVERAGE_DIR** : If build type is set as debug then you can mount a directory from your host machine where you will get the code coverage of cloud connect microservice.
* **UT_COVERAGE_DIR** : If you are running the one of the unit tests microservices then you need to mount a directory where you will have unit test coverage report for cloud connect microservice.
* **UT_REPORTS_DIR** : If you are running the one of the unit tests microservices then you need to mount a directory where you will have unit test report for cloud connect microservice.
* **MQTT_IMAGE** : You can point it to a local build image or you can point it to download image from docker registry for MQTT image
* **KAFKA_IMAGE** : You can point it to a local build image or you can point it to download image from docker registry for KAFKA image
* **REDIS_HOST** : Assign the host/IP of your redis server.
* **REDIS_PORT** : Assign the port of your redis server.
* **BROKER_HOST** : Assign the host/IP of your broker(mosquitto/kafka).
* **BROKER_PORT** : Assign the port on which the broker(mosquitto/kafka) is running.

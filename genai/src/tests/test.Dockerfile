#######################
# Stage 1: Build Stage
#######################

FROM python:3.10.12 AS builder
WORKDIR /usr/src/app

# create python virtual environment
RUN python3 -m venv /venv
ENV PATH="/venv/bin:$PATH"

RUN pip install --upgrade pip

# copy source code
COPY . .

# package folder name can not contain the hyphen(-) so replacing it with underscore(_)
RUN mv web-api/openapi-server web-api/openapi_server
RUN mv web-api/openapi_server/impl/genie-wrapper/ web-api/openapi_server/impl/genie_wrapper/

# install web-api server as python package
RUN pip install --no-cache-dir .

# install test depepndencies
RUN pip install pytest==8.4.0
RUN pip install pytest-cov==6.2.0
RUN pip install coverage==7.9.1

#########################
# Stage 2: Runtime Stage
#########################

FROM python:3.10.12 AS service
WORKDIR /usr/src/app

# copy the virtual environment with web-api package installed with dependency
COPY --from=builder /venv /venv

# copy the test sources
COPY --from=builder /usr/src/app/web-api/tests /root/tests

# copy genai interface file
COPY --from=builder /usr/src/app/web-api/openapi_server/impl/genie_wrapper/include/genai_interface.h /root/app/site-packages/

ENV PATH=/venv/bin:$PATH

RUN cd /root/tests/mock_lib &&  /bin/make clean && /bin/make && \
              mkdir -p /root/app/site-packages/test/ && \
              chmod 777 -R /root/app/site-packages/test/ && \
              mkdir -p /root/app/site-packages/report/html && \
              cp libllmservice.so /root/app/site-packages/test/  && \
              cd /root/app/site-packages
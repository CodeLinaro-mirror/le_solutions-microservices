#  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Multi-stage Dockerfile for Audio Analytics
# Builds both API (Node.js 18+) and Server (Python) in a single image
# For blackbox mode (API + Server in single container)

ARG INSTALL_NATIVE_LIBS=true
ARG INSTALL_PULSE_AUDIO=false
#ARG DISTRO=debian

#ARG DEBIAN_VERSION=trixie
#FROM docker-registry.qualcomm.com/library/debian:${DEBIAN_VERSION} AS base

ARG DISTRO=ubuntu
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION} AS base

# Install common dependencies
# NOTE: curl is only needed in builder stages; runtime uses python3 for healthcheck
RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y --no-install-recommends \
    curl \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Conditionally install PulseAudio / PortAudio libs (required for microphone and speaker support)
# Pass --build-arg INSTALL_PULSE_AUDIO=true to enable
ARG INSTALL_PULSE_AUDIO=false
RUN if [ "$INSTALL_PULSE_AUDIO" = "true" ]; then \
      apt-get update && \
      apt-get install -y --no-install-recommends \
        libpulse0 \
        portaudio19-dev \
        libportaudio2 \
        pulseaudio-utils \
        pulseaudio && \
      rm -rf /var/lib/apt/lists/*; \
    fi

# ============================
# Stage: Native Libraries (ARM64 only)
# ============================
FROM base AS native-libs

# Inherit the global default or --build-arg
ARG INSTALL_NATIVE_LIBS=true

# Tools for download/unzip
RUN if [ "$INSTALL_NATIVE_LIBS" = "true" ]; then \
    apt-get update && \
    apt-get install -y wget unzip && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*; \
    fi

ENV VLM_BUILD_DIR="/mnt/work/build"
ENV VLM_DEPLOY_DIR="/mnt/work/deploy"
ENV VLM_ADSP_DIR=${VLM_DEPLOY_DIR}/usr/lib/rfsa/adsp
ENV VLM_QNP_VER=2.46.0.260424
#ENV VLM_QNP_VER=2.45.0.260326
#ENV VLM_QNP_VER=2.40.0.251030
ENV VLM_DOWNLOAD_DIR="/mnt/work/downloads"
ENV VLM_QNP_SDK=v${VLM_QNP_VER}.zip
# Expected unzip layout (adjust if archive differs)
ENV VLM_QNN_ROOT=${VLM_DOWNLOAD_DIR}/qairt/${VLM_QNP_VER}
ENV VLM_QNN_LIB_PATH=${VLM_QNN_ROOT}/lib

# Show variables
RUN echo "INSTALL_NATIVE_LIBS=${INSTALL_NATIVE_LIBS}" && \
    echo "VLM_QNP_VER=${VLM_QNP_VER}" && \
    echo "VLM_QNP_SDK=${VLM_QNP_SDK}" && \
    echo "VLM_QNN_ROOT=${VLM_QNN_ROOT}" && \
    echo "VLM_QNN_LIB_PATH=${VLM_QNN_LIB_PATH}"

# Ensure structure for downstream COPY
RUN mkdir -p ${VLM_DEPLOY_DIR}/usr/lib ${VLM_ADSP_DIR} /usr/include /usr/lib ${VLM_DOWNLOAD_DIR}

# Install and VALIDATE native libs
RUN set -eu && \
    echo "INSTALL_NATIVE_LIBS=${INSTALL_NATIVE_LIBS}" && \
    if [ "${INSTALL_NATIVE_LIBS}" = "true" ]; then \
      echo "==> Clean previous artifacts"; \
      rm -rf "${VLM_DOWNLOAD_DIR}/qairt" || true; \
      rm -f  "${VLM_DOWNLOAD_DIR}/${VLM_QNP_SDK}" || true; \
      \
      echo "==> Download QAIRT ${VLM_QNP_VER}"; \
      wget -t 3 -T 300 -P "${VLM_DOWNLOAD_DIR}" \
        "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${VLM_QNP_VER}/${VLM_QNP_SDK}"; \
      \
      echo "==> Unzip SDK"; \
      cd "${VLM_DOWNLOAD_DIR}" && unzip -q "${VLM_QNP_SDK}" && rm -f "${VLM_QNP_SDK}"; \
      \
      echo '==> Validate expected directory layout'; \
      test -d "${VLM_QNN_ROOT}" || { echo "ERROR: ${VLM_QNN_ROOT} missing"; ls -la "${VLM_DOWNLOAD_DIR}/qairt" || true; exit 1; }; \
      test -d "${VLM_QNN_LIB_PATH}" || { echo "ERROR: ${VLM_QNN_LIB_PATH} missing"; ls -la "${VLM_QNN_ROOT}" || true; exit 1; }; \
      \
      echo '==> Copy aarch64 QNN libs to deploy'; \
      cp -v "${VLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/"* "${VLM_DEPLOY_DIR}/usr/lib/"; \
      \
      echo '==> Copy ADSP (Hexagon) unsigned binaries'; \
      for hv in hexagon-v73 hexagon-v75; do \
        if [ -d "${VLM_QNN_LIB_PATH}/${hv}/unsigned" ]; then \
          cp -v "${VLM_QNN_LIB_PATH}/${hv}/unsigned/"* "${VLM_ADSP_DIR}/"; \
        else \
          echo "WARN: ${hv}/unsigned not found for ${VLM_QNP_VER}"; \
        fi; \
      done; \
      \
      echo '==> Validate ADSP dir non-empty and contains .so'; \
      test -d "${VLM_ADSP_DIR}" && test "$(find "${VLM_ADSP_DIR}" -type f -name '*.so' | wc -l)" -ge 1 || { \
        echo "ERROR: No ADSP .so files copied into ${VLM_ADSP_DIR}"; exit 1; \
      }; \
      \
      echo "==> QAIRT ${VLM_QNP_VER} install and validation complete"; \
    else \
      echo "Skipping native libs install (INSTALL_NATIVE_LIBS!=true)"; \
    fi

# Debug: show ADSP dir
RUN if [ "$INSTALL_NATIVE_LIBS" = "true" ]; then \
      echo "Contents of ${VLM_ADSP_DIR}:" && ls -al "${VLM_ADSP_DIR}" || true ; \
    fi

# Fail-fast: ADSP sanity (sprpc/skel presence optional)
RUN if [ "$INSTALL_NATIVE_LIBS" = "true" ]; then \
      if ! find "${VLM_ADSP_DIR}" -type f -name "*.so" | grep -q . ; then \
        echo "ERROR: No .so files in ${VLM_ADSP_DIR}"; exit 1; \
      fi; \
      if ! (find "${VLM_ADSP_DIR}" -type f \( -name "*sprpc*.so" -o -name "*skel*.so" \) | grep -q .) ; then \
        echo "WARNING: No *sprpc*/*skel* libs detected in ${VLM_ADSP_DIR}. Continuing."; \
      fi; \
    fi


# ============================
# Stage: Build API (Node.js)
# ============================
# node:22-slim is not available in the internal registry, so we install via
# nodesource - but ONLY in this builder stage. The runtime stage copies just
# the node binary + app files across, so the ~288 MB install never lands in
# the final image.
# Inherits from base to reuse the already-pulled ubuntu:24.04 image and apt-get upgrade layer.
FROM base AS api-builder

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      curl ca-certificates gnupg && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash -

RUN apt-get update && \
    apt-get install -y --no-install-recommends nodejs && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN node --version

RUN userdel -r ubuntu 2>/dev/null || true && \
    useradd -ms /bin/bash -u 1000 iot-user && \
    mkdir -p /usr/src/api && \
    chown -R iot-user:iot-user /usr/src/api
USER iot-user
WORKDIR /usr/src/api
COPY --chown=iot-user:iot-user audio-analytics-api/package*.json ./
RUN npm install --omit=dev
COPY --chown=iot-user:iot-user audio-analytics-api/data ./data



# ============================
# Stage: Build Server (Python)
# ============================
FROM base AS server-builder

ARG INSTALL_NATIVE_LIBS
ARG INSTALL_PULSE_AUDIO=false
ARG VERSION_TAG=1.0.3
ARG ARTIFACTORY_BASE_URL=https://artifacts.codelinaro.org/artifactory/clo-561-iot-solutions-microservices-test-generic/audio-analytics
ARG JFROG_USERNAME
ARG JFROG_PASSWORD


# Install default Python 3 packages
RUN apt-get update --fix-missing && \
    apt-get install -y \
    wget \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Create python virtual environment
RUN python3 -m venv /venv
ENV PATH="/venv/bin:$PATH"

RUN pip install --upgrade pip
RUN userdel -r ubuntu 2>/dev/null || true && \
    useradd -ms /bin/bash -u 1000 iot-user
# Create virtual environment and install requirements as root
WORKDIR /usr/src/server
COPY audio-analytics-server/app/requirements.txt ./
COPY audio-analytics-server/app/requirements-audio.txt ./
RUN pip install -r requirements.txt && \
    if [ "$INSTALL_PULSE_AUDIO" = "true" ]; then \
      echo "==> Installing audio device packages (sounddevice/cffi/pycparser)"; \
      pip install -r requirements-audio.txt; \
    else \
      echo "==> Skipping audio device packages (INSTALL_PULSE_AUDIO!=true)"; \
    fi && \
    python3 -c "import site; print('Python packages installed at:', site.getsitepackages())"

# Change ownership of the server directory to iot-user
RUN chown -R iot-user:iot-user /usr/src/server

COPY --chown=iot-user:iot-user audio-analytics-server/app ./

# Copy Python wrapper files to server directory (same location as .so files)
USER root
RUN mkdir -p /usr/src/engine/python /opt/audio/models /usr/src/engine/native /usr/src/engine/models/whisper
COPY --chown=iot-user:iot-user audio-analytics-server/engine/python/*.py /usr/src/server/

# Download melo & piper files from artifactory
RUN wget -r -np -nH --cut-dirs=6 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/server/melo" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/python/melo/"
RUN wget -r -np -nH --cut-dirs=6 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/server/piper" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/python/piper/"
# Keep the COPY as these folders still exist and have .py source code files
COPY --chown=iot-user:iot-user audio-analytics-server/engine/python/melo /usr/src/server/melo
COPY --chown=iot-user:iot-user audio-analytics-server/engine/python/piper /usr/src/server/piper
RUN chown -R iot-user:iot-user /usr/src/server/melo
RUN chown -R iot-user:iot-user /usr/src/server/piper

# Move .pyc files directly into their package dir (keeping cpython tag), remove __pycache__ and stub .py files.
# Python can load .pyc files placed directly in the package directory without a .py source file.
RUN find /usr/src/server/melo -name '__pycache__' | while read cache_dir; do \
      pkg_dir=$(dirname "$cache_dir"); \
      for pyc in "$cache_dir"/*.pyc; do \
        [ -f "$pyc" ] || continue; \
        base=$(basename "$pyc" | sed 's/\.cpython-[0-9]*\.pyc$/.pyc/'); \
        mv "$pyc" "$pkg_dir/$base"; \
      done; \
      rm -rf "$cache_dir"; \
    done && \
    find /usr/src/server/melo -mindepth 2 -name '*.py' ! -name '__init__.py' -delete

# Download .so files & whisper speech float files from artifactory
RUN wget -r -np -nH --cut-dirs=6 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/engine/native/" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/native/aarch64-oe-linux/"
RUN wget -r -np -nH --cut-dirs=5 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/engine/models/whisper/" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/whisper/"
# Comment out COPYs when pulling from Artifactory
# COPY --chown=root:root audio-analytics-server/engine/native/aarch64-oe-linux/*.so /usr/src/engine/native/
# COPY --chown=iot-user:iot-user audio-analytics-server/engine/models/whisper/ /usr/src/engine/models/whisper/
# COPY --chown=iot-user:iot-user audio-analytics-server/engine/config/models.json /opt/audio/models/models.json
RUN chown -R root:root /usr/src/engine/native/
RUN chown -R iot-user:iot-user /usr/src/engine/models/whisper/

# Download Notice and License files from artifactory
RUN wget -r -np -nH --cut-dirs=4 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/server/" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/NO.LOGIN.BINARY.LICENSE.QTI.pdf"
RUN wget -r -np -nH --cut-dirs=4 --reject "index.html*" \
   --user="${JFROG_USERNAME}" --password="${JFROG_PASSWORD}" \
   -P "/usr/src/server/" \
     "${ARTIFACTORY_BASE_URL}/${VERSION_TAG}/NOTICE.FOR.AUDIO.ANALAYTICS.BINARIES.txt"
RUN chown -R iot-user:iot-user /usr/src/server/

# ============================
# Stage: Build fastrpc (libcdsprpc.so + libadsprpc.so)
# ============================
# Builds fastrpc from source. Provides libcdsprpc.so and libadsprpc.so
# which are required by libtranslation.so at runtime.
# Inherits from base to reuse the already-pulled ubuntu:24.04 image and apt-get upgrade layer.
FROM base AS fastrpc-builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      git ca-certificates \
      autoconf automake libtool make gcc pkg-config \
      libyaml-dev libmd-dev libbsd-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/quic/fastrpc.git \
    && cd fastrpc \
    && autoreconf -is \
    && CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
       ./configure --prefix=/usr --libdir=/usr/lib \
    && make -j1 \
    && make install \
    && rm -rf /fastrpc

# ============================
# Stage: Final Runtime Image
# ============================
# Inherits from base to reuse the already-pulled ubuntu:24.04 image and apt-get upgrade layer.
# base already ran apt-get upgrade so we don't need to repeat it here.
FROM base AS runtime

ARG INSTALL_NATIVE_LIBS

# Single combined layer: runtime deps only.
# base already ran apt-get upgrade. No build tools, no nodesource.
# Node binary is copied from api-builder.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      python3 supervisor curl libyaml-0-2 libbsd0 libmd0 libatomic1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Conditionally install PulseAudio / PortAudio runtime libs
ARG INSTALL_PULSE_AUDIO=false
RUN if [ "$INSTALL_PULSE_AUDIO" = "true" ]; then \
      apt-get update && \
      apt-get install -y --no-install-recommends \
        libpulse0 \
        libportaudio2 \
        pulseaudio-utils \
        pulseaudio && \
      rm -rf /var/lib/apt/lists/*; \
    fi

# Copy Node.js binary from the builder stage - this is the only thing needed
# to run `node data/index.js` at runtime. No npm, no nodesource, no apt repo.
# nodesource installs node at /usr/bin/node on Debian.
COPY --from=api-builder /usr/bin/node /usr/bin/node

RUN node --version


RUN userdel -r ubuntu 2>/dev/null || true && \
    useradd -ms /bin/bash -u 1000 iot-user && \
    usermod -aG audio iot-user
RUN mkdir -p /usr/src/api /usr/src/server /app/uploads /opt/audio/models /opt/cache /tmp/audio-sockets /usr/src/engine/models/whisper && \
    chown iot-user:iot-user /usr/src/api /usr/src/server /opt/audio/models /opt/cache /var/run && \
    chmod 777 /tmp/audio-sockets /app/uploads && \
    chmod -R 777 /app

COPY --from=api-builder --chown=iot-user:iot-user /usr/src/api /usr/src/api
COPY --from=server-builder --chown=iot-user:iot-user /usr/src/server /usr/src/server
# Copy native libraries (directory always exists, only populated when INSTALL_NATIVE_LIBS=true)
COPY --from=native-libs /mnt/work/deploy/usr /usr

# Copy fastrpc libs built from source
COPY --from=fastrpc-builder /usr/lib/libcdsprpc* /usr/lib/
COPY --from=fastrpc-builder /usr/lib/libadsprpc* /usr/lib/

# Bake fastrpc DSP path config for all supported devices.
# The entrypoint copies this to /usr/share/qcom/conf.d/ at runtime
# after the bind-mount is in place, so fastrpc_shell resolves from the host.
RUN mkdir -p /etc/fastrpc && \
    { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo '# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.'; \
      echo 'machines:'; \
      echo '  Qualcomm Technologies, Inc. Monaco Monza addons:'; \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp'; \
      echo '  Qualcomm Technologies, Inc. Monaco EVK:'; \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp'; \
      echo '  Qualcomm Technologies, Inc. Addons IQ 9075 EVK:'; \
      echo '    DSP_LIBRARY_PATH: sa8775p/Qualcomm/SA8775P-RIDE/dsp'; \
      echo '  Qualcomm Technologies, Inc. IQ8 8275 Pro SKU EVK:'; \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp'; \
    } > /etc/fastrpc/hexagon-dsp-binaries.yaml

# Copy the virtual environment
COPY --from=server-builder /venv /venv

# Copy pre-built wrapper libraries for ARM64 to server directory (like temp setup)
COPY --from=server-builder --chown=iot-user:iot-user /usr/src/engine/native/*.so /usr/src/server/
COPY --from=server-builder --chown=iot-user:iot-user /usr/src/engine/models/whisper/ /usr/src/engine/models/whisper/

# Using virtual environment Python packages
ENV PYTHONPATH=/usr/src/server
ENV PATH="/venv/bin:$PATH"
# Use container libraries
ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp

RUN mkdir -p /etc/supervisor/conf.d
COPY <<'EOF' /etc/supervisor/conf.d/supervisord.conf
[supervisord]
nodaemon=true
user=iot-user
logfile=/dev/null
logfile_maxbytes=0
pidfile=/tmp/supervisord.pid

[program:server]
priority=10
command=python3 main.py
directory=/usr/src/server
user=iot-user
autostart=true
autorestart=true
stdout_logfile=/proc/1/fd/1
stdout_logfile_maxbytes=0
stderr_logfile=/proc/1/fd/2
stderr_logfile_maxbytes=0
stdout_events_enabled=true
stderr_events_enabled=true
environment=LOG_LEVEL="%(ENV_LOG_LEVEL)s",BLACKBOX_CONTAINER="%(ENV_BLACKBOX_CONTAINER)s",SOCKET_PATH="%(ENV_SOCKET_PATH)s",LD_LIBRARY_PATH="/usr/lib/rfsa/adsp:/usr/lib:/usr/src/server:/usr/src/engine/native:/mnt/work/deploy/usr/lib",PYTHONUNBUFFERED="1"

[program:api]
priority=20
command=node data/index.js
directory=/usr/src/api
user=iot-user
autostart=true
autorestart=true
startsecs=5
stopasgroup=true
killasgroup=true
stdout_logfile=/proc/1/fd/1
stdout_logfile_maxbytes=0
stderr_logfile=/proc/1/fd/2
stderr_logfile_maxbytes=0
stdout_events_enabled=true
stderr_events_enabled=true
environment=NODE_ENV="%(ENV_NODE_ENV)s",BLACKBOX_CONTAINER="%(ENV_BLACKBOX_CONTAINER)s",SOCKET_PATH="%(ENV_SOCKET_PATH)s"

EOF

EXPOSE 8085 8001 8000
# Use python3 for healthcheck — eliminates the need for curl in the runtime image
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
  CMD curl -f http://localhost:8085/audio-analytics/v1/api/health || exit 1

ENV REDIS_HOST=redis
ENV REDIS_PORT=6379
ENV LOG_LEVEL=20
ENV NODE_ENV=production
ENV BLACKBOX_CONTAINER=true
ENV SOCKET_PATH=/tmp/audio-sockets/audio-analytics.sock

# Set environment variables for native libraries
#ENV DSP_LIBRARY_PATH=/usr/src/server
#ENV LD_LIBRARY_PATH=/usr/lib/rfsa/adsp:/usr/lib:/usr/src/server

# Entrypoint — copies yaml to /usr/share/qcom/conf.d/ after bind-mount is in place,
# then sets ADSP_LIBRARY_PATH to container's skel directory before supervisord starts.
COPY <<'EOF' /entrypoint.sh
#!/bin/bash
set -e
chmod 666 /dev/fastrpc* 2>/dev/null || true
chmod -R 777 /var/log/supervisor 2>/dev/null || true
chmod 666 /proc/1/fd/1 /proc/1/fd/2 2>/dev/null || true
mkdir -p /usr/share/qcom/conf.d
cp /etc/fastrpc/hexagon-dsp-binaries.yaml /usr/share/qcom/conf.d/audio-analytics-hexagon-dsp-binaries.yaml
export DSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
export ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
export CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
exec /usr/bin/supervisord -c /etc/supervisor/conf.d/supervisord.conf
EOF
RUN chmod +x /entrypoint.sh

CMD ["/entrypoint.sh"]
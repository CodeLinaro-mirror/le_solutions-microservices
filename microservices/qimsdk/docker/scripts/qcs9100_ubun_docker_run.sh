# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

docker run -it -d --net host --device qualcomm.com/device=cdi-hw-acc -e XDG_RUNTIME_DIR=/run/user/1000 -e WAYLAND_DISPLAY=wayland-1 -e GST_DEBUG_NO_COLOR=1 -e GST_DEBUG=2 -e ADSP_LIBRARY_PATH="/usr/lib/dsp/cdsp;/usr/lib/dsp/cdsp1;${ADSP_LIBRARY_PATH}" -e GST_PLUGIN_SCANNER="/usr/lib/aarch64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner" -h qimsdk --user ubuntu --name qimsdk qimsdk

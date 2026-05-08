#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

export CC="aarch64-linux-gnu-gcc  -march=armv8.2-a+crypto -mbranch-protection=standard -fstack-protector-strong  -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --sysroot=${TARGET_SYSROOT}"
export CXX="aarch64-linux-gnu-g++  -march=armv8.2-a+crypto -mbranch-protection=standard -fstack-protector-strong  -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --sysroot=${TARGET_SYSROOT}"
export CPP="aarch64-linux-gnu-gcc -E  -march=armv8.2-a+crypto -mbranch-protection=standard -fstack-protector-strong  -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --sysroot=${TARGET_SYSROOT}"
export AS="aarch64-linux-gnu-as "
export LD="aarch64-linux-gnu-ld  --sysroot=${TARGET_SYSROOT}"
export GDB=aarch64-linux-gnu-gdb
export STRIP=aarch64-linux-gnu-strip
export RANLIB=aarch64-linux-gnu-ranlib
export OBJCOPY=aarch64-linux-gnu-objcopy
export OBJDUMP=aarch64-linux-gnu-objdump
export READELF=aarch64-linux-gnu-readelf
export AR=aarch64-linux-gnu-ar
export NM=aarch64-linux-gnu-nm
export M4=m4
export TARGET_PREFIX=aarch64-linux-gnu-
export CONFIGURE_FLAGS="--target=aarch64-linux-gnu --host=aarch64-linux-gnu --build=x86_64-linux --with-libtool-sysroot=${TARGET_SYSROOT}"
export CFLAGS=" -O2 -pipe -g -feliminate-unused-debug-types "
export CXXFLAGS=" -O2 -pipe -g -feliminate-unused-debug-types "
export LDFLAGS="-Wl,-O1 -Wl,--hash-style=gnu -Wl,--as-needed  -Wl,-z,relro,-z,now"
export CPPFLAGS=""
export KCFLAGS="--sysroot=${TARGET_SYSROOT}"
export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH}"

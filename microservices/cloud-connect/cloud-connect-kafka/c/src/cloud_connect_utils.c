/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_utils.c

DESCRIPTION
    Implementation file for util functions.
*/
#include <stdlib.h>
#include <string.h>

#include "cloud_connect_utils.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t src_len = strlen(src);

    if (size > 0) {
        size_t copy_len = (src_len >= size) ? size - 1 : src_len;
        memcpy(dst, src, copy_len);
        dst[copy_len] = '\0';
    }

    return src_len;
}
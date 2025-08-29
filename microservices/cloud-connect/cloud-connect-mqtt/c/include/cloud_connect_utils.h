/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_utils.h

DESCRIPTION
    Header file for the util functions.
*/

#ifndef CLOUD_CONNECT_UTILS_H_
#define CLOUD_CONNECT_UTILS_H_

/**
 * @brief Copies a string from src to dst ensuring null-termination.
 *
 * This function copies up to size-1 characters from the NUL-terminated string src to dst,
 * NUL-terminating the result. If size is 0, dst is not modified.
 *
 * @param dst Destination buffer where the string will be copied.
 * @param src Source string to be copied.
 * @param size Size of the destination buffer.
 * @return The total length of the string src. If the return value is >= size, truncation occurred.
 *
 * @note If size is 0, the function returns the length of src without modifying dst.
 * @note The function ensures that the destination string is always NUL-terminated unless size is 0.
 */
size_t strlcpy(char *dst, const char *src, size_t size);

#endif /* CLOUD_CONNECT_UTILS_H_ */
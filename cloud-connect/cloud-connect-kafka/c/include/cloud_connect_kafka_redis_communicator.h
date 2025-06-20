/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_kafka_redis_communicator.h

DESCRIPTION
    Header file for the redis communicator module of Cloud Connect kafka service.
*/

#ifndef CLOUD_CONNECT_KAFKA_REDIS_COMMUNICATOR_H_
#define CLOUD_CONNECT_KAFKA_REDIS_COMMUNICATOR_H_

#include "cloud_connect_kafka_config.h"

/**
 * @fn cloud_connect_kafka_init_redis_communicator(cck_sys_config_t *config)
 * @brief Function to initialize the redis communicator module of the cloud connect kafka microservice.
 *
 * @param config cck_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_init_redis_communicator(cck_sys_config_t *config);

/**
 * @fn cloud_connect_kafka_deinit_redis_communicator(cck_sys_config_t *config)
 * @brief Function to deinitialize the redis communicator module of the cloud connect kafka microservice.
 *
 * @param config cck_sys_config_t structure pointer
 * @return int
 * @retval 0: Operation success
 * @retval non zero: operation fail
 */
int cloud_connect_kafka_deinit_redis_communicator(cck_sys_config_t *config);

#endif /* CLOUD_CONNECT_KAFKA_REDIS_COMMUNICATOR_H_ */
/****************************************************************************
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear

FILE NAME
    cloud_connect_mqtt_status.h

DESCRIPTION
    Header file for the different status code of Cloud Connect MQTT service.
*/

#ifndef CLOUD_CONNECT_MQTT_STATUS_H_
#define CLOUD_CONNECT_MQTT_STATUS_H_

/**
 * @enum LogLevel
 * @brief Enum for different error codes
 */
typedef enum
{
    CCM_STATUS_FAIL = -1,            /*!< Status error */
    CCM_STATUS_SUCCESS = 0,          /*!< Status success */
    CCM_STATUS_ERROR_NO_MAPPING = 1, /*!< Status msg sending failed due to mapping not found */
    /* Add more status code here as per your implementation */
} CCMStatus;

/**
 * @enum MQTT connection status
 * @brief Enum for different error codes of connection
 */
typedef enum
{
    CCM_CONN_STATUS_ERROR = -1,  /*!< Status error */
    CCM_CONN_STATUS_SUCCESS = 0, /*!< Status success */
    CCM_CONN_STATUS_CLOSED = 2,  /*!< Status conn closed */

    /* Add more connection status code here as per your implementation */

} CCMConnectionStatus;

#endif /* CLOUD_CONNECT_MQTT_STATUS_H_ */

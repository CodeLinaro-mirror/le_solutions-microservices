// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <grpcpp/grpcpp.h>
#include "qai_forge/InternalDTOs.h"  // GenAIErrorCode

namespace grpc_util {

grpc::StatusCode MapErrorCode(GenAIErrorCode code);

} // namespace grpc_util

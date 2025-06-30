/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 * File :- main.cpp
 * Description :- main function file in unit test demo
 */
#include "CppUTest/CommandLineTestRunner.h"
#include <CppUTest/TestHarness.h>
#include <CppUTest/MemoryLeakWarningPlugin.h>
#include <CppUTest/TestRegistry.h>

int main(int argc, char **argv)
{
    // Turn off new/delete overloads
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();

    // Optionally, remove the MemoryLeakWarningPlugin from the registry
    TestRegistry::getCurrentRegistry()->removePluginByName("MemoryLeakWarningPlugin");

    return CommandLineTestRunner::RunAllTests(argc, argv);
}

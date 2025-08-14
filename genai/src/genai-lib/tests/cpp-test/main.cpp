//=============================================================================
//
//  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
//  SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "llm-service.hpp"

#include <iostream>

int main(int argc, char** argv) {
    std::string model;
    std::cout << "What model will be used? 0 for LLAMA3_1_8B, 1 for LLAMA3_2_3B, "
            << "and 2 for QWEN2_5_7B: ";
    std::string choiceString;
    std::getline(std::cin, choiceString);
    int choice = std::stoi(choiceString);
    switch(choice)
    {
        case 0:
            model = "LLAMA3_1_8B";
            break;

        case 1:
            model = "LLAMA3_2_3B";
            break;

        case 2:
            model = "QWEN2_5_7B";
            break;

        default:
            std::cout << "ERROR Unsupported model selected" << std::endl;
            break;
        }
    std::cout << std::endl;

    LLMObject obj(model);

    while(1)
    {
        Message message;
        strlcpy(message.role, "user", sizeof(message.role));

        std::string userQuery;
        std::cout << "Enter your prompt: ";
        std::getline(std::cin, userQuery);
        std::cout << std::endl;

        strlcpy(message.content, userQuery.c_str(), sizeof(message.content));

        Query inputQuery;
        inputQuery.message = message;
        strlcpy(inputQuery.model, "LLAMA3_1_8B", sizeof(inputQuery.model));

        *(obj.query) = inputQuery;

        Response output;

        std::cout << "Using libGenie.so version " << Genie_getApiMajorVersion() << "."
                            << Genie_getApiMinorVersion() << "." << Genie_getApiPatchVersion()
                            << "\n"
                            << std::endl;

        try {

            //Here is where Query is sent to LLM to generate Response
            obj.chat_completion_create();

            output = *(obj.response);

            std::cout << "Answer: " << output.choices[0].message.content << std::endl;

            obj.profiler->getJsonData();

        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
            return EXIT_FAILURE;
        }

        //return EXIT_SUCCESS;
    }
    return EXIT_SUCCESS;
}

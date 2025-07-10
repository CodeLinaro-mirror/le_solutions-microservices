//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#ifndef LLM_SERVICE_H
#define LLM_SERVICE_H

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <cstring>

#include <mutex>
#include <condition_variable>

#include "GenieCommon.h"
#include "GenieDialog.h"
#include "GenieProfile.h"

#include "llm-buffer.h"

typedef struct {
    std::mutex* mtx;
    std::condition_variable* cv;
    bool* queryDone;
    std::string* responseStr;
} QueryMutex;

class Profile
{
    public:
        std::string profilePath = "profile.txt";
        Profile();
        ~Profile();
        void getJsonData();
        GenieProfile_Handle_t getProfileHandle() { return m_handle; }

    private:
        GenieProfile_Handle_t m_handle = NULL;
};

class Dialog
{
    public:
        //Config Class nested inside Dialog
        class Config
        {
            public:
                Config(const std::string& config, std::shared_ptr<Profile> profile);
                ~Config();
                // Disable both copying and moving
                Config(const Config&) = delete;
                Config& operator=(const Config&) = delete;
                Config(Config&&) = delete;
                Config& operator=(Config&&) = delete;
                GenieDialogConfig_Handle_t operator()() const { return m_handle; }
                GenieDialogConfig_Handle_t getHandle() { return m_handle; }
            private:
                GenieDialogConfig_Handle_t m_handle   = NULL;
                GenieProfile_Handle_t m_profileHandle = NULL;
        };
        Dialog(Config config);
        ~Dialog();
        // Disable both copying and moving
        Dialog(const Dialog&) = delete;
        Dialog& operator=(const Dialog&) = delete;
        Dialog(Dialog&&) = delete;
        Dialog& operator=(Dialog&&) = delete;
        void query(const std::string prompt, GenieDialog_SentenceCode_t sentencCode,
        void* userData);
        void save(const std::string name);
        void restore(const std::string name);

        static void queryCallback(const char* responseStr,
            const GenieDialog_SentenceCode_t sentenceCode,
            const void* userData);
    private:
        GenieDialog_Handle_t m_handle         = NULL;
};


class LLMObject
{
    public:

        //Enum for LLM Model Selection
        enum class LLMModel: int
        {
            LLAMA3_1_8B = 0,
            LLAMA3_2_3B = 1,
            QWEN2_5_7B = 2,
            UNKNOWN = -1
        };

        LLMObject(std::string model);
        ~LLMObject() {delete diag;}
        // Disable both copying and moving
        LLMObject(const LLMObject&) = delete;
        LLMObject& operator=(const LLMObject&) = delete;
        LLMObject(LLMObject&&) = delete;
        LLMObject& operator=(LLMObject&&) = delete;

        std::unique_ptr<Query> query;
        std::unique_ptr<Response> response;
        std::shared_ptr<Profile> profiler;

        char id[256];

        void chat_completion_create ();
        void chat_completion_retrieve ();
        void chat_completion_list ();
        void chat_completion_delete ();
        void chat_completion_messages_list();
    private:
        Dialog *diag;
        std::string config{};
        std::string prompt{};
        std::string savePath{};
        std::string restorePath{};
        std::string profilePath;
        std::vector<Message> conversation;

        void constructPrompt(const std::string query, LLMModel model);

        LLMModel getModelFromQuery(const std::string& model);

};

#endif// LLM_SERVICE_H

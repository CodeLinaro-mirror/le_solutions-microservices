// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Logger — Environment-driven log-level filter
//
// Usage
// ─────
//   #include "qai_forge/utils/Logger.h"
//
//   LOG_DEBUG("[Worker] prompt length = " << prompt.size());
//   LOG_INFO ("[Worker] Started PID " << pid << " for model " << model_id);
//   LOG_WARN ("[Session] Context near limit: " << tokens << " tokens");
//   LOG_ERROR("[Worker] execl failed: " << strerror(errno));
//
// Environment variable
// ────────────────────
//   LOG_LEVEL=TRACE|DEBUG|INFO|WARN|ERROR|OFF   (default: INFO)
//
// The singleton is initialised the first time Logger::instance() is called,
// which happens on the first LOG_* macro invocation.  No explicit init call
// is required, but you may call qai::Logger::instance().setLevel(...) at
// runtime to change the level programmatically.
//
// Output format (to stderr)
// ─────────────────────────
//   [2026-05-21 23:51:14.123] [INFO ] [SessionManager.cpp:87] message text
//
// Thread safety
// ─────────────
//   All log calls are serialised through an internal std::mutex.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace qai {

// ── Log levels ────────────────────────────────────────────────────────────────
enum class LogLevel : int {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    OFF   = 5   // disables all output
};

// ── Logger singleton ──────────────────────────────────────────────────────────
class Logger {
public:
    // Returns the process-wide singleton, initialised from LOG_LEVEL on first call.
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // Returns true when messages at `level` should be emitted.
    bool isEnabled(LogLevel level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(level_);
    }

    // Emit a log line to stderr.  Called by the LOG_* macros.
    void log(LogLevel level, const char* file, int line, const std::string& msg) {
        if (!isEnabled(level)) return;

        // Format timestamp
        using clock = std::chrono::system_clock;
        auto now    = clock::now();
        auto now_t  = clock::to_time_t(now);
        auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &now_t);
#else
        localtime_r(&now_t, &tm_buf);
#endif

        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr
            << '[' << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setw(3) << std::setfill('0') << ms.count() << "] "
            << '[' << levelStr(level) << "] "
            << '[' << basename(file) << ':' << line << "] "
            << msg << '\n';
    }

    // Runtime level override (e.g. from a --log-level CLI flag).
    void setLevel(LogLevel level) noexcept { level_ = level; }
    LogLevel getLevel() const noexcept { return level_; }

    // Parse and apply the LOG_LEVEL environment variable.
    // Called automatically by the constructor; exposed for testing.
    static LogLevel parseEnvLevel() noexcept {
        const char* env = std::getenv("LOG_LEVEL");
        if (!env) return LogLevel::INFO;

        // Case-insensitive compare via a local uppercase copy
        std::string s(env);
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (s == "TRACE") return LogLevel::TRACE;
        if (s == "DEBUG") return LogLevel::DEBUG;
        if (s == "INFO")  return LogLevel::INFO;
        if (s == "WARN" || s == "WARNING") return LogLevel::WARN;
        if (s == "ERROR") return LogLevel::ERROR;
        if (s == "OFF")   return LogLevel::OFF;

        // Unknown value — warn and fall back to INFO
        std::cerr << "[Logger] Unknown LOG_LEVEL='" << env
                  << "'. Valid values: TRACE DEBUG INFO WARN ERROR OFF. Defaulting to INFO.\n";
        return LogLevel::INFO;
    }

private:
    Logger() : level_(parseEnvLevel()) {}

    // Non-copyable / non-movable
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    // Fixed-width level label (5 chars) for aligned output
    static const char* levelStr(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            default:              return "?????";
        }
    }

    // Return just the filename component of a full __FILE__ path
    static const char* basename(const char* path) noexcept {
        const char* last_slash = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '/' || *p == '\\') last_slash = p + 1;
        }
        return last_slash;
    }

    LogLevel           level_;
    mutable std::mutex mutex_;
};

} // namespace qai

// ─────────────────────────────────────────────────────────────────────────────
// Convenience macros — stream-style, zero-cost when level is disabled
//
//   LOG_INFO("[Worker] Started PID " << pid << " for model " << model_id);
//
// The ostringstream is only constructed when the level is active, so there is
// no formatting overhead for suppressed messages.
// ─────────────────────────────────────────────────────────────────────────────

#define QAI_LOG_(level, msg)                                                    \
    do {                                                                        \
        if (::qai::Logger::instance().isEnabled(::qai::LogLevel::level)) {     \
            std::ostringstream _qai_oss_;                                       \
            _qai_oss_ << msg;                                                   \
            ::qai::Logger::instance().log(                                      \
                ::qai::LogLevel::level, __FILE__, __LINE__, _qai_oss_.str());   \
        }                                                                       \
    } while (0)

// Undefine Trantor's logging macros if they exist (from Drogon framework).
// Trantor uses stream-style syntax (LOG_INFO << "msg"), while QAI uses
// parenthesized syntax (LOG_INFO("msg")). We must undefine Trantor's macros
// to ensure QAI's macros are used throughout the codebase.
#ifdef LOG_TRACE
#undef LOG_TRACE
#endif

#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif

#ifdef LOG_INFO
#undef LOG_INFO
#endif

#ifdef LOG_WARN
#undef LOG_WARN
#endif

#ifdef LOG_ERROR
#undef LOG_ERROR
#endif

// Now define QAI's logging macros with parenthesized syntax
#define LOG_TRACE(msg) QAI_LOG_(TRACE, msg)
#define LOG_DEBUG(msg) QAI_LOG_(DEBUG, msg)
#define LOG_INFO(msg)  QAI_LOG_(INFO,  msg)
#define LOG_WARN(msg)  QAI_LOG_(WARN,  msg)
#define LOG_ERROR(msg) QAI_LOG_(ERROR, msg)

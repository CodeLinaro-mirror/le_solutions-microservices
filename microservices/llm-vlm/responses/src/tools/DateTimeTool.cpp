// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "tools/DateTimeTool.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <array>
#include <cstring>

std::string DateTimeTool::name() const {
    return "datetime";
}

std::string DateTimeTool::description() const {
    return
        "Get the current date, time, day of week, and Unix timestamp. "
        "Use this tool when the user asks about: the current time, today's date, "
        "what day of the week it is, the current year or month, or any question "
        "that requires knowing the current date/time. "
        "Optionally accepts a timezone argument (IANA format, e.g. "
        "'America/Los_Angeles', 'Europe/London', 'Asia/Tokyo'). "
        "Defaults to UTC if no timezone is provided or if the timezone is unsupported.";
}

json DateTimeTool::inputSchema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"timezone", {
                {"type", "string"},
                {"description",
                    "IANA timezone name (e.g. 'America/Los_Angeles', 'Europe/London', "
                    "'Asia/Tokyo', 'UTC'). Optional — defaults to UTC."}
            }}
        }},
        {"required", json::array()}
    };
}

McpToolResult DateTimeTool::execute(const json& arguments) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);

    std::string timezone = "UTC";
    bool use_local = false;

    if (arguments.contains("timezone") && arguments["timezone"].is_string()) {
        std::string tz_arg = arguments["timezone"].get<std::string>();
        if (!tz_arg.empty() && tz_arg != "UTC" && tz_arg != "utc") {
            timezone = tz_arg;
            use_local = true;
        }
    }

    struct tm time_info;
    std::memset(&time_info, 0, sizeof(time_info));

    if (use_local) {
        const char* old_tz = std::getenv("TZ");
        std::string old_tz_str = old_tz ? old_tz : "";
        setenv("TZ", timezone.c_str(), 1);
        tzset();
        localtime_r(&now_t, &time_info);
        if (!old_tz_str.empty()) {
            setenv("TZ", old_tz_str.c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    } else {
        gmtime_r(&now_t, &time_info);
        timezone = "UTC";
    }

    std::ostringstream iso8601;
    iso8601 << std::put_time(&time_info, "%Y-%m-%dT%H:%M:%S");
    if (!use_local) {
        iso8601 << "Z";
    } else {
        char offset_buf[8];
        std::strftime(offset_buf, sizeof(offset_buf), "%z", &time_info);
        std::string offset(offset_buf);
        if (offset.size() == 5) {
            iso8601 << offset.substr(0, 3) << ":" << offset.substr(3, 2);
        } else {
            iso8601 << offset;
        }
    }

    std::ostringstream date_str;
    date_str << std::put_time(&time_info, "%Y-%m-%d");

    std::ostringstream time_str;
    time_str << std::put_time(&time_info, "%H:%M:%S");

    static const std::array<const char*, 7> day_names = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };
    static const std::array<const char*, 12> month_names = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    json result = {
        {"iso8601",        iso8601.str()},
        {"date",           date_str.str()},
        {"time",           time_str.str()},
        {"day_of_week",    day_names[time_info.tm_wday]},
        {"month",          month_names[time_info.tm_mon]},
        {"year",           1900 + time_info.tm_year},
        {"month_number",   time_info.tm_mon + 1},
        {"day",            time_info.tm_mday},
        {"hour",           time_info.tm_hour},
        {"minute",         time_info.tm_min},
        {"second",         time_info.tm_sec},
        {"timezone",       timezone},
        {"unix_timestamp", static_cast<long long>(now_t)}
    };

    return makeTextResult(result.dump());
}

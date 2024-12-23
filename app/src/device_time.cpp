#include <cstdio>
#include <memory>
#include <string>
#include <chrono>
#include <array>
#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "util/log.h"
#include "device_time.h"

namespace {
    // Helper function to execute command and get output
    std::string exec_command(const char* cmd) {
        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (!pipe) {
            LOGE("popen() failed!");
            return "";
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }
}

int64_t get_device_boot_time(const char* ip_port, const char* adb_path) {
    // Get current time in milliseconds
    std::string cmd = "\"" + std::string(adb_path) + "\" -s " + std::string(ip_port) + " shell date +%s%3N";
    std::string now_str = exec_command(cmd.c_str());
    if (now_str.empty()) {
        LOGE("Failed to get device time");
        return 0;
    }
    int64_t now_ms = std::stoll(now_str);

    // Get device uptime using adb with IP address and port
    cmd = "\"" + std::string(adb_path) + "\" -s " + std::string(ip_port) + " shell cat /proc/uptime";
    std::string uptime_str = exec_command(cmd.c_str());
    if (uptime_str.empty()) {
        LOGE("Failed to get device uptime");
        return 0;
    }

    // Parse uptime (first number before space, in seconds)
    double uptime_s = std::stod(uptime_str);
    int64_t uptime_ms = static_cast<int64_t>(uptime_s * 1000);

    // Calculate boot time
    return now_ms - uptime_ms;
}

const char* fromTimestamp(int64_t timestamp) {
    // Static buffer to store the result string
    static char buffer[64];

    // Extract seconds and milliseconds
    time_t seconds = static_cast<time_t>(timestamp / 1000); // Seconds part
    int milliseconds = timestamp % 1000;                   // Milliseconds part

    // Convert seconds to tm structure
    std::tm* tmTime = std::localtime(&seconds);

    // Format the date and time into the buffer
    if (tmTime) {
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                        tmTime->tm_year + 1900,  // Year
                        tmTime->tm_mon + 1,     // Month (0-11, so add 1)
                        tmTime->tm_mday,        // Day
                        tmTime->tm_hour,        // Hour
                        tmTime->tm_min,         // Minute
                        tmTime->tm_sec,         // Second
                        milliseconds);          // Milliseconds
    } else {
        std::snprintf(buffer, sizeof(buffer), "Invalid timestamp");
    }

    // Return the static buffer
    return buffer;
}
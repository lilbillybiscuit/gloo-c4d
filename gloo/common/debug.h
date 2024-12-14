//
// Created by Billy Qian on 12/16/24.
//

#ifndef DEBUG_H
#define DEBUG_H
#pragma once

#include <iostream>
#include <sstream>

namespace gloo {
    namespace debug {

        // Enum for debug levels
        enum class DebugLevel {
            NONE = 0,
            INFO,
            WARNING,
            ERROR,
            VERBOSE
          };

        // Global debug level (can be set at runtime)
        extern DebugLevel global_debug_level;

        // Macro for conditional debugging output based on debug level
#define GLOO_DEBUG(level, ...)                                         \
do {                                                                 \
if (gloo::debug::global_debug_level >= gloo::debug::DebugLevel::level) { \
gloo::debug::debugPrint(gloo::debug::DebugLevel::level, __FILE__, __LINE__, __VA_ARGS__); \
}                                                                  \
} while (0)

        inline void debugPrint(DebugLevel level, const char* file, int line, const std::string& msg) {
            std::stringstream ss;
            switch (level) {
                case DebugLevel::INFO:
                    ss << "[INFO] ";
                break;
                case DebugLevel::WARNING:
                    ss << "[WARNING] ";
                break;
                case DebugLevel::ERROR:
                    ss << "[ERROR] ";
                break;
                case DebugLevel::VERBOSE:
                    ss << "[VERBOSE] ";
                break;
                default:
                    ss << "[DEBUG] ";
            }
            ss << file << ":" << line << ": " << msg << std::endl;
            std::cerr << ss.str(); // Output to stderr to avoid buffering issues
        }

        // Overload debugPrint to handle various argument types using variadic templates
        template <typename T, typename... Args>
        void debugPrint(DebugLevel level, const char* file, int line, const T& first, const Args&... args) {
            std::stringstream ss;
            ss << first;
            debugPrint(level, file, line, ss.str());
            debugPrint(level, file, line, args...);
        }

        template <typename T>
        void debugPrint(DebugLevel level, const char* file, int line, const T& value) {
            std::stringstream ss;
            ss << value;
            debugPrint(level, file, line, ss.str());
        }

    }
}
#endif //DEBUG_H

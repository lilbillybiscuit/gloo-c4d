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

        enum class DebugLevel {
            NONE = 0,
            INFO,
            WARNING,
            ERROR,
            VERBOSE
        };

        extern DebugLevel global_debug_level;

        // Helper function to concatenate all arguments with spaces
        template<typename... Args>
        std::string concatenateArgs(const Args&... args) {
            std::stringstream ss;
            ((ss << args << ' '), ...);  // fold expression to concatenate all args
            return ss.str();
        }

#define GLOO_DEBUG(level, ...)                                         \
do {                                                                   \
    if (gloo::debug::global_debug_level >= gloo::debug::DebugLevel::level) { \
        std::stringstream ss;                                          \
        switch (gloo::debug::DebugLevel::level) {                     \
            case gloo::debug::DebugLevel::INFO:                       \
                ss << "[INFO] ";                                      \
                break;                                                \
            case gloo::debug::DebugLevel::WARNING:                    \
                ss << "[WARNING] ";                                   \
                break;                                                \
            case gloo::debug::DebugLevel::ERROR:                      \
                ss << "[ERROR] ";                                     \
                break;                                                \
            case gloo::debug::DebugLevel::VERBOSE:                    \
                ss << "[VERBOSE] ";                                   \
                break;                                                \
            default:                                                  \
                ss << "[DEBUG] ";                                     \
        }                                                             \
        ss << __FILE__ << ":" << __LINE__ << ": "                    \
           << gloo::debug::concatenateArgs(__VA_ARGS__)              \
           << "\n";                                             \
        std::cerr << ss.str();                                       \
    }                                                                 \
} while (0)

    }
}
#endif //DEBUG_H

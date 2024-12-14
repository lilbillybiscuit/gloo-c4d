//
// Created by Billy Qian on 12/16/24.
//

#ifndef CCL_MONITOR_H
#define CCL_MONITOR_H
#pragma once

#include <vector>
#include <map>
#include <string>
#include <chrono>
#include "gloo/types.h"
#include "gloo/common/logging.h"
#include "gloo/common/debug.h"

namespace gloo {

    // Forward declaration of context
    class Context;

    namespace ccl {

        class CCLMonitor {
        public:
            CCLMonitor(const std::shared_ptr<Context>& context);
            ~CCLMonitor();

            void recordStart(
                const std::string& opType,
                const std::string& algorithm,
                const std::string& dataType,
                size_t count,
                int rootRank = -1);

            void recordEnd(const std::string& opType);

            // Updated to record latency
            void recordSend(int remoteRank, size_t bytes, int tag);
            void recordRecv(int remoteRank, size_t bytes, int tag);

        private:
            std::shared_ptr<Context> context_;
            int rank_;
            int size_;

            struct OperationStats {
                std::string opType;
                std::string algorithm;
                std::string dataType;
                size_t count;
                int rootRank;
                std::chrono::time_point<std::chrono::steady_clock> startTime;
                std::chrono::time_point<std::chrono::steady_clock> endTime;
            };

            struct SendRecvEvent {
                std::chrono::time_point<std::chrono::steady_clock> time;
                int remoteRank;
                size_t bytes;
            };

            std::map<int, SendRecvEvent> sendEvents;
            std::map<int, SendRecvEvent> recvEvents;

            std::map<std::string, OperationStats> ongoingOperations_;

            void sendDataToMonitor(const std::string& key, const std::string& value);
        };

    } // namespace ccl
} // namespace gloo

#endif //CCL_MONITOR_H

#ifndef CCL_MONITOR_H
#define CCL_MONITOR_H
#pragma once

#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <string>
#include <atomic>
#include <memory>
#include "gloo/types.h"
#include "gloo/common/logging.h"
#include "gloo/common/debug.h"

#define MONITOR_URL "http://localhost:8081/log"


#ifndef DEFAULT_COUNT
#define DEFAULT_COUNT (opts.elementSize * opts.out[0]->size / context->size)
#endif

// Helper macro to handle default value for count
#define GET_COUNT(_1, _2, _3, _4, COUNT2, ...) COUNT2

// Define two versions of RECORD_START_HELPER, one for each number of arguments
#define RECORD_START_HELPER_WITH_COUNT(opType, algorithm, dataType, count) \
gloo::ccl::CCLMonitor cclMonitor(context); \
int tag8randomstart = cclMonitor.recordStart(opType, algorithm, dataType, count, context->rank); \
int tag8randomsend = -1; \
int tag8randomrecv = -1;

#define RECORD_START_HELPER_DEFAULT_COUNT(opType, algorithm, dataType) \
RECORD_START_HELPER_WITH_COUNT(opType, algorithm, dataType, DEFAULT_COUNT)

// Macro to check if count is provided; if not, use DEFAULT_COUNT
#define RECORD_START(...) \
GET_COUNT(__VA_ARGS__, RECORD_START_HELPER_WITH_COUNT, RECORD_START_HELPER_DEFAULT_COUNT)(__VA_ARGS__)

// #define RECORD_START(opType, algorithm, dataType, count) \
//     gloo::ccl::CCLMonitor cclMonitor(context); \
//     int tag8randomstart = cclMonitor.recordStart(opType, algorithm, dataType, count, context->rank); \
//     int tag8randomsend = -1; \
//     int tag8randomrecv = -1;

#define RECORD_SEND_START(remoteRank, nbytes) \
tag8randomsend = cclMonitor.recordSendStart(remoteRank, nbytes, __FILE__);

#define SEND(outObj, remoteRank, slot, offset, nbytes) \
    outObj->send(remoteRank, slot, offset, nbytes); \
    RECORD_SEND_START(remoteRank, nbytes);

#define WAIT_SEND(outObj, tmt) \
    outObj->waitSend(tmt); \
    cclMonitor.recordSendEnd(tag8randomsend);


#define RECORD_RECV_START(remoteRank, bytes) \
tag8randomrecv = cclMonitor.recordRecvStart(remoteRank, bytes, __FILE__);


#define RECV(outObj, remoteRank, slot, offset, nbytes) \
    outObj->recv(remoteRank, slot, offset, nbytes); \
    RECORD_RECV_START(remoteRank, nbytes);

#define WAIT_RECV(outObj, tmt) \
    outObj->waitRecv(tmt); \
    cclMonitor.recordRecvEnd(tag8randomrecv);

#define RECORD_END() \
    cclMonitor.recordEnd(tag8randomstart); \
    tag8randomstart = -1;

namespace gloo {
    class Context;

    namespace ccl {
        class CCLMonitor {
        public:
            CCLMonitor(const std::shared_ptr<Context> &context);
            ~CCLMonitor();

            int recordStart(
                const std::string &opType,
                const std::string &algorithm,
                const std::string &dataType,
                size_t count,
                int rootRank = -1);

            void recordEnd(int tag);

            int recordSendStart(int remoteRank, size_t bytes, const std::string &filename);
            int recordRecvStart(int remoteRank, size_t bytes, const std::string &filename);

            void recordSendEnd(int tag);
            void recordRecvEnd(int tag);

        private:
            std::shared_ptr<Context> context_;
            int rank_;
            int size_;

            struct StatStruct {
                int tag;
                std::string filename;
                bool finished = false;
                virtual const std::string serialize() const = 0;
                virtual ~StatStruct() = default;
            };

            struct OperationStats : StatStruct {
                std::string opType;
                std::string algorithm;
                std::string dataType;
                size_t count;
                int rootRank;
                std::chrono::time_point<std::chrono::steady_clock> startTime;
                std::chrono::time_point<std::chrono::steady_clock> endTime;
                int rank_;
                int size_;


                const std::string serialize() const override {
                    return "opType=" + opType +
                           "&algorithm=" + algorithm +
                           "&dataType=" + dataType +
                           "&count=" + std::to_string(count) +
                           "&rootRank=" + std::to_string(rootRank) +
                           "&context_rank=" + std::to_string(rank_) +
                           "&context_size=" + std::to_string(size_) +
                           "&tag=" + std::to_string(tag) +
                           "&startTime=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(startTime.time_since_epoch()).count()) +
                           "&endTime=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(endTime.time_since_epoch()).count()) +
                           "&filename=" + filename +
                           "&finished=" + std::to_string(finished);
                }

                OperationStats(int tag, const std::string &filename, const std::string &opType, const std::string &algorithm, const std::string &dataType, size_t count, int rootRank, std::chrono::time_point<std::chrono::steady_clock> startTime, std::chrono::time_point<std::chrono::steady_clock> endTime, int rank, int size, bool finished)
                    : opType(opType), algorithm(algorithm), dataType(dataType), count(count), rootRank(rootRank), startTime(startTime), endTime(endTime), rank_(rank), size_(size) {
                    this->tag = tag;
                    this->filename = filename;
                    this->finished = finished;

                }
            };

            struct SendRecvEvent : StatStruct {
                std::string opType;
                std::chrono::time_point<std::chrono::steady_clock> time;
                int remoteRank;
                size_t bytes;
                int rank_;
                int size_;

                const std::string serialize() const override {
                    return "remoteRank=" + std::to_string(remoteRank) +
                           "&context_rank=" + std::to_string(rank_) +
                           "&context_size=" + std::to_string(size_) +
                           "&bytes=" + std::to_string(bytes) +
                           "&tag=" + std::to_string(tag) +
                           "&time=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count()) +
                           "&filename=" + filename +
                           "&finished=" + std::to_string(finished);
                }

                SendRecvEvent(int tag, const std::string &filename, const std::string &opType, std::chrono::time_point<std::chrono::steady_clock> time, int remoteRank, size_t bytes, int rank, int size, bool finished)
                    : opType(opType), time(time), remoteRank(remoteRank), bytes(bytes), rank_(rank), size_(size) {
                    this->tag = tag;
                    this->filename = filename;
                    this->finished = finished;
                }
            };

            std::map<int, std::unique_ptr<StatStruct>> ongoingOperations_;
            std::atomic<int> nextTag_;
            void sendDataToMonitor(const StatStruct &stats);
        };
    } // namespace ccl
} // namespace gloo

#endif //CCL_MONITOR_H
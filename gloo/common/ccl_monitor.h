#ifndef CCL_MONITOR_H
#define CCL_MONITOR_H
#pragma once

#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <string>
#include <atomic>
#include <iomanip>
#include <queue>
#include <mutex>
#include <memory>
#include "gloo/types.h"
#include "gloo/common/logging.h"
#include "gloo/common/debug.h"
#define MONITOR_URL "http://localhost:8081/log_ccl"

#ifndef DEFAULT_COUNT
#define DEFAULT_COUNT (opts.elementSize * opts.out[0]->size / context->size)
#endif

// Helper macro to handle default value for count
#define GET_COUNT(_1, _2, _3, _4, COUNT2, ...) COUNT2

#define RECORD_START_HELPER_WITH_COUNT(opType, algorithm, dataType, count) \
gloo::ccl::CCLMonitor cclMonitor(context); \
std::string opId = cclMonitor.recordStart(opType, algorithm, dataType, count, context->rank);

#define RECORD_START_HELPER_DEFAULT_COUNT(opType, algorithm, dataType) \
RECORD_START_HELPER_WITH_COUNT(opType, algorithm, dataType, DEFAULT_COUNT)

#define RECORD_START(...) \
GET_COUNT(__VA_ARGS__, RECORD_START_HELPER_WITH_COUNT, RECORD_START_HELPER_DEFAULT_COUNT)(__VA_ARGS__)

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define QUEUE_NAME(outObj2) (std::string(__func__) + "-" + TOSTRING(outObj2)).c_str()

#define RECORD_SEND_START(outObj, remoteRank, nbytes) \
cclMonitor.recordSendStart(QUEUE_NAME(outObj), remoteRank, nbytes, __FILE__);

#define SEND(outObj, remoteRank, slot, offset, nbytes) \
do { \
    outObj->send(remoteRank, slot, offset, nbytes); \
    cclMonitor.recordSendStart(QUEUE_NAME(outObj), remoteRank, nbytes, __FILE__); \
} while(0)

#define WAIT_SEND(outObj, tmt) \
do {\
outObj->waitSend(tmt); \
cclMonitor.recordSendEnd(QUEUE_NAME(outObj)); \
} while (0)


#define RECORD_RECV_START(outObj, remoteRank, bytes) \
cclMonitor.recordRecvStart(QUEUE_NAME(outObj), remoteRank, bytes, __FILE__);

#define RECV(outObj, remoteRank, slot, offset, nbytes) \
do { \
    outObj->recv(remoteRank, slot, offset, nbytes); \
    cclMonitor.recordRecvStart(QUEUE_NAME(outObj), remoteRank, nbytes, __FILE__); \
} while(0)

#define WAIT_RECV(outObj, tmt) \
do {\
outObj->waitRecv(tmt); \
cclMonitor.recordRecvEnd(QUEUE_NAME(outObj)); \
} while (0)

#define RECORD_END() \
cclMonitor.recordEnd(opId);

namespace gloo {
    class Context;

    namespace ccl {
        class JsonWriter {
            std::stringstream& ss_;
            bool first_ = true;

        public:
            explicit JsonWriter(std::stringstream& ss) : ss_(ss) {
                ss_ << '{';
            }

            ~JsonWriter() {
                ss_ << '}';
            }

            template<typename T>
            JsonWriter& field(const char* key, const T& value) {
                if (!first_) ss_ << ',';
                first_ = false;
                writeField(key, value);
                return *this;
            }

        private:
            static std::string escapeJson(const std::string& input) {
                std::stringstream escaped;
                for (char c : input) {
                    switch (c) {
                        case '"': escaped << "\\\""; break;
                        case '\\': escaped << "\\\\"; break;
                        case '\b': escaped << "\\b"; break;
                        case '\f': escaped << "\\f"; break;
                        case '\n': escaped << "\\n"; break;
                        case '\r': escaped << "\\r"; break;
                        case '\t': escaped << "\\t"; break;
                        default:
                            if ('\x00' <= c && c <= '\x1f') {
                                escaped << "\\u"
                                       << std::hex << std::setw(4) << std::setfill('0')
                                       << static_cast<int>(c);
                            } else {
                                escaped << c;
                            }
                    }
                }
                return escaped.str();
            }

            void writeField(const char* key, const std::string& value) {
                ss_ << '"' << escapeJson(key) << "\":\"" << escapeJson(value) << '"';
            }

            void writeField(const char* key, const char* value) {
                ss_ << '"' << escapeJson(key) << "\":\"" << escapeJson(std::string(value)) << '"';
            }

            void writeField(const char* key, bool value) {
                ss_ << '"' << escapeJson(key) << "\":" << (value ? "true" : "false");
            }

            template<typename T>
            void writeField(const char* key, const T& value) {
                ss_ << '"' << escapeJson(key) << "\":" << value;
            }
        };

        class CCLMonitor {
        public:
            CCLMonitor(const std::shared_ptr<Context>& context);
            ~CCLMonitor();

            std::string recordStart(
                const std::string& opType,
                const std::string& algorithm,
                const std::string& dataType,
                size_t count,
                int rootRank = -1);

            void recordEnd(const std::string& opId);

            void recordSendStart(const std::string& queueId, int remoteRank, size_t bytes, const std::string& filename);
            void recordRecvStart(const std::string& queueId, int remoteRank, size_t bytes, const std::string& filename);

            void recordSendEnd(const std::string& queueId);
            void recordRecvEnd(const std::string& queueId);

        private:
            std::shared_ptr<Context> context_;
            int rank_;
            int size_;

            struct StatStruct {
                std::string queueId;
                std::string filename;
                bool finished = false;
                virtual const std::string serialize() const = 0;
                virtual ~StatStruct() = default;
            };

            struct OperationStats : StatStruct {
                std::string opId;
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
                    std::stringstream ss;
                    {
                        JsonWriter json(ss);

                        json.field("opType", opType)
                            .field("algorithm", algorithm)
                            .field("dataType", dataType)
                            .field("count", count)
                            .field("rootRank", rootRank)
                            .field("context_rank", rank_)
                            .field("context_size", size_)
                            .field("startTime", std::chrono::duration_cast<std::chrono::microseconds>(startTime.time_since_epoch()).count())
                            .field("endTime", std::chrono::duration_cast<std::chrono::microseconds>(endTime.time_since_epoch()).count())
                            .field("filename", filename)
                            .field("finished", finished);
                    }

                    return ss.str();
                }


                OperationStats(const std::string opId, const std::string &filename, const std::string &opType, const std::string &algorithm, const std::string &dataType, size_t count, int rootRank, std::chrono::time_point<std::chrono::steady_clock> startTime, std::chrono::time_point<std::chrono::steady_clock> endTime, int rank, int size, bool finished)
                    : opId(opId), opType(opType), algorithm(algorithm), dataType(dataType), count(count), rootRank(rootRank), startTime(startTime), endTime(endTime), rank_(rank), size_(size) {
                    this->filename = filename;
                    this->finished = finished;

                }
            };

            struct SendRecvEvent : StatStruct {
                std::string queueId;
                std::string opType;
                std::chrono::time_point<std::chrono::steady_clock> startTime;
                std::chrono::time_point<std::chrono::steady_clock> endTime;
                int remoteRank;
                size_t bytes;
                int rank_;
                int size_;

                const std::string serialize() const override {
                    std::stringstream ss;
                    {
                        JsonWriter json(ss);

                        json.field("opType", opType)
                            .field("remoteRank", remoteRank)
                            .field("context_rank", rank_)
                            .field("context_size", size_)
                            .field("bytes", bytes)
                            .field("startTime", std::chrono::duration_cast<std::chrono::microseconds>(startTime.time_since_epoch()).count())
                            .field("endTime", std::chrono::duration_cast<std::chrono::microseconds>(endTime.time_since_epoch()).count())
                            .field("filename", filename)
                            .field("finished", finished);
                    }

                    return ss.str();
                }

                SendRecvEvent(const std::string &queueId, const std::string &filename, const std::string &opType, std::chrono::time_point<std::chrono::steady_clock> time, int remoteRank, size_t bytes, int rank, int size, bool finished)
                    : queueId(queueId), opType(opType), startTime(time), remoteRank(remoteRank), bytes(bytes), rank_(rank), size_(size) {
                    this->filename = filename;
                    this->finished = finished;
                }
            };

            std::map<std::string, std::unique_ptr<StatStruct>> ongoingOperations_;
            std::map<std::string, std::queue<std::unique_ptr<SendRecvEvent>>> sendQueues_;
            std::map<std::string, std::queue<std::unique_ptr<SendRecvEvent>>> recvQueues_;

            std::mutex queueMutex_, sendQueueMutex_, recvQueueMutex_;
            static void sendDataToMonitor(const std::string &serialized, bool finished);

            std::atomic<uint64_t> opCounter_;
            std::string generateOpId() {
                return "op-" + std::to_string(opCounter_++);
            }

        };
    } // namespace ccl
} // namespace gloo

#endif //CCL_MONITOR_H
//
// Created by Billy Qian on 12/16/24.
//

#include "gloo/common/ccl_monitor.h"
#include "gloo/context.h"

#include <curl/curl.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sstream>

namespace gloo {
    namespace ccl {
        std::queue<std::string> sendQueue;
        std::mutex queueMutex;
        std::condition_variable queueCondVar;
        bool sendThreadRunning = true;

        void sendDataThread() {
            CURL *curl = curl_easy_init();
            if (!curl) {
                GLOO_DEBUG(ERROR, "Could not initialize libcurl for sending data to monitor");
                return;
            }

            while (sendThreadRunning) {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCondVar.wait(lock, [] { return !sendQueue.empty() || !sendThreadRunning; });

                if (!sendQueue.empty()) {
                    std::string data = sendQueue.front();
                    GLOO_DEBUG(VERBOSE, "Sending data to monitor: ", data);
                    sendQueue.pop();
                    lock.unlock();

                    std::string url = MONITOR_URL;
                    std::string postData = data;

                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1L);

                    CURLcode res = curl_easy_perform(curl);
                    if (res != CURLE_OK) {
                        GLOO_DEBUG(ERROR, "Failed to send data to monitor: ", curl_easy_strerror(res));
                    }
                } else {
                    lock.unlock();
                }
            }

            curl_easy_cleanup(curl);
        }

        CCLMonitor::CCLMonitor(const std::shared_ptr<Context> &context)
            : context_(context), rank_(context->rank), size_(context->size), nextTag_(0) {
            curl_global_init(CURL_GLOBAL_ALL);

            std::thread sender(sendDataThread);
            sender.detach();
        }

        CCLMonitor::~CCLMonitor() {
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                sendThreadRunning = false;
            }
            queueCondVar.notify_all();

            curl_global_cleanup();
        }

        int CCLMonitor::recordStart(const std::string &opType, const std::string &algorithm, const std::string &dataType, size_t count, int rootRank) {
            GLOO_DEBUG(INFO, "CCLMonitor::recordStart - Operation: ", opType, ", Algorithm: ", algorithm,
                       ", DataType: ", dataType, ", Count: ", count, ", RootRank: ", rootRank);
            int tag = nextTag_++;
            auto now = std::chrono::steady_clock::now();
            auto stats = std::make_unique<OperationStats>(tag, "", opType, algorithm, dataType, count, rootRank, now, now, rank_, size_, false);
            ongoingOperations_[tag] = std::move(stats);

            sendDataToMonitor(*ongoingOperations_[tag]);
            return tag;
        }

        void CCLMonitor::recordEnd(int tag) {
            GLOO_DEBUG(INFO, "CCLMonitor::recordEnd - Tag: ", tag);
            auto time_now = std::chrono::steady_clock::now();
            auto it = ongoingOperations_.find(tag);
            if (it != ongoingOperations_.end()) {
                auto *opStats = dynamic_cast<OperationStats *>(it->second.get());
                if (opStats != nullptr) {
                    opStats->endTime = time_now;
                    opStats->finished = true;
                    sendDataToMonitor(*opStats);
                    ongoingOperations_.erase(it);
                } else {
                    GLOO_DEBUG(WARNING, "CCLMonitor::recordEnd - Event is not of type OperationStats");
                }
            } else {
                GLOO_DEBUG(WARNING, "CCLMonitor::recordEnd - No matching start event found for tag: ", tag);
            }
        }

        int CCLMonitor::recordSendStart(int remoteRank, size_t bytes, const std::string &filename) {
            int tag = nextTag_++;
            auto event = std::make_unique<SendRecvEvent>(tag, filename, "send", std::chrono::steady_clock::now(), remoteRank, bytes, rank_, size_, false);
            ongoingOperations_[tag] = std::move(event);

            GLOO_DEBUG(VERBOSE, "CCLMonitor::recordSendStart - Rank: ", rank_, ", RemoteRank: ", remoteRank,
                       ", Bytes: ", bytes, ", Tag: ", tag, ", Filename: ", filename);
            return tag;
        }

        int CCLMonitor::recordRecvStart(int remoteRank, size_t bytes, const std::string &filename) {
            int tag = nextTag_++;
            auto event = std::make_unique<SendRecvEvent>(tag, filename, "recv", std::chrono::steady_clock::now(), remoteRank, bytes, rank_, size_, false);
            ongoingOperations_[tag] = std::move(event);

            GLOO_DEBUG(VERBOSE, "CCLMonitor::recordRecvStart - Rank: ", rank_, ", RemoteRank: ", remoteRank,
                       ", Bytes: ", bytes, ", Tag: ", tag, ", Filename: ", filename);
            return tag;
        }

        void CCLMonitor::recordSendEnd(int tag) {
            GLOO_DEBUG(VERBOSE, "CCLMonitor::recordSendEnd - Tag: ", tag);
            auto time_now = std::chrono::steady_clock::now();
            auto it = ongoingOperations_.find(tag);
            if (it != ongoingOperations_.end()) {
                auto *event = dynamic_cast<SendRecvEvent *>(it->second.get());
                if (event != nullptr) {
                    event->time = time_now;
                    event->finished = true;
                    sendDataToMonitor(*event);
                    ongoingOperations_.erase(it);
                } else {
                    GLOO_DEBUG(WARNING, "CCLMonitor::recordSendEnd - Event is not of type SendRecvEvent");
                }
            } else {
                GLOO_DEBUG(WARNING, "CCLMonitor::recordSendEnd - No matching start event found for tag: ", tag);
            }
        }

        void CCLMonitor::recordRecvEnd(int tag) {
            GLOO_DEBUG(VERBOSE, "CCLMonitor::recordRecvEnd - Tag: ", tag);
            auto time_now = std::chrono::steady_clock::now();
            auto it = ongoingOperations_.find(tag);
            if (it != ongoingOperations_.end()) {
                auto *event = dynamic_cast<SendRecvEvent *>(it->second.get());
                if (event != nullptr) {
                    event->time = time_now;
                    event->finished = true;
                    sendDataToMonitor(*event);
                    ongoingOperations_.erase(it);
                } else {
                    GLOO_DEBUG(WARNING, "CCLMonitor::recordRecvEnd - Event is not of type SendRecvEvent");
                }
            } else {
                GLOO_DEBUG(WARNING, "CCLMonitor::recordRecvEnd - No matching start event found for tag: ", tag);
            }
        }

        void CCLMonitor::sendDataToMonitor(const StatStruct &stat) {
            std::string serialized = stat.serialize();
            std::string logMsg = (stat.finished ? "Finished: " : "Started: ") + serialized;
            GLOO_DEBUG(VERBOSE, "CCLMonitor::sendDataToMonitor - ", logMsg);
            std::unique_lock<std::mutex> lock(queueMutex);
            sendQueue.push(serialized);
            queueCondVar.notify_one();
        }
    } // namespace ccl
} // namespace gloo

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
        class SendThreadManager {
        private:
            static std::unique_ptr<SendThreadManager> instance_;
            static std::mutex initMutex_;

            std::thread sendThread_;
            std::queue<std::string> sendQueue_;
            std::mutex queueMutex_;
            std::condition_variable queueCondVar_;
            bool sendThreadRunning_;

            // Private constructor for singleton
            SendThreadManager() : sendThreadRunning_(true) {
                sendThread_ = std::thread(&SendThreadManager::sendDataThread, this);
                sendThread_.detach();
            }
        public:
            // Delete copy constructor and assignment operator
            SendThreadManager(const SendThreadManager&) = delete;
            SendThreadManager& operator=(const SendThreadManager&) = delete;

            static SendThreadManager& getInstance() {
                std::lock_guard<std::mutex> lock(initMutex_);
                if (!instance_) {
                    instance_.reset(new SendThreadManager());
                }
                return *instance_;
            }

            void pushData(const std::string& data) {
                std::unique_lock<std::mutex> lock(queueMutex_);
                sendQueue_.push(data);
                queueCondVar_.notify_one();
            }

            ~SendThreadManager() {
                {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    sendThreadRunning_ = false;
                }
                queueCondVar_.notify_all();
            }

        private:
            void sendDataThread() {
                CURL *curl = curl_easy_init();
                if (!curl) {
                    GLOO_DEBUG(ERROR, "Could not initialize libcurl for sending data to monitor");
                    return;
                }

                // Set up headers for JSON
                struct curl_slist *headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                headers = curl_slist_append(headers, "Accept: application/json");

                while (sendThreadRunning_) {
                    std::unique_lock<std::mutex> lock(queueMutex_);
                    queueCondVar_.wait(lock, [this] {
                        return !sendQueue_.empty() || !sendThreadRunning_;
                    });

                    if (!sendQueue_.empty()) {
                        std::string data = sendQueue_.front();
                        GLOO_DEBUG(VERBOSE, "Sending data to monitor: ", data);
                        sendQueue_.pop();
                        lock.unlock();

                        std::string url = MONITOR_URL;

                        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
                        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1L);

                        CURLcode res = curl_easy_perform(curl);
                        if (res != CURLE_OK) {
                            GLOO_DEBUG(ERROR, "Failed to send data to monitor: ", curl_easy_strerror(res));
                        }
                    }
                }

                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        };

        std::unique_ptr<SendThreadManager> SendThreadManager::instance_;
        std::mutex SendThreadManager::initMutex_;

        CCLMonitor::CCLMonitor(const std::shared_ptr<Context> &context)
            : context_(context), rank_(context->rank), size_(context->size), opCounter_(0) {
            curl_global_init(CURL_GLOBAL_ALL);
        }

        CCLMonitor::~CCLMonitor() {
            // {
            //     std::unique_lock<std::mutex> lock(queueMutex);
            //     sendThreadRunning = false;
            // }
            // queueCondVar.notify_all();

            curl_global_cleanup();
        }



        std::string CCLMonitor::recordStart(const std::string& opType, const std::string& algorithm,
                                          const std::string& dataType, size_t count, int rootRank) {
            GLOO_DEBUG(INFO, "CCLMonitor::recordStart - Operation: ", opType, ", Algorithm: ", algorithm,
                       ", DataType: ", dataType, ", Count: ", count, ", RootRank: ", rootRank);
            auto now = std::chrono::steady_clock::now();
            std::string opId = generateOpId();
            std::string serialized;

            auto stats = std::make_unique<OperationStats>(opId, "", opType, algorithm, dataType, count, rootRank, now, now, rank_, size_, false);
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                ongoingOperations_[opId] = std::move(stats);
                serialized = ongoingOperations_[opId]->serialize();
            }

            sendDataToMonitor(serialized, false);
            return opId;
        }

        void CCLMonitor::recordEnd(const std::string& opId) {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(queueMutex_);
            std::string serialized;
            auto it = ongoingOperations_.find(opId);
            if (it != ongoingOperations_.end()) {
                auto* opStats = dynamic_cast<OperationStats*>(it->second.get());
                if (opStats != nullptr) {
                    opStats->endTime = now;
                    opStats->finished = true;
                    serialized = opStats->serialize();
                    // sendDataToMonitor(*opStats);
                    ongoingOperations_.erase(it);
                }
            }
            if (!serialized.empty()) {
                sendDataToMonitor(serialized, true);
            }

        }

        void CCLMonitor::recordSendStart(const std::string& queueId, int remoteRank,
                                       size_t bytes, const std::string& filename) {

            auto event = std::make_unique<SendRecvEvent>(queueId, filename, "send",
                std::chrono::steady_clock::now(), remoteRank, bytes, rank_, size_, false);
            std::string serialized;
            {
                std::lock_guard<std::mutex> lock(sendQueueMutex_);
                sendQueues_[queueId].push(std::move(event));
                serialized =  sendQueues_[queueId].back()->serialize();
            }
            sendDataToMonitor(serialized, true);
        }

        void CCLMonitor::recordSendEnd(const std::string& queueId) {
            std::string serialized;
            {
                std::lock_guard<std::mutex> lock(sendQueueMutex_);
                if (sendQueues_[queueId].empty()) {
                    GLOO_DEBUG(WARNING, "No matching send operation found for queue: ", queueId);
                    return;
                }

                auto& event = sendQueues_[queueId].front();
                event->endTime = std::chrono::steady_clock::now();
                event->finished = true;
                serialized = event->serialize();
                sendQueues_[queueId].pop();
            }
            sendDataToMonitor(serialized, true);

        }

        void CCLMonitor::recordRecvStart(const std::string& queueId, int remoteRank,
                                       size_t bytes, const std::string& filename) {

            auto event = std::make_unique<SendRecvEvent>(queueId, filename, "recv",
                std::chrono::steady_clock::now(), remoteRank, bytes, rank_, size_, false);
            std::string serialized;
            {
                std::lock_guard<std::mutex> lock(recvQueueMutex_);
                recvQueues_[queueId].push(std::move(event));
                serialized = recvQueues_[queueId].back()->serialize();
            }

            sendDataToMonitor(serialized, false);
        }

        void CCLMonitor::recordRecvEnd(const std::string& queueId) {
            std::string serialized;
            {
                std::lock_guard<std::mutex> lock(recvQueueMutex_);
                auto now = std::chrono::steady_clock::now();
                if (recvQueues_[queueId].empty()) {
                    GLOO_DEBUG(WARNING, "No matching recv operation found for queue: ", queueId);
                    return;
                }

                auto& event = recvQueues_[queueId].front();
                event->endTime = now;
                event->finished = true;
                serialized = event->serialize();
                recvQueues_[queueId].pop();
            }
            sendDataToMonitor(serialized, true);

        }


        void CCLMonitor::sendDataToMonitor(const std::string &serialized, bool finished) {
            std::string logMsg = (finished ? "Finished: " : "Started: ") + serialized;
            GLOO_DEBUG(VERBOSE, "CCLMonitor::sendDataToMonitor - ", logMsg);
            SendThreadManager::getInstance().pushData(serialized);
        }
    } // namespace ccl
} // namespace gloo

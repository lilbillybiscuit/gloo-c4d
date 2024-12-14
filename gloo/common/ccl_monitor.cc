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

namespace gloo {
namespace ccl {

// Structure to hold data for asynchronous send operations
struct MonitorData {
  std::string key;
  std::string value;
};

// Queue for asynchronous send operations
std::queue<MonitorData> sendQueue;
std::mutex queueMutex;
std::condition_variable queueCondVar;
bool sendThreadRunning = true;

// Thread function for sending data asynchronously
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
      MonitorData data = sendQueue.front();
      sendQueue.pop();
      lock.unlock();

      std::string url = "http://localhost:8081/log"; // Your C4D agent URL
      std::string postData = "key=" + data.key + "&value=" + data.value;

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1L); // Timeout after 1 second

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

// Initialize the monitor, get rank and size from the context
CCLMonitor::CCLMonitor(const std::shared_ptr<Context>& context)
    : context_(context), rank_(context->rank), size_(context->size) {
  curl_global_init(CURL_GLOBAL_ALL);

  // Start the sender thread
  std::thread sender(sendDataThread);
  sender.detach(); // Detach the thread so it runs independently
}

CCLMonitor::~CCLMonitor() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        sendThreadRunning = false;
    }
    queueCondVar.notify_all();

    curl_global_cleanup();
}

void CCLMonitor::recordStart(
    const std::string& opType,
    const std::string& algorithm,
    const std::string& dataType,
    size_t count,
    int rootRank) {
  GLOO_DEBUG(INFO, "CCLMonitor::recordStart - Operation: ", opType, ", Algorithm: ", algorithm, ", DataType: ", dataType, ", Count: ", count, ", RootRank: ", rootRank);
  OperationStats stats;
  stats.opType = opType;
  stats.algorithm = algorithm;
  stats.dataType = dataType;
  stats.count = count;
  stats.rootRank = rootRank;
  stats.startTime = std::chrono::steady_clock::now();
  ongoingOperations_[opType] = stats;
}

void CCLMonitor::recordEnd(const std::string& opType) {
  GLOO_DEBUG(INFO, "CCLMonitor::recordEnd - Operation: ", opType);
  auto it = ongoingOperations_.find(opType);
  if (it != ongoingOperations_.end()) {
    it->second.endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        it->second.endTime - it->second.startTime);

    // Prepare data to send to the monitor server
    std::string data =
        "opType=" + it->second.opType +
        "&algorithm=" + it->second.algorithm +
        "&dataType=" + it->second.dataType +
        "&count=" + std::to_string(it->second.count) +
        "&rootRank=" + std::to_string(it->second.rootRank) +
        "&rank=" + std::to_string(rank_) +
        "&size=" + std::to_string(size_) +
        "&duration=" + std::to_string(duration.count());

    // Send data to monitor asynchronously
    sendDataToMonitor(opType + "_end", data);

    ongoingOperations_.erase(it);
  } else {
    GLOO_DEBUG(WARNING, "CCLMonitor::recordEnd - No matching start event found for operation: ", opType);
  }
}

    void CCLMonitor::recordSend(int remoteRank, size_t bytes, int tag) {

    sendEvents[tag] = {std::chrono::steady_clock::now(), remoteRank, bytes};
    GLOO_DEBUG(VERBOSE, "CCLMonitor::recordSend - Rank: ", rank_, ", RemoteRank: ", remoteRank, ", Bytes: ", bytes, ", Tag: ", tag);
}

    void CCLMonitor::recordRecv(int remoteRank, size_t bytes, int tag) {

    recvEvents[tag] = {std::chrono::steady_clock::now(), remoteRank, bytes};
    GLOO_DEBUG(VERBOSE, "CCLMonitor::recordRecv - Rank: ", rank_, ", RemoteRank: ", remoteRank, ", Bytes: ", bytes, ", Tag: ", tag);

    // Calculate latency if corresponding send event exists
    auto sendIt = sendEvents.find(tag);
    if (sendIt != sendEvents.end()) {
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            recvEvents[tag].time - sendIt->second.time);

        std::stringstream dataStream;
        dataStream << "send_rank=" << sendIt->second.remoteRank
                   << "&recv_rank=" << rank_
                   << "&latency_us=" << duration.count()
                   << "&bytes=" << bytes;

        sendDataToMonitor("latency", dataStream.str());

        // Clean up the recorded send event
        sendEvents.erase(sendIt);
    }
}

// Asynchronous send operation
void CCLMonitor::sendDataToMonitor(const std::string& key, const std::string& value) {
  GLOO_DEBUG(VERBOSE, "CCLMonitor::sendDataToMonitor - Key: ", key, ", Value: ", value);
  std::unique_lock<std::mutex> lock(queueMutex);
  sendQueue.push({key, value});
  queueCondVar.notify_one(); // Notify the sender thread that there's data in the queue
}

} // namespace ccl
} // namespace gloo
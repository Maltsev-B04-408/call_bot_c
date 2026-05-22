#ifndef TELEGRAM_CLIENT_H
#define TELEGRAM_CLIENT_H

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

class TelegramClient {
public:
    using Callback = std::function<void(const std::string&, const std::string&)>;

    TelegramClient(int apiId, const std::string& apiHash, const std::string& sessionDir, Callback onMessage);
    ~TelegramClient();

    void addListenChannel(const std::string& channel);
    void start();
    void sendMessage(int64_t chatId, const std::string& text);
    void stop();

private:
    void runLoop();
    void processUpdate(const char* update);
    void handleAuthState(const nlohmann::json& update);
    void handleNewMessage(const nlohmann::json& update);
    void loadChats();
    void requestListenChatsFromKnownChats(const nlohmann::json& chatIds);
    void sendRequest(const nlohmann::json& request);
    std::string toJson(const nlohmann::json& j);

    int apiId_;
    std::string apiHash_;
    std::string sessionDir_;
    Callback onMessage_;
    bool initialized_;
    bool running_;
    void* tdClient_;
    std::vector<std::string> listenChannels_;
    std::mutex channelsMutex_;
    std::thread workerThread_;
};

#endif

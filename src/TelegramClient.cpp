#include "TelegramClient.h"
#include <nlohmann/json.hpp>
#include <td/telegram/td_json_client.h>
#include <iostream>
#include <thread>
#include <filesystem>
#include <mutex>
#include <algorithm>
#include <chrono>

using json = nlohmann::json;

TelegramClient::TelegramClient(int apiId, const std::string& apiHash, const std::string& sessionDir, Callback onMessage)
    : apiId_(apiId), apiHash_(apiHash), sessionDir_(std::filesystem::absolute(sessionDir).string()),
    onMessage_(std::move(onMessage)), initialized_(false), running_(false), tdClient_(nullptr) {
    std::filesystem::create_directories(sessionDir_);
    tdClient_ = td_json_client_create();
    if (!tdClient_) {
        throw std::runtime_error("Failed to create TDLib client");
    }
}

TelegramClient::~TelegramClient() {
    stop();
}

void TelegramClient::stop() {
    running_ = false;
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    if (tdClient_) {
        td_json_client_destroy(tdClient_);
        tdClient_ = nullptr;
    }
}

void TelegramClient::addListenChannel(const std::string& channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    listenChannels_.push_back(channel);
}

std::string TelegramClient::toJson(const json& j) {
    return j.dump();
}

void TelegramClient::start() {
    if (running_) return;
    running_ = true;

    json logReq = { {"@type", "setLogVerbosityLevel"}, {"new_verbosity_level", 1} };
    td_json_client_execute(tdClient_, toJson(logReq).c_str());

    json paramReq = {
        {"@type", "setTdlibParameters"},
        {"use_test_dc", false},
        {"database_directory", sessionDir_},
        {"files_directory", sessionDir_},
        {"database_encryption_key", ""},
        {"use_file_database", false},
        {"use_chat_info_database", false},
        {"use_message_database", false},
        {"use_secret_chats", false},
        {"api_id", apiId_},
        {"api_hash", apiHash_},
        {"system_language_code", "en"},
        {"device_model", "Bot"},
        {"system_version", "1.0"},
        {"application_version", "1.0"},
        {"enable_storage_optimizer", true}
    };
    td_json_client_send(tdClient_, toJson(paramReq).c_str());

    workerThread_ = std::thread(&TelegramClient::runLoop, this);
}

void TelegramClient::runLoop() {
    auto lastLoadChats = std::chrono::steady_clock::now();

    while (running_ && tdClient_) {
        const char* update = td_json_client_receive(tdClient_, 0.1);
        if (!update) {
            if (initialized_ && std::chrono::steady_clock::now() - lastLoadChats > std::chrono::seconds(10)) {
                std::cout << "[DEBUG] Refreshing chat list\n";
                this->loadChats();
                lastLoadChats = std::chrono::steady_clock::now();
            }
            continue;
        }

        try {
            processUpdate(update);
        }
        catch (const std::exception& e) {
            std::cerr << "[ERROR] Update processing failed: " << e.what() << "\n";
        }
    }
}

void TelegramClient::processUpdate(const char* update) {
    try {
        auto j = json::parse(update);
        std::string type = j.value("@type", "");

        if (type == "updateAuthorizationState") {
            this->handleAuthState(j);
        }
        else if (type == "updateNewMessage" && initialized_) {
            this->handleNewMessage(j);
        }
        else if (type == "chat") {
            std::cout << "[CHAT] id=" << j.value("id", 0LL)
                << ", title=\"" << j.value("title", "") << "\"\n";
        }
        else if (type == "chats" && j.contains("chat_ids")) {
            std::cout << "[CHATS] Known chat ids: " << j["chat_ids"].dump() << "\n";
            this->requestListenChatsFromKnownChats(j["chat_ids"]);
        }
        else if (type == "error") {
            std::cerr << "[ERROR] TDLib error";
            if (j.contains("@extra")) {
                std::cerr << " for " << j["@extra"].dump();
            }
            std::cerr << ": " << j.dump() << "\n";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Update processing failed: " << e.what() << "\n";
        return;
    }
}

void TelegramClient::handleAuthState(const json& update) {
    std::string state = update["authorization_state"]["@type"];
    std::cout << "[AUTH] State: " << state << "\n";

    if (state == "authorizationStateWaitPhoneNumber") {
        std::string phone;
        std::cout << "Enter phone number: ";
        std::getline(std::cin, phone);
        json req = {
            {"@type", "setAuthenticationPhoneNumber"},
            {"phone_number", phone},
            {"settings", {
                {"@type", "phoneNumberAuthenticationSettings"},
                {"allow_flash_call", false},
                {"allow_missed_call", false},
                {"is_current_phone_number", false},
                {"allow_sms_retriever_api", false}
            }}
        };
        this->sendRequest(req);
        std::cout << "[AUTH] Phone number sent. Waiting for code or confirmation state...\n";
    }
    else if (state == "authorizationStateWaitCode") {
        std::string code;
        std::cout << "Enter code: ";
        std::getline(std::cin, code);
        json req = { {"@type", "checkAuthenticationCode"}, {"code", code} };
        this->sendRequest(req);
        std::cout << "[AUTH] Code sent.\n";
    }
    else if (state == "authorizationStateWaitPassword") {
        std::string password;
        std::cout << "Enter 2FA password: ";
        std::getline(std::cin, password);
        json req = { {"@type", "checkAuthenticationPassword"}, {"password", password} };
        this->sendRequest(req);
        std::cout << "[AUTH] 2FA password sent.\n";
    }
    else if (state == "authorizationStateWaitOtherDeviceConfirmation") {
        std::string link = update["authorization_state"].value("link", "");
        std::cout << "[AUTH] Confirm login in another Telegram app";
        if (!link.empty()) {
            std::cout << ": " << link;
        }
        std::cout << "\n";
    }
    else if (state == "authorizationStateWaitEmailAddress") {
        std::string email;
        std::cout << "Enter email address: ";
        std::getline(std::cin, email);
        json req = { {"@type", "setAuthenticationEmailAddress"}, {"email_address", email} };
        this->sendRequest(req);
    }
    else if (state == "authorizationStateWaitEmailCode") {
        std::string code;
        std::cout << "Enter email code: ";
        std::getline(std::cin, code);
        json req = {
            {"@type", "checkAuthenticationEmailCode"},
            {"code", {{"@type", "emailAddressAuthenticationCode"}, {"code", code}}}
        };
        this->sendRequest(req);
    }
    else if (state == "authorizationStateWaitRegistration") {
        std::string firstName;
        std::string lastName;
        std::cout << "Enter first name: ";
        std::getline(std::cin, firstName);
        std::cout << "Enter last name: ";
        std::getline(std::cin, lastName);
        json req = { {"@type", "registerUser"}, {"first_name", firstName}, {"last_name", lastName} };
        this->sendRequest(req);
    }
    else if (state == "authorizationStateReady") {
        initialized_ = true;
        std::cout << "[AUTH] Successfully authorized!\n";
        this->loadChats();
    }
    else {
        std::cout << "[AUTH] Unhandled authorization state: " << update["authorization_state"].dump() << "\n";
    }
}

void TelegramClient::handleNewMessage(const json& update) {
    const auto& msg = update["message"];
    if (msg["content"]["@type"] != "messageText") return;

    int64_t chatId = msg["chat_id"].get<int64_t>();
    std::string text = msg["content"]["text"]["text"].get<std::string>();
    int64_t msgDate = msg["date"].get<int64_t>();
    time_t currentTime = std::time(nullptr);

    if (currentTime - msgDate > 60) {
        std::cout << "[MSG] Ignore old message in chat " << chatId << ": " << text << " (date: " << msgDate << ")\n";
        return;
    }

    std::cout << "[MSG] Chat " << chatId << ": " << text << " (date: " << msgDate << ")\n";

    if (onMessage_) {
        onMessage_(std::to_string(chatId), text);
    }
}

void TelegramClient::loadChats() {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    json chatsReq = {
        {"@type", "getChats"},
        {"chat_list", {{"@type", "chatListMain"}}},
        {"limit", 100},
        {"@extra", "load:known_chats"}
    };
    this->sendRequest(chatsReq);

    for (const auto& channel : listenChannels_) {
        std::cout << "[DEBUG] Preparing chat load: " << channel << "\n";
        try {
            int64_t chatId = std::stoll(channel);
            (void)chatId;
            std::cout << "[DEBUG] Numeric chat id will be loaded after getChats: " << channel << "\n";
        }
        catch (const std::exception&) {
            std::cout << "[DEBUG] Using searchPublicChat for username: " << channel << "\n";
            json req = { {"@type", "searchPublicChat"}, {"username", channel}, {"@extra", "load:" + channel} };
            this->sendRequest(req);
        }
    }
}

void TelegramClient::requestListenChatsFromKnownChats(const json& chatIds) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& channel : listenChannels_) {
        try {
            int64_t chatId = std::stoll(channel);
            bool found = false;
            for (const auto& knownChatId : chatIds) {
                if (knownChatId.is_number_integer() && knownChatId.get<int64_t>() == chatId) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                std::cout << "[WARN] Configured chat id is not in known chats: " << chatId << "\n";
                continue;
            }

            std::cout << "[DEBUG] Loading known chat_id: " << chatId << "\n";
            json req = { {"@type", "getChat"}, {"chat_id", chatId}, {"@extra", "load:" + channel} };
            this->sendRequest(req);
        }
        catch (const std::exception&) {
        }
    }
}

void TelegramClient::sendRequest(const json& request) {
    if (!tdClient_) return;
    std::string reqStr = this->toJson(request);
    td_json_client_send(tdClient_, reqStr.c_str());
}

void TelegramClient::sendMessage(int64_t chatId, const std::string& text) {
    if (!initialized_) {
        std::cerr << "[ERROR] Not initialized\n";
        return;
    }

    json req = {
        {"@type", "sendMessage"},
        {"chat_id", chatId},
        {"input_message_content", {
            {"@type", "inputMessageText"},
            {"text", { {"@type", "formattedText"}, {"text", text} }}
        }}
    };
    this->sendRequest(req);
}

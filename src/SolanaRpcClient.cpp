#include "SolanaRpcClient.h"
#include <curl/curl.h>
#include <regex>
#include <stdexcept>
#include <mutex>
#include <fstream>

using json = nlohmann::json;

extern std::ofstream errorLog;
extern std::recursive_mutex logMutex;



size_t SolanaRpcClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    ((std::string*)userp)->append((char*)contents, realsize);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][SOLANA] Buffer size after append: " << ((std::string*)userp)->size() << " bytes\n";
    }
    return realsize;
}

SolanaRpcClient::SolanaRpcClient(const std::string& endpoint)
    : endpoint_(endpoint) {}

json SolanaRpcClient::performRpc(const json& payload) {
    std::string resp;
    resp.reserve(16384);
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SOLANA] Не удалось инициализировать CURL\n";
        throw std::runtime_error("CURL init failed");
    }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    std::string body = payload.dump();
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][SOLANA] Отправка HTTP POST на: " << endpoint_ << ", тело запроса: " << body << "\n";
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][SOLANA] RPC response size: " << resp.size() << " bytes, HTTP code: " << http_code << "\n";
        errorLog << "[DEBUG][SOLANA] Получен ответ от RPC: " << resp.substr(0, 500) << "...\n";
    }

    if (res != CURLE_OK) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SOLANA] RPC request failed: " << curl_easy_strerror(res) << ", HTTP code: " << http_code << "\n";
        throw std::runtime_error("RPC request failed: " + std::string(curl_easy_strerror(res)));
    }

    if (http_code != 200) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SOLANA] Неверный HTTP код: " << http_code << ", ответ: " << resp.substr(0, 500) << "\n";
        throw std::runtime_error("Invalid HTTP code: " + std::to_string(http_code));
    }

    try {
        return json::parse(resp);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SOLANA] Не удалось разобрать JSON: " << e.what() << ", ответ: " << resp.substr(0, 500) << "\n";
        throw;
    }
}
AccountInfo SolanaRpcClient::getAccountInfo(const std::string& pubkey) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getAccountInfo"},
        {"params", {pubkey, {{"encoding", "base64"}}}}
    };
    auto j = performRpc(req);
    if (j.value("result", json()).contains("value") && !j["result"]["value"].is_null()) {
        return { true, j["result"]["value"] };
    }
    return { false, {} };
}

TransactionInfo SolanaRpcClient::getTransaction(const std::string& txHash) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getTransaction"},
        {"params", {txHash, {{"encoding", "jsonParsed"}, {"maxSupportedTransactionVersion", 0}}}}
    };
    auto j = performRpc(req);
    if (j.contains("result") && !j["result"].is_null()) {
        return { true, j["result"] };
    }
    return { false, {} };
}

TokenBalance SolanaRpcClient::getTokenBalance(const std::string& owner, const std::string& mint) {
    json req1 = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getTokenAccountsByOwner"},
        {"params", {owner, {{"mint", mint}}, {{"encoding", "base64"}}}}
    };
    auto j1 = performRpc(req1);
    auto arr = j1["result"]["value"];
    if (!arr.is_array() || arr.empty()) {
        return { false, 0, "" };
    }
    std::string tokenAccount = arr[0]["pubkey"].get<std::string>();

    json req2 = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getTokenAccountBalance"},
        {"params", {tokenAccount, {{"commitment", "processed"}}}}
    };
    auto j2 = performRpc(req2);
    if (j2["result"].contains("value")) {
        auto v = j2["result"]["value"];
        uint64_t raw = std::stoull(v["amount"].get<std::string>());
        std::string ui = v["uiAmountString"].get<std::string>();
        return { true, raw, ui };
    }
    return { false, 0, "" };
}

std::vector<std::string> SolanaRpcClient::extractPubkeys(const std::vector<uint8_t>& rawData) {
    std::string s(rawData.begin(), rawData.end());
    std::vector<std::string> out;
    std::regex re(R"([A-HJ-NP-Za-km-z1-9]{43,44})");
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        out.push_back(it->str());
    }
    return out;
}

std::string SolanaRpcClient::sendRawTransaction(const std::string& txBase64) {
    std::string resp;
    resp.reserve(16384);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][SOLANA] sendRawTransaction, prefix: "
            << txBase64.substr(0, 100) << "...\n";
    }

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("CURL init failed");

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    std::string req = std::string(R"({"jsonrpc":"2.0","id":1,"method":"sendTransaction","params":[")")
        + txBase64
        + R"(",{"encoding":"base64","skipPreflight":true,"preflightCommitment":"processed"}]})";

    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][SOLANA] Response size: " << resp.size()
            << ", HTTP code: " << http_code << "\n";
        errorLog << "[DEBUG][SOLANA] Body (first 500 bytes): "
            << resp.substr(0, 500) << "\n";
    }

    if (res != CURLE_OK)
        throw std::runtime_error("RPC failed: " + std::string(curl_easy_strerror(res)));
    if (http_code != 200)
        throw std::runtime_error("Invalid HTTP code: " + std::to_string(http_code));

    json j;
    try {
        j = json::parse(resp);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SOLANA] JSON parse error: " << e.what() << "\n";
        throw;
    }

    if (j.contains("result")) return j["result"].get<std::string>();
    throw std::runtime_error("sendRawTransaction failed: " + j.dump());
}


uint64_t SolanaRpcClient::getBalance(const std::string& pubkey) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getBalance"},
        {"params", {pubkey, {{"commitment", "processed"}}}}
    };
    auto j = performRpc(req);
    if (j.contains("result") && j["result"].contains("value")) {
        return j["result"]["value"].get<uint64_t>();
    }
    std::lock_guard<std::recursive_mutex> lock(logMutex);
    errorLog << "[ERROR][SOLANA] getBalance failed: " << j.dump() << "\n";
    throw std::runtime_error("getBalance failed: " + j.dump());
}


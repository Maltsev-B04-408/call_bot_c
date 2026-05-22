#pragma once
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Quote {
    uint64_t outAmount;
};

struct SwapResult {
    std::string swapTransaction;
};

class JupiterClient {
public:
    JupiterClient(const std::string& apiBase, const std::string& userPublicKey);
    Quote getQuote(const std::string& inMint, const std::string& outMint, uint64_t amount);
    SwapResult swap(const std::string& inMint, const std::string& outMint, uint64_t amount, uint32_t slippageBps);
    json httpGet(const std::string& url);
    json httpPost(const std::string& url, const json& body);

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, std::string* headers);

private:
    std::string apiBase_;
    std::string userPublicKey_;
};

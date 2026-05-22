#ifndef SOLANA_RPC_CLIENT_H
#define SOLANA_RPC_CLIENT_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct TokenBalance {
    bool success;
    uint64_t amountRaw;
    std::string amountUi;
};

struct AccountInfo {
    bool success;
    json value;
};

struct TransactionInfo {
    bool success;
    json value;
};

class SolanaRpcClient {
public:
    SolanaRpcClient(const std::string& rpcUrl);

    AccountInfo getAccountInfo(const std::string& pubkey);
    TransactionInfo getTransaction(const std::string& txHash);
    TokenBalance getTokenBalance(const std::string& owner, const std::string& mint);
    std::string sendRawTransaction(const std::string& rawTxBase64);
    uint64_t getBalance(const std::string& pubkey);
    std::vector<std::string> extractPubkeys(const std::vector<uint8_t>& rawData);

private:
    std::string endpoint_;
    json performRpc(const json& payload);
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
};

#endif


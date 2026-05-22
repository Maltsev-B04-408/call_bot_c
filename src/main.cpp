#include <iostream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <chrono>
#include <thread>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <csignal>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <stdexcept>
#include <cstdint>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include "SolanaRpcClient.h"
#include "JupiterClient.h"
#include "TelegramClient.h"

using json = nlohmann::json;

std::unique_ptr<SolanaRpcClient> sol_client_ptr;
std::unique_ptr<JupiterClient> jupiter_client_ptr;
std::shared_ptr<TelegramClient> telegram_bot_ptr;

std::ofstream logFile("bot.log", std::ios::app);
std::ofstream errorLog("error.log", std::ios::app);
std::recursive_mutex logMutex;
std::recursive_mutex callbackLogMutex;

void signalHandler(int signum) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        logFile << "[INFO][MAIN] Received signal " << signum << ", shutting down\n" << std::flush;
        errorLog << "[INFO][MAIN] Received signal " << signum << ", shutting down\n" << std::flush;
    }
    logFile.close();
    errorLog.close();
    exit(signum);
}

static std::string PRIVATE_KEY_BASE58;
static std::string WALLET_ADDRESS;
static std::string SOL_MINT = "So11111111111111111111111111111111111111112";
static uint64_t BUY_SOL_LAMPORTS = 5000000;
static int64_t TRANSACTIONS_CHAT_ID = 0;
static int API_ID = 0;
static std::string API_HASH;
static std::string JUPITER_API_BASE = "https://lite-api.jup.ag/swap/v1";
static std::string SOLANA_RPC_ENDPOINT = "https://api.mainnet-beta.solana.com";
static std::string TELEGRAM_SESSION_DIR = "telegram_session";
static std::vector<std::string> LISTEN_CHANNELS;
static constexpr uint32_t SWAP_SLIPPAGE_BPS = 3000;

static const std::regex RX_MINT(R"(\b[A-HJ-NP-Za-km-z1-9]{43,44}\b)");
static const std::regex RX_DEX(R"(https://dexscreener\.com/solana/([^?]+))");
static const std::regex RX_PUMP(R"(https://pump\.fun/coin/([A-HJ-NP-Za-km-z1-9]+))");
static const std::regex RX_PHOT(R"(https://photon-sol\.tinyastro\.io/\w+/lp/([A-HJ-NP-Za-km-z1-9]+))");

std::unordered_set<std::string> stopList;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> processingTokens;
std::recursive_mutex processingTokensMutex;

static bool firstMessageLogged = false;

std::string nowStr() {
    time_t t = std::time(nullptr);
    struct tm tm_info;
    char buf[64];
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf);
}

static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, std::string* s) {
    size_t total = size * nmemb;
    try {
        s->append((char*)ptr, total);
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Buffer size after append: " << s->size() << '\n' << std::flush;
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Exception in WriteCallback: " << e.what() << '\n' << std::flush;
    }
    return total;
}

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, std::string* headers) {
    headers->append(buffer, size * nitems);
    return size * nitems;
}

std::string trim(std::string value) {
    auto notSpace = [](unsigned char c) {
        return !std::isspace(c);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream ss(value);
    std::string item;

    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            result.push_back(item);
        }
    }

    return result;
}

std::string getEnvValue(const std::string& key) {
#ifdef _WIN32
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, key.c_str()) != 0 || buffer == nullptr) {
        return "";
    }

    std::string value(buffer);
    free(buffer);
    return value;
#else
    const char* value = std::getenv(key.c_str());
    return value == nullptr ? "" : std::string(value);
#endif
}

void setEnvIfMissing(const std::string& key, const std::string& value) {
    if (!getEnvValue(key).empty()) {
        return;
    }

#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 0);
#endif
}

void loadDotEnv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, equalsPos));
        std::string value = trim(line.substr(equalsPos + 1));
        if (key.empty()) {
            continue;
        }

        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        setEnvIfMissing(key, value);
    }
}

std::string requiredEnv(const std::string& key) {
    std::string value = getEnvValue(key);
    if (value.empty()) {
        throw std::runtime_error("Missing required environment variable: " + key);
    }
    return value;
}

std::string optionalEnv(const std::string& key, const std::string& defaultValue) {
    std::string value = getEnvValue(key);
    if (value.empty()) {
        return defaultValue;
    }
    return value;
}

uint64_t parseUint64Env(const std::string& key, uint64_t defaultValue) {
    std::string value = getEnvValue(key);
    if (value.empty()) {
        return defaultValue;
    }
    return std::stoull(value);
}

int64_t parseInt64Env(const std::string& key) {
    return std::stoll(requiredEnv(key));
}

int parseIntEnv(const std::string& key) {
    return std::stoi(requiredEnv(key));
}

void loadConfig() {
    loadDotEnv();

    PRIVATE_KEY_BASE58 = requiredEnv("CALL_BOT_PRIVATE_KEY_BASE58");
    WALLET_ADDRESS = requiredEnv("CALL_BOT_WALLET_ADDRESS");
    API_ID = parseIntEnv("CALL_BOT_TELEGRAM_API_ID");
    API_HASH = requiredEnv("CALL_BOT_TELEGRAM_API_HASH");
    TRANSACTIONS_CHAT_ID = parseInt64Env("CALL_BOT_TRANSACTIONS_CHAT_ID");

    SOL_MINT = optionalEnv("CALL_BOT_SOL_MINT", SOL_MINT);
    BUY_SOL_LAMPORTS = parseUint64Env("CALL_BOT_BUY_SOL_LAMPORTS", BUY_SOL_LAMPORTS);
    JUPITER_API_BASE = optionalEnv("CALL_BOT_JUPITER_API_BASE", JUPITER_API_BASE);
    SOLANA_RPC_ENDPOINT = optionalEnv("CALL_BOT_SOLANA_RPC_ENDPOINT", SOLANA_RPC_ENDPOINT);
    TELEGRAM_SESSION_DIR = optionalEnv("CALL_BOT_TELEGRAM_SESSION_DIR", TELEGRAM_SESSION_DIR);
    LISTEN_CHANNELS = splitCsv(requiredEnv("CALL_BOT_LISTEN_CHANNELS"));

    if (LISTEN_CHANNELS.empty()) {
        throw std::runtime_error("CALL_BOT_LISTEN_CHANNELS must contain at least one channel");
    }
}

std::string httpGet(const std::string& url) {
    std::string resp, headers;
    resp.reserve(16384);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Отправка HTTP GET на: " << url << "\n" << std::flush;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Не удалось инициализировать CURL\n" << std::flush;
        throw std::runtime_error("CURL init failed");
    }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "User-Agent: Mozilla/5.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] HTTP заголовки от " << url << ": " << headers << '\n' << std::flush;
        errorLog << "[DEBUG][HELPER] HTTP тело    от " << url << ": " << resp << '\n' << std::flush;
    }

    if (res != CURLE_OK) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] HTTP GET failed: " << curl_easy_strerror(res) << ", HTTP code: " << http_code << "\n" << std::flush;
        throw std::runtime_error("HTTP GET failed: " + std::string(curl_easy_strerror(res)));
    }

    if (http_code != 200) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Неверный HTTP код: " << http_code << ", ответ: " << resp.substr(0, 500) << "\n" << std::flush;
        throw std::runtime_error("Invalid HTTP code: " + std::to_string(http_code));
    }

    return resp;
}

static const char* B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::vector<uint8_t> decodeBase58(const std::string& in) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Начало декодирования Base58, входная строка: " << in.substr(0, 50) << "...\n" << std::flush;
    }
    std::vector<uint8_t> out;
    for (char c : in) {
        const char* p = std::strchr(B58_ALPHABET, c);
        if (!p) {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[ERROR][HELPER] Неверный символ Base58: " << c << "\n" << std::flush;
            throw std::runtime_error("Invalid Base58 char: " + std::string(1, c));
        }
        int carry = static_cast<int>(p - B58_ALPHABET);
        for (auto& b : out) {
            int v = static_cast<int>(b) * 58 + carry;
            b = static_cast<uint8_t>(v & 0xFF);
            carry = v >> 8;
        }
        while (carry) {
            out.push_back(static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }
    for (char c : in) {
        if (c == B58_ALPHABET[0]) out.push_back(0);
        else break;
    }
    std::reverse(out.begin(), out.end());
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Декодирование Base58 завершено, размер вывода: " << out.size() << " байт\n" << std::flush;
    }
    return out;
}

std::vector<uint8_t> decodeBase64(const std::string& in) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Начало декодирования Base64, входная строка: " << in.substr(0, 50) << "...\n" << std::flush;
        errorLog << "[DEBUG][HELPER] Размер входной строки Base64: " << in.size() << " байт\n" << std::flush;
        errorLog << "[DEBUG][HELPER] Полная входная строка Base64: " << in << "\n" << std::flush;
    }

    if (in.size() % 4 != 0) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Неверная длина Base64 строки, должна быть кратна 4, текущая длина: " << in.size() << "\n" << std::flush;
        throw std::runtime_error("Invalid Base64 length: " + std::to_string(in.size()));
    }

    size_t decoded_buf_size = (in.size() / 4) * 3;
    std::vector<uint8_t> buf(decoded_buf_size);

    int len = EVP_DecodeBlock(buf.data(), reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
    if (len < 0) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Не удалось декодировать Base64, код ошибки: " << len << "\n" << std::flush;
        throw std::runtime_error("Base64 decode failed with code: " + std::to_string(len));
    }

    if (!in.empty() && in[in.size() - 1] == '=') len--;
    if (in.size() > 1 && in[in.size() - 2] == '=') len--;
    buf.resize(static_cast<size_t>(len));

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Декодирование Base64 завершено, размер вывода: " << buf.size() << " байт\n" << std::flush;
    }
    return buf;
}

std::string encodeBase64(const std::vector<uint8_t>& data) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Начало кодирования Base64, размер входных данных: " << data.size() << " байт\n" << std::flush;
    }
    size_t encoded_size = ((data.size() + 2) / 3) * 4;
    std::vector<unsigned char> buf(encoded_size + 1);

    int len = EVP_EncodeBlock(buf.data(), data.data(), static_cast<int>(data.size()));
    if (len < 0) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Не удалось кодировать Base64, код ошибки: " << len << "\n" << std::flush;
        throw std::runtime_error("Base64 encode failed with code: " + std::to_string(len));
    }

    std::string result(buf.begin(), buf.begin() + len);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Кодирование Base64 завершено, размер вывода: " << result.size() << " байт\n" << std::flush;
    }
    return result;
}

uint64_t readShortVecLength(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t value = 0;
    int shift = 0;

    while (offset < data.size()) {
        uint8_t byte = data[offset++];
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
        shift += 7;
        if (shift > 63) {
            break;
        }
    }

    throw std::runtime_error("Invalid Solana shortvec length");
}

std::string signTransaction(const std::string& txBase64) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Начало подписи транзакции, входная строка Base64: " << txBase64.substr(0, 50) << "...\n" << std::flush;
    }

    auto raw = decodeBase64(txBase64);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Транзакция декодирована из Base64, размер: " << raw.size() << " байт\n" << std::flush;
        errorLog << "[DEBUG][HELPER] Полный декодированный raw (hex): ";
        for (uint8_t b : raw) errorLog << std::hex << static_cast<int>(b) << " ";
        errorLog << std::dec << "\n" << std::flush;
    }

    auto sk = decodeBase58(PRIVATE_KEY_BASE58);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Секретный ключ декодирован из Base58, размер: " << sk.size() << " байт\n" << std::flush;
    }

    if (sk.size() != 64) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Неверный размер секретного ключа: " << sk.size() << ", ожидалось 64 байта\n" << std::flush;
        throw std::runtime_error("Invalid secretKey size: " + std::to_string(sk.size()));
    }

    size_t signaturesOffset = 0;
    uint64_t signatureCount = readShortVecLength(raw, signaturesOffset);
    if (signatureCount == 0) {
        throw std::runtime_error("Transaction does not contain a signature slot");
    }

    size_t messageOffset = signaturesOffset + static_cast<size_t>(signatureCount) * crypto_sign_BYTES;
    if (messageOffset >= raw.size()) {
        throw std::runtime_error("Invalid transaction: message is missing after signatures");
    }

    const uint8_t* message = raw.data() + messageOffset;
    size_t messageSize = raw.size() - messageOffset;

    std::vector<uint8_t> sig(crypto_sign_BYTES);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Transaction signature slots: " << signatureCount
                 << ", message offset: " << messageOffset
                 << ", message size: " << messageSize << " bytes\n" << std::flush;
    }

    if (crypto_sign_detached(sig.data(), nullptr, message, messageSize, sk.data()) != 0) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][HELPER] Не удалось подписать транзакцию (crypto_sign_detached failed)\n" << std::flush;
        throw std::runtime_error("Signing failed");
    }
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] crypto_sign_detached завершено успешно\n" << std::flush;
    }

    std::copy(sig.begin(), sig.end(), raw.begin() + static_cast<std::ptrdiff_t>(signaturesOffset));

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Signature written into transaction slot 0\n" << std::flush;
    }

    auto result = encodeBase64(raw);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][HELPER] Подписанная транзакция (sig+raw) закодирована в Base64, размер: " << result.size() << " байт\n" << std::flush;
        errorLog << "[DEBUG][HELPER] Полный result Base64: " << result << "\n" << std::flush;
    }
    return result;
}

static uint64_t getTokenAmountFromBalances(const json& balances, const std::string& targetMint, const std::string& walletAddress) {
    for (const auto& balance : balances) {
        if (balance.contains("mint") && balance["mint"].get<std::string>() == targetMint &&
            balance.contains("owner") && balance["owner"].get<std::string>() == walletAddress &&
            balance.contains("uiTokenAmount") && balance["uiTokenAmount"].contains("amount")) {
            try {
                return std::stoull(balance["uiTokenAmount"]["amount"].get<std::string>());
            }
            catch (const std::exception& e) {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[ERROR][HELPER] Failed to parse raw amount: " << e.what() << " for token " << targetMint << "\n" << std::flush;
                return 0;
            }
        }
    }
    return 0;
}

static std::string getUiTokenAmountFromBalances(const json& balances, const std::string& targetMint, const std::string& walletAddress) {
    for (const auto& balance : balances) {
        if (balance.contains("mint") && balance["mint"].get<std::string>() == targetMint &&
            balance.contains("owner") && balance["owner"].get<std::string>() == walletAddress &&
            balance.contains("uiTokenAmount")) {
            if (balance["uiTokenAmount"].contains("uiAmountString")) {
                return balance["uiTokenAmount"]["uiAmountString"].get<std::string>();
            }
            else if (balance["uiTokenAmount"].contains("uiAmount")) {
                try {
                    double uiAmount = balance["uiTokenAmount"]["uiAmount"].get<double>();
                    int decimals = balance["uiTokenAmount"].value("decimals", 0);
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(decimals) << uiAmount;
                    return oss.str();
                }
                catch (const std::exception& e) {
                    std::lock_guard<std::recursive_mutex> lock(logMutex);
                    errorLog << "[ERROR][HELPER] Failed to parse UI amount: " << e.what() << " for token " << targetMint << "\n" << std::flush;
                    return "0";
                }
            }
        }
    }
    return "0";
}

std::string extractTokenMint(const std::string& text) {
    std::string cleanedText;
    for (char c : text) {
        if (c >= 32 && c <= 126) {
            cleanedText += c;
        }
    }
    std::smatch m;
    if (std::regex_search(cleanedText, m, RX_MINT)) return m.str(0);
    if (std::regex_search(cleanedText, m, RX_DEX)) return m.str(1);
    if (std::regex_search(cleanedText, m, RX_PUMP)) return m.str(1);
    if (std::regex_search(cleanedText, m, RX_PHOT)) return m.str(1);
    return "";
}

std::string resolvePairToTokenMint(const std::string& pairAddress) {
    if (!sol_client_ptr) return "";
    try {
        std::string dexScreenerPairUrl = "https://api.dexscreener.com/latest/dex/pairs/solana/" + pairAddress;
        std::string respPair = httpGet(dexScreenerPairUrl);
        json jPair = json::parse(respPair);
        if (jPair.contains("pair") && jPair["pair"].is_object() &&
            jPair["pair"].contains("baseToken") && jPair["pair"]["baseToken"].is_object() &&
            jPair["pair"]["baseToken"].contains("address")) {
            return jPair["pair"]["baseToken"]["address"].get<std::string>();
        }
        std::string dexScreenerSearchUrl = "https://api.dexscreener.com/latest/dex/search?q=" + pairAddress;
        std::string respSearch = httpGet(dexScreenerSearchUrl);
        json jSearch = json::parse(respSearch);
        if (jSearch.contains("pairs") && jSearch["pairs"].is_array() && !jSearch["pairs"].empty() &&
            jSearch["pairs"][0].is_object() && jSearch["pairs"][0].contains("baseToken") &&
            jSearch["pairs"][0]["baseToken"].is_object() && jSearch["pairs"][0]["baseToken"].contains("address")) {
            return jSearch["pairs"][0]["baseToken"]["address"].get<std::string>();
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][MAIN] DexScreener resolution failed for " << pairAddress << ": " << e.what() << "\n" << std::flush;
    }
    return "";
}

void sellTokens(SolanaRpcClient& sol, JupiterClient& jup, const std::string& tokenMint, uint64_t initialAmountRaw, TelegramClient& bot) {
    std::string sellMsg;
    try {
        if (initialAmountRaw == 0) {
            sellMsg = "Продажа токена " + tokenMint + " отменена: нулевое количество для продажи.";
            logFile << "[INFO][SELL] Selling token " << tokenMint << " cancelled: zero amount to sell.\n" << std::flush;
            bot.sendMessage(TRANSACTIONS_CHAT_ID, sellMsg);
            return;
        }

        uint64_t amountToSellRaw = initialAmountRaw;
        if (amountToSellRaw == 0) {
            sellMsg = "Продажа токена " + tokenMint + " отменена: количество для продажи слишком мало.";
            logFile << "[INFO][SELL] Selling token " << tokenMint << " cancelled: amount to sell is too small. Initial raw: " << initialAmountRaw << "\n" << std::flush;
            bot.sendMessage(TRANSACTIONS_CHAT_ID, sellMsg);
            return;
        }

        logFile << "[INFO][SELL] Attempting to sell " << amountToSellRaw << " raw units of token " << tokenMint << "\n" << std::flush;

        Quote sellQuote = jup.getQuote(tokenMint, SOL_MINT, amountToSellRaw);
        logFile << "[INFO][SELL] Jupiter sell quote for " << tokenMint << " -> SOL: outAmount (SOL lamports) = " << sellQuote.outAmount << "\n" << std::flush;
        if (sellQuote.outAmount == 0) {
            throw std::runtime_error("Jupiter quote returned 0 outAmount for sell.");
        }

        SwapResult sellResult = jup.swap(tokenMint, SOL_MINT, amountToSellRaw, SWAP_SLIPPAGE_BPS);
        std::string signedSell = signTransaction(sellResult.swapTransaction);
        if (signedSell.empty()) throw std::runtime_error("Signing sell transaction failed (empty result).");
        std::string sellTxHash = sol.sendRawTransaction(signedSell);

        sellMsg = "Продан токен " + tokenMint + ". Количество: " + std::to_string(amountToSellRaw) + ". Хэш: https://solscan.io/tx/" + sellTxHash;
        logFile << "[INFO][SELL] " << sellMsg << "\n" << std::flush;
        bot.sendMessage(TRANSACTIONS_CHAT_ID, sellMsg);
    }
    catch (const std::exception& e) {
        sellMsg = "Ошибка при продаже токена " + tokenMint + ": " + e.what();
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][SELL] " << sellMsg << "\n" << std::flush;
        try {
            bot.sendMessage(TRANSACTIONS_CHAT_ID, sellMsg);
        }
        catch (const std::exception& telegram_e) {
            errorLog << "[ERROR][SELL] Failed to send sell error to Telegram: " << telegram_e.what() << "\n" << std::flush;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(processingTokensMutex);
    processingTokens.erase(tokenMint);
}

void processTransaction(const std::string& initialAddressFromTelegram) {
    if (!sol_client_ptr || !jupiter_client_ptr || !telegram_bot_ptr) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][PROCESS] Clients not initialized!\n" << std::flush;
        return;
    }
    SolanaRpcClient& sol = *sol_client_ptr;
    JupiterClient& jup = *jupiter_client_ptr;
    TelegramClient& bot = *telegram_bot_ptr;

    std::string tokenMint = initialAddressFromTelegram;
    logFile << "[INFO][PROCESS] Processing initial address from Telegram: " << initialAddressFromTelegram << "\n" << std::flush;

    std::string extractedAddress = extractTokenMint(initialAddressFromTelegram);
    if (!extractedAddress.empty()) {
        logFile << "[INFO][PROCESS] Extracted address: " << extractedAddress << " from Telegram message.\n" << std::flush;
        tokenMint = extractedAddress;
    }

    if (tokenMint.find("dexscreener.com") != std::string::npos || (tokenMint.length() >= 43 && tokenMint.length() <= 44 && tokenMint.find("pump.fun") == std::string::npos && tokenMint.find("tinyastro.io") == std::string::npos)) {
        std::string potentialPairAddress = tokenMint;
        if (tokenMint.find("dexscreener.com") != std::string::npos) {
            std::smatch m;
            if (std::regex_search(tokenMint, m, RX_DEX)) potentialPairAddress = m.str(1);
        }
        if (potentialPairAddress.length() >= 43 && potentialPairAddress.length() <= 44) {
            std::string resolvedMint = resolvePairToTokenMint(potentialPairAddress);
            if (!resolvedMint.empty()) {
                logFile << "[INFO][PROCESS] Resolved potential pair " << potentialPairAddress << " to token mint: " << resolvedMint << "\n" << std::flush;
                tokenMint = resolvedMint;
            }
            else {
                logFile << "[INFO][PROCESS] Could not resolve " << potentialPairAddress << " via DexScreener, assuming it's a direct mint or invalid pair.\n" << std::flush;
            }
        }
    }

    if (tokenMint.empty() || tokenMint.length() < 43 || tokenMint.length() > 44) {
        logFile << "[INFO][PROCESS] Invalid or empty token mint after extraction/resolution: '" << tokenMint << "'. Skipping.\n" << std::flush;
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(processingTokensMutex);
        if (stopList.count(tokenMint) || processingTokens.count(tokenMint)) {
            logFile << "[INFO][PROCESS] Token " << tokenMint << " is in stopList or already being processed. Skipping.\n" << std::flush;
            return;
        }
        processingTokens[tokenMint] = std::chrono::steady_clock::now();
    }

    std::string buyMsg;
    try {
        logFile << "[INFO][PROCESS] Attempting to buy token: " << tokenMint << " with " << BUY_SOL_LAMPORTS << " lamports of SOL.\n" << std::flush;

        Quote buyQuote = jup.getQuote(SOL_MINT, tokenMint, BUY_SOL_LAMPORTS);
        logFile << "[INFO][PROCESS] Jupiter buy quote for SOL -> " << tokenMint << ": outAmount (token raw) = " << buyQuote.outAmount << "\n" << std::flush;
        if (buyQuote.outAmount == 0) {
            throw std::runtime_error("Jupiter quote returned 0 outAmount for buy.");
        }

        SwapResult buyResult = jup.swap(SOL_MINT, tokenMint, BUY_SOL_LAMPORTS, SWAP_SLIPPAGE_BPS);
        logFile << "[INFO][PROCESS] Jupiter swap response (buy transaction base64): " << buyResult.swapTransaction.substr(0, 50) << "...\n" << std::flush;

        std::string signedBuy = signTransaction(buyResult.swapTransaction);
        logFile << "[INFO][PROCESS] Signed buy transaction: " << signedBuy.substr(0, 50) << "...\n" << std::flush;
        if (signedBuy.empty()) throw std::runtime_error("Signing buy transaction failed (empty result).");

        std::string txHash = sol.sendRawTransaction(signedBuy);
        logFile << "[INFO][PROCESS] Buy transaction sent. Hash: " << txHash << "\n" << std::flush;
        buyMsg = "Куплен токен " + tokenMint + ". Хэш: https://solscan.io/tx/" + txHash;
        bot.sendMessage(TRANSACTIONS_CHAT_ID, buyMsg);

        std::this_thread::sleep_for(std::chrono::seconds(20));
        TransactionInfo txInfo = sol.getTransaction(txHash);

        uint64_t amountBoughtRaw = 0;
        std::string amountBoughtUi = "0";

        if (txInfo.success && txInfo.value.is_object() && txInfo.value.contains("meta")) {
            const auto& meta = txInfo.value["meta"];
            uint64_t amountBeforeRaw = 0;
            uint64_t amountAfterRaw = 0;

            if (meta.is_object() && meta.contains("preTokenBalances")) {
                amountBeforeRaw = getTokenAmountFromBalances(meta["preTokenBalances"], tokenMint, WALLET_ADDRESS);
            }
            if (meta.is_object() && meta.contains("postTokenBalances")) {
                amountAfterRaw = getTokenAmountFromBalances(meta["postTokenBalances"], tokenMint, WALLET_ADDRESS);
                amountBoughtUi = getUiTokenAmountFromBalances(meta["postTokenBalances"], tokenMint, WALLET_ADDRESS);
            }

            logFile << "[INFO][PROCESS] Token " << tokenMint << " balance before tx: " << amountBeforeRaw << " raw. After tx: " << amountAfterRaw << " raw (UI total: " << amountBoughtUi << ").\n" << std::flush;
            if (amountAfterRaw > amountBeforeRaw) {
                amountBoughtRaw = amountAfterRaw - amountBeforeRaw;
                logFile << "[INFO][PROCESS] Calculated amount bought in this tx (raw): " << amountBoughtRaw << " of token " << tokenMint << "\n" << std::flush;
            }
            else if (amountAfterRaw > 0 && amountBeforeRaw == 0) {
                amountBoughtRaw = amountAfterRaw;
                logFile << "[INFO][PROCESS] Calculated amount bought (new total, raw): " << amountBoughtRaw << " of token " << tokenMint << "\n" << std::flush;
            }
            else {
                logFile << "[WARNING][PROCESS] Could not determine positive amount bought for token " << tokenMint << " from tx meta. Raw after: " << amountAfterRaw << ", Raw before: " << amountBeforeRaw << ". Will use total if positive.\n" << std::flush;
                if (amountAfterRaw > 0) amountBoughtRaw = amountAfterRaw;
            }
        }

        if (amountBoughtRaw == 0) {
            logFile << "[WARNING][PROCESS] Amount bought from tx meta is zero for " << tokenMint << ". Falling back to getTokenBalance (total balance).\n" << std::flush;
            TokenBalance tb = sol.getTokenBalance(WALLET_ADDRESS, tokenMint);
            if (tb.success) {
                amountBoughtRaw = tb.amountRaw;
                amountBoughtUi = tb.amountUi;
                logFile << "[INFO][PROCESS] Fallback: Current total balance of token " << tokenMint << " is " << amountBoughtUi << " (" << amountBoughtRaw << " raw).\n" << std::flush;
            }
            else {
                logFile << "[ERROR][PROCESS] Failed to get token balance for " << tokenMint << " as fallback. Cannot determine amount bought.\n" << std::flush;
                throw std::runtime_error("Failed to determine amount of token bought.");
            }
        }

        logFile << "[INFO][PROCESS] Bought token " << tokenMint
                << ". Amount UI: " << amountBoughtUi
                << ", raw for sell: " << amountBoughtRaw
                << ". Hash: https://solscan.io/tx/" << txHash << "\n" << std::flush;

        if (amountBoughtRaw > 0) {
            std::thread sellThread(sellTokens, std::ref(sol), std::ref(jup), tokenMint, amountBoughtRaw, std::ref(bot));
            sellThread.detach();
        }
        else {
            std::string errMsg = "Не удалось определить количество купленного токена " + tokenMint + " для планирования продажи.";
            logFile << "[ERROR][PROCESS] " << errMsg << "\n" << std::flush;
            bot.sendMessage(TRANSACTIONS_CHAT_ID, errMsg);
            std::lock_guard<std::recursive_mutex> lock(processingTokensMutex);
            processingTokens.erase(tokenMint);
        }
        stopList.insert(tokenMint);
    }
    catch (const std::exception& e) {
        buyMsg = "Ошибка при покупке токена " + tokenMint + ": " + e.what();
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][PROCESS] " << buyMsg << "\n" << std::flush;
        try {
            bot.sendMessage(TRANSACTIONS_CHAT_ID, buyMsg);
        }
        catch (const std::exception& telegram_e) {
            errorLog << "[ERROR][PROCESS] Failed to send buy error to Telegram: " << telegram_e.what() << "\n" << std::flush;
        }
        std::lock_guard<std::recursive_mutex> lock_processing(processingTokensMutex);
        processingTokens.erase(tokenMint);
    }
}

void onTelegramMessage(const std::string& chat, const std::string& text) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][CALLBACK] Получено сообщение из чата: " << chat << ", текст: " << text.substr(0, 50) << "...\n" << std::flush;
    }

    if (std::find(LISTEN_CHANNELS.begin(), LISTEN_CHANNELS.end(), chat) == LISTEN_CHANNELS.end()) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        logFile << "[INFO][CALLBACK] Игнорирование сообщения из непрослушиваемого канала: " << chat << "\n" << std::flush;
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(callbackLogMutex);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][CALLBACK] Успешно захвачен callbackLogMutex\n" << std::flush;
    }

    if (!firstMessageLogged) {
        logFile << "[INFO][CALLBACK] Получено новое сообщение в чате: " << chat << "\n" << std::flush;
        firstMessageLogged = true;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][CALLBACK] Текст сообщения: " << text << "\n" << std::flush;
    }

    auto mint = extractTokenMint(text);
    if (mint.empty()) {
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[ERROR][CALLBACK] Не удалось извлечь адрес токена из текста: " << text << "\n" << std::flush;
        }
        telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, "Не удалось извлечь адрес токена из сообщения в чате " + chat);
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][CALLBACK] Извлеченный токен: " << mint << "\n" << std::flush;
    }
    try {
        std::thread t([mint, chat]() {
            {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[DEBUG][THREAD] Поток запущен для токена: " << mint << "\n" << std::flush;
            }
            try {
                processTransaction(mint);
            }
            catch (const std::exception& e) {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[ERROR][THREAD] Ошибка в потоке processTransaction для адреса " << mint << ": " << e.what() << "\n" << std::flush;
                telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, "Не удалось выполнить поток для адреса " + mint + ": " + e.what());
            }
            catch (...) {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[ERROR][THREAD] Неизвестная ошибка в потоке для адреса " << mint << "\n" << std::flush;
                telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, "Неизвестная ошибка в потоке для адреса " + mint);
            }
            {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[DEBUG][THREAD] Поток завершен для токена: " << mint << "\n" << std::flush;
            }
            });
        t.detach();
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[DEBUG][CALLBACK] Поток запущен для токена: " << mint << "\n" << std::flush;
        }
    }
    catch (const std::exception& e) {
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[ERROR][CALLBACK] Не удалось запустить поток processTransaction для адреса " << mint << ": " << e.what() << "\n" << std::flush;
        }
        telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, "Не удалось запустить поток для адреса " + mint + ": " + e.what());
    }
    catch (...) {
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[ERROR][CALLBACK] Неизвестная ошибка при запуске потока для адреса " << mint << "\n" << std::flush;
        }
        telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, "Неизвестная ошибка при запуске потока для адреса " + mint);
    }
}

int main() {
    if (sodium_init() < 0) {
        errorLog << "[ERROR][MAIN] Libsodium initialization failed!\n" << std::flush;
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    logFile << "[INFO][MAIN] Bot starting at " << nowStr() << "\n" << std::flush;

    try {
        loadConfig();

        sol_client_ptr = std::make_unique<SolanaRpcClient>(SOLANA_RPC_ENDPOINT);
        jupiter_client_ptr = std::make_unique<JupiterClient>(JUPITER_API_BASE, WALLET_ADDRESS);
        telegram_bot_ptr = std::make_shared<TelegramClient>(API_ID, API_HASH, TELEGRAM_SESSION_DIR, onTelegramMessage);

        for (const auto& channel : LISTEN_CHANNELS) {
            std::cout << "[DEBUG] Adding listen channel: " << channel << "\n";
            telegram_bot_ptr->addListenChannel(channel);
        }
        telegram_bot_ptr->start();

        logFile << "[INFO][MAIN] Telegram client started. Bot is running.\n" << std::flush;
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            std::lock_guard<std::recursive_mutex> lock(processingTokensMutex);
            auto now = std::chrono::steady_clock::now();
            for (auto it = processingTokens.begin(); it != processingTokens.end(); ) {
                if (std::chrono::duration_cast<std::chrono::minutes>(now - it->second).count() > 10) {
                    logFile << "[WARNING][MAIN] Token " << it->first << " stuck in processing for >10 mins. Removing.\n" << std::flush;
                    it = processingTokens.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[FATAL][MAIN] Unhandled exception in main: " << e.what() << "\n" << std::flush;
        logFile << "[FATAL][MAIN] Unhandled exception in main: " << e.what() << "\n" << std::flush;
        if (telegram_bot_ptr) {
            try {
                telegram_bot_ptr->sendMessage(TRANSACTIONS_CHAT_ID, std::string("Фатальная ошибка бота: ") + e.what());
            }
            catch (const std::exception& te) {
                errorLog << "[FATAL][MAIN] Failed to send fatal error to Telegram: " << te.what() << "\n" << std::flush;
            }
        }
        return 1;
    }
    return 0;
}

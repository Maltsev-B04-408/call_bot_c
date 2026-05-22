#include "JupiterClient.h"
#include <curl/curl.h>
#include <stdexcept>
#include <sstream>
#include <cassert>
#include <climits>
#include <mutex>
#include <fstream>

extern std::recursive_mutex logMutex;
extern std::ofstream errorLog;

using json = nlohmann::json;

size_t JupiterClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    try {
        ((std::string*)userp)->append((char*)contents, realsize);
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] Buffer size after append: " << ((std::string*)userp)->size() << " bytes\n";
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Exception in WriteCallback: " << e.what() << "\n";
    }
    return realsize;
}

size_t JupiterClient::HeaderCallback(char* buffer, size_t size, size_t nitems, std::string* headers) {
    headers->append(buffer, size * nitems);
    return size * nitems;
}

JupiterClient::JupiterClient(const std::string& apiBase, const std::string& userPublicKey)
    : apiBase_(apiBase), userPublicKey_(userPublicKey) {
    std::lock_guard<std::recursive_mutex> lock(logMutex);
    errorLog << "[DEBUG][JUPITER] Initialized with API base: " << apiBase_ << ", user public key: " << userPublicKey_ << "\n";
}

json JupiterClient::httpGet(const std::string& url) {
    std::string resp;
    resp.reserve(16384);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] Sending HTTP GET to: " << url << "\n";
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to initialize CURL\n";
        throw std::runtime_error("CURL init failed");
    }
    std::string headers;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);

    CURLcode r = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] HTTP code: " << http_code << ", Headers: " << headers << "\n";
        errorLog << "[DEBUG][JUPITER] Received response: " << resp << "\n";
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (r != CURLE_OK) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] HTTP GET failed for URL " << url << ": " << curl_easy_strerror(r) << ", HTTP code: " << http_code << "\n";
        throw std::runtime_error("HTTP GET failed: " + std::string(curl_easy_strerror(r)));
    }

    if (http_code != 200) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Invalid HTTP code: " << http_code << ", response: " << resp.substr(0, 500) << "\n";
        throw std::runtime_error("Invalid HTTP code: " + std::to_string(http_code));
    }

    if (headers.find("Content-Type: application/json") == std::string::npos) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Response is not JSON, Content-Type: " << headers << "\n";
        throw std::runtime_error("Response is not JSON");
    }

    if (resp.size() > 1e6) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Response too large: " << resp.size() << " bytes\n";
        throw std::runtime_error("Response too large");
    }

    try {
        if (!resp.empty() && resp[0] == 0xEF && resp[1] == 0xBB && resp[2] == 0xBF) {
            resp = resp.substr(3);
        }
        return json::parse(resp);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to parse JSON response from " << url << ": " << e.what() << "\n";
        throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()));
    }
}

json JupiterClient::httpPost(const std::string& url, const json& body) {
    std::string resp;
    resp.reserve(16384);
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] Sending HTTP POST to: " << url << ", request body: " << body.dump() << "\n";
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to initialize CURL\n";
        throw std::runtime_error("CURL init failed");
    }
    std::string headers;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    std::string b = body.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, b.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);

    CURLcode r = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] HTTP code: " << http_code << ", Headers: " << headers << "\n";
        errorLog << "[DEBUG][JUPITER] Received response: " << resp << "\n";
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (r != CURLE_OK) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] HTTP POST failed for URL " << url << ": " << curl_easy_strerror(r) << ", HTTP code: " << http_code << "\n";
        throw std::runtime_error("HTTP POST failed: " + std::string(curl_easy_strerror(r)));
    }

    if (http_code != 200) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Invalid HTTP code: " << http_code << ", response: " << resp.substr(0, 500) << "\n";
        try {
            json j = json::parse(resp);
            if (j.contains("error")) {
                std::string errorMsg = j["error"].get<std::string>();
                errorLog << "[ERROR][JUPITER] Jupiter API error: " << errorMsg << "\n";
                throw std::runtime_error("Jupiter API error: " + errorMsg);
            }
        }
        catch (...) {
        }
        throw std::runtime_error("Invalid HTTP code: " + std::to_string(http_code));
    }

    if (headers.find("Content-Type: application/json") == std::string::npos) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Response is not JSON, Content-Type: " << headers << "\n";
        throw std::runtime_error("Response is not JSON");
    }

    try {
        if (!resp.empty() && resp[0] == 0xEF && resp[1] == 0xBB && resp[2] == 0xBF) {
            resp = resp.substr(3);
        }
        return json::parse(resp);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to parse JSON response from " << url << ": " << e.what() << "\n";
        throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()));
    }
}

Quote JupiterClient::getQuote(const std::string& inMint, const std::string& outMint, uint64_t amount) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] Getting quote for inputMint=" << inMint << ", outputMint=" << outMint << ", amount=" << amount << "\n";
    }
    std::ostringstream oss;
    if (inMint.length() > static_cast<size_t>(INT_MAX) || outMint.length() > static_cast<size_t>(INT_MAX)) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Input or output mint string length exceeds INT_MAX\n";
        throw std::runtime_error("Mint string length exceeds INT_MAX");
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to initialize CURL\n";
        throw std::runtime_error("CURL init failed");
    }
    char* escapedInMint = curl_easy_escape(curl, inMint.c_str(), static_cast<int>(inMint.length()));
    char* escapedOutMint = curl_easy_escape(curl, outMint.c_str(), static_cast<int>(outMint.length()));
    oss << apiBase_ << "/quote"
        << "?inputMint=" << escapedInMint
        << "&outputMint=" << escapedOutMint
        << "&amount=" << amount;
    curl_free(escapedInMint);
    curl_free(escapedOutMint);
    curl_easy_cleanup(curl);

    try {
        auto j = httpGet(oss.str());
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[DEBUG][JUPITER] Quote response: " << j.dump() << "\n";
        }
        if (j.contains("outAmount")) {
            uint64_t outAmt = std::stoull(j["outAmount"].get<std::string>());
            {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[DEBUG][JUPITER] Parsed outAmount: " << outAmt << "\n";
            }
            uint64_t fee = uint64_t(outAmt * 0.0003);
            {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[DEBUG][JUPITER] Calculated fee: " << fee << ", final outAmount: " << (outAmt > fee ? outAmt - fee : outAmt) << "\n";
            }
            return { outAmt > fee ? outAmt - fee : outAmt };
        }
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] getQuote failed: missing 'outAmount' in response\n";
        throw std::runtime_error("getQuote: unexpected response");
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] getQuote failed for inputMint=" << inMint << ", outputMint=" << outMint << ": " << e.what() << "\n";
        throw;
    }
}

SwapResult JupiterClient::swap(const std::string& inMint, const std::string& outMint, uint64_t amount, uint32_t slippageBps) {
    {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[DEBUG][JUPITER] Initiating swap for inputMint=" << inMint << ", outputMint=" << outMint << ", amount=" << amount << ", slippageBps=" << slippageBps << "\n";
    }
    std::ostringstream quoteUrl;
    if (inMint.length() > static_cast<size_t>(INT_MAX) || outMint.length() > static_cast<size_t>(INT_MAX)) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Input or output mint string length exceeds INT_MAX\n";
        throw std::runtime_error("Mint string length exceeds INT_MAX");
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to initialize CURL\n";
        throw std::runtime_error("CURL init failed");
    }
    char* escapedInMint = curl_easy_escape(curl, inMint.c_str(), static_cast<int>(inMint.length()));
    char* escapedOutMint = curl_easy_escape(curl, outMint.c_str(), static_cast<int>(outMint.length()));
    quoteUrl << apiBase_ << "/quote"
        << "?inputMint=" << escapedInMint
        << "&outputMint=" << escapedOutMint
        << "&amount=" << amount
        << "&slippageBps=" << slippageBps;
    curl_free(escapedInMint);
    curl_free(escapedOutMint);
    curl_easy_cleanup(curl);

    json quoteResponse;
    try {
        quoteResponse = httpGet(quoteUrl.str());
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[DEBUG][JUPITER] Quote response for swap: " << quoteResponse.dump() << "\n";
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] Failed to get quote for swap: " << e.what() << "\n";
        throw;
    }

    json body = {
        {"quoteResponse", quoteResponse},
        {"userPublicKey", userPublicKey_},
        {"wrapAndUnwrapSol", true}
    };

    try {
        auto j = httpPost(apiBase_ + "/swap", body);
        {
            std::lock_guard<std::recursive_mutex> lock(logMutex);
            errorLog << "[DEBUG][JUPITER] Swap response: " << j.dump() << "\n";
        }
        if (j.contains("swapTransaction")) {
            std::string swapTx = j["swapTransaction"].get<std::string>();
            {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[DEBUG][JUPITER] Swap transaction obtained: " << swapTx.substr(0, 50) << "...\n";
                errorLog << "[DEBUG][JUPITER] Full swapTransaction: " << swapTx << "\n";
            }
            bool isValidBase64 = true;
            for (char c : swapTx) {
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
                    isValidBase64 = false;
                    break;
                }
            }
            if (!isValidBase64) {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[ERROR][JUPITER] swapTransaction contains invalid Base64 characters\n";
                throw std::runtime_error("Invalid Base64 in swapTransaction");
            }
            if (swapTx.size() % 4 != 0) {
                std::lock_guard<std::recursive_mutex> lock(logMutex);
                errorLog << "[ERROR][JUPITER] swapTransaction length is not divisible by 4: " << swapTx.size() << "\n";
                throw std::runtime_error("Invalid Base64 length in swapTransaction");
            }
            return { swapTx };
        }
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] swap failed: missing 'swapTransaction' in response\n";
        if (j.contains("error")) {
            std::string errorMsg = j["error"].get<std::string>();
            errorLog << "[ERROR][JUPITER] Jupiter API error: " << errorMsg << "\n";
            throw std::runtime_error("Jupiter API error: " + errorMsg);
        }
        throw std::runtime_error("swap: unexpected response");
    }
    catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lock(logMutex);
        errorLog << "[ERROR][JUPITER] swap failed for inputMint=" << inMint << ", outputMint=" << outMint << ": " << e.what() << "\n";
        throw;
    }
}

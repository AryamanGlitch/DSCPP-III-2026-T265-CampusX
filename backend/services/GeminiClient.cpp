#include "GeminiClient.h"

#include <cstdlib>
#include <fstream>
#include <mutex>

#ifdef CAMPUSX_HAVE_CURL
#include <curl/curl.h>
#endif

#include "../util/Json.h"
#include "../util/JsonParse.h"
#include "../util/Text.h"

namespace cx {

namespace {
const char* ENDPOINT = "https://generativelanguage.googleapis.com/v1beta/models/";
// Fastest first: on a free key the bigger models are often busy (503) or slow.
const char* DEFAULT_MODELS[] = {"gemini-flash-lite-latest", "gemini-3.1-flash-lite", "gemini-3.8-flash"};

#ifdef CAMPUSX_HAVE_CURL
std::size_t collect(char* data, std::size_t size, std::size_t count, void* out) {
    static_cast<std::string*>(out)->append(data, size * count);
    return size * count;
}

// POST JSON; returns the HTTP status, or 0 for a network error/timeout (curlError says which).
int post(const std::string& url, const std::string& apiKey, const std::string& body, long timeout,
         std::string& response, std::string& curlError) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) { curlError = "could not start libcurl"; return 0; }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string keyHeader = "x-goog-api-key: " + apiKey;  // header, not URL, so the key never lands in logs
    headers = curl_slist_append(headers, keyHeader.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // timeouts must not use signals in a threaded server
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else curlError = rc == CURLE_OPERATION_TIMEDOUT ? "timed out" : curl_easy_strerror(rc);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return static_cast<int>(status);
}
#endif
}  // namespace

GeminiClient::GeminiClient(std::string apiKey, std::vector<std::string> models, long timeoutSeconds)
    : apiKey_(trim(apiKey)), models_(std::move(models)), timeoutSeconds_(timeoutSeconds) {}

bool GeminiClient::available() {
#ifdef CAMPUSX_HAVE_CURL
    return true;
#else
    return false;
#endif
}

std::string GeminiClient::loadKey(const std::string& keyFile) {
    const char* env = std::getenv("GEMINI_API_KEY");
    if (env && *env) return trim(env);
    std::ifstream in(keyFile);
    std::string line;
    if (in && std::getline(in, line)) return trim(line);
    return "";
}

std::vector<std::string> GeminiClient::loadModels() {
    std::vector<std::string> models;
    const char* env = std::getenv("GEMINI_MODELS");
    if (env && *env) {
        std::string cur;
        for (const char* p = env;; ++p) {
            if (*p == ',' || *p == '\0') {
                if (!trim(cur).empty()) models.push_back(trim(cur));
                cur.clear();
                if (*p == '\0') break;
            } else {
                cur += *p;
            }
        }
    }
    if (models.empty()) models.assign(std::begin(DEFAULT_MODELS), std::end(DEFAULT_MODELS));
    return models;
}

std::string GeminiClient::requestBody(const std::string& prompt, const std::string& model) {
    Json config = Json::object({{"responseMimeType", "application/json"}, {"temperature", 0.7}});
    // Gemini 3 models "think" before answering, which can take over a minute; keep it short.
    if (model.rfind("gemini-3", 0) == 0) config.set("thinkingConfig", Json::object({{"thinkingLevel", "low"}}));
    Json parts = Json::array();
    parts.push(Json::object({{"text", prompt}}));
    Json contents = Json::array();
    contents.push(Json::object({{"role", "user"}, {"parts", parts}}));
    return Json::object({{"contents", contents}, {"generationConfig", config}}).dump();
}

GeminiResult GeminiClient::parseResponse(int status, const std::string& body) {
    GeminiResult r;
    if (status == 429) { r.error = "Gemini's free-tier limit was reached. Wait a minute and try again."; return r; }
    if (status == 503 || status >= 500) { r.error = "Gemini is busy right now. Try again in a moment."; return r; }
    if (status == 401 || status == 403) { r.error = "Gemini rejected the API key."; return r; }
    if (status == 404) { r.error = "This Gemini model isn't available for this key."; return r; }
    if (status != 200) { r.error = "Gemini returned HTTP " + std::to_string(status) + "."; return r; }
    try {
        JsonValue doc = parseJson(body);
        const JsonValue* candidates = doc.get("candidates");
        if (!candidates || !candidates->isArray() || candidates->items.empty()) {
            const JsonValue* feedback = doc.get("promptFeedback");
            r.error = feedback && !feedback->str("blockReason").empty() ? "Gemini blocked the request." : "Gemini sent no answer.";
            return r;
        }
        const JsonValue* content = candidates->items[0].get("content");
        const JsonValue* parts = content ? content->get("parts") : nullptr;
        if (parts && parts->isArray()) {
            for (const JsonValue& p : parts->items) {
                const JsonValue* thought = p.get("thought");
                if (thought && thought->boolean) continue;  // skip the model's reasoning, keep the answer
                r.text += p.str("text");
            }
        }
        if (trim(r.text).empty()) { r.error = "Gemini sent an empty answer."; return r; }
        r.ok = true;
    } catch (const std::exception&) {
        r.error = "Gemini sent a response that isn't valid JSON.";
    }
    return r;
}

GeminiResult GeminiClient::generateJson(const std::string& prompt) const {
    GeminiResult last;
    if (!available()) { last.error = "This build has no libcurl, so Gemini is off."; return last; }
    if (apiKey_.empty()) { last.error = "Gemini isn't set up (no GEMINI_API_KEY or gemini.key)."; return last; }
#ifdef CAMPUSX_HAVE_CURL
    for (const std::string& model : models_) {
        std::string response, curlError;
        int status = post(std::string(ENDPOINT) + model + ":generateContent", apiKey_, requestBody(prompt, model),
                          timeoutSeconds_, response, curlError);
        if (status == 0) {
            last = GeminiResult{};
            last.error = "Gemini didn't answer (" + curlError + ").";
            continue;  // try the next model
        }
        last = parseResponse(status, response);
        last.model = model;
        if (last.ok || status == 401 || status == 403) return last;  // success, or a bad key: no point retrying
    }
#endif
    return last;
}

}  // namespace cx

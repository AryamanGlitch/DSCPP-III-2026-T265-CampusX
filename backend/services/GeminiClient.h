#pragma once
// Gemini API client (HTTPS POST via libcurl). Only this class talks to Google;
// it does not calculate gaps or rank anything.
//
// The API key stays on the server: it comes from the GEMINI_API_KEY environment
// variable or the git-ignored file gemini.key, never from the browser.
// Free-tier keys are often refused with "busy" (503) or "rate limited" (429),
// so each request tries the models in order until one answers.
#include <string>
#include <vector>

namespace cx {

struct GeminiResult {
    bool ok = false;
    std::string text;   // the model's answer (JSON text when ok)
    std::string model;  // which model answered
    std::string error;  // short, user-facing reason when !ok
};

class GeminiClient {
public:
    GeminiClient(std::string apiKey, std::vector<std::string> models, long timeoutSeconds = 25);

    bool configured() const { return !apiKey_.empty() && available(); }
    static bool available();  // false when built without libcurl

    // Asks for a JSON answer to `prompt`.
    GeminiResult generateJson(const std::string& prompt) const;

    // Exposed for tests: request body for one model, and the answer text from a response body.
    static std::string requestBody(const std::string& prompt, const std::string& model);
    static GeminiResult parseResponse(int httpStatus, const std::string& body);

    // Key from GEMINI_API_KEY, else the first line of `keyFile` ("" if neither).
    static std::string loadKey(const std::string& keyFile);
    // Models from GEMINI_MODELS (comma-separated), else the defaults.
    static std::vector<std::string> loadModels();

private:
    std::string apiKey_;
    std::vector<std::string> models_;
    long timeoutSeconds_;
};

}  // namespace cx

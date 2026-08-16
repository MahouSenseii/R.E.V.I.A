#pragma once

#include "Library/structLibrary.h"

#include <stop_token>
#include <string>

class llamaCppEmbeddingService
{
public:
    void ApplySettings(const embeddingSettings& settings);
    healthOutput CheckHealth() const;
    embeddingOutput EmbedQuery(
        const std::string& text,
        std::stop_token stopToken = {}) const;
    embeddingOutput EmbedDocument(
        const std::string& text,
        std::stop_token stopToken = {}) const;
    bool IsEnabled() const;
    const std::string& ModelName() const;

private:
    embeddingOutput Embed(
        const std::string& text,
        const std::string& prefix,
        std::stop_token stopToken) const;

    bool enabled = false;
    std::string host = "127.0.0.1";
    int port = 8081;
    std::string modelName;
    std::string apiKey;
    std::string queryPrefix = "search_query: ";
    std::string documentPrefix = "search_document: ";
};

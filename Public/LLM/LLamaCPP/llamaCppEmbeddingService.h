#pragma once


#include "LLM/backendTypes.h"
#include "LLM/embeddingTypes.h"
#include "LLM/endpointSettings.h"
#include <cstddef>
#include <list>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>

class llamaCppEmbeddingService
{
public:
    void ApplySettings(const embeddingSettings& settings);
    healthOutput CheckHealth(std::stop_token stopToken = {}) const;
    embeddingOutput EmbedQuery(const std::string& text, std::stop_token stopToken = {}) const;
    embeddingOutput EmbedDocument(const std::string& text, std::stop_token stopToken = {}) const;
    bool IsEnabled() const;
    const std::string& ModelName() const;

private:
    embeddingOutput Embed(const std::string& text, const std::string& prefix, std::stop_token stopToken) const;

    // Recent successful embeddings, newest first. The same text is embedded more than
    // once per turn -- for the reply's recall, for self-inquiry, and for the memory
    // classifier's duplicate check -- and each repeat cost a CPU embedding call.
    static constexpr std::size_t CacheCapacity = 64;
    using CacheEntry = std::pair<std::string, embeddingOutput>;
    mutable std::mutex cacheMutex;
    mutable std::list<CacheEntry> cache;
    mutable std::unordered_map<std::string, std::list<CacheEntry>::iterator> cacheIndex;

    bool enabled = false;
    std::string host = "127.0.0.1";
    int port = 8081;
    std::string modelName;
    std::string apiKey;
    bool bVerifyServerKey = false;
    std::string queryPrefix = "search_query: ";
    std::string documentPrefix = "search_document: ";
};

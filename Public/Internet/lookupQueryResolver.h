#pragma once

#include <string>

namespace revia::internet
{

// Search subject resolution is separate from the policy that decides whether to search.
struct ResolvedLookupQuery
{
    // False when the request names nothing searchable. The caller must not fall back to
    // the raw sentence: sending the instruction to a search engine is the defect this
    // exists to remove.
    bool resolved = false;
    // The subject, with the original casing and punctuation of whatever survived. Error
    // codes, namespaces, and version strings only work as search terms if they are
    // returned exactly as the user wrote them.
    std::string query;
    // Plain diagnostic sentence. Never contains conversation content beyond the
    // requested subject itself.
    std::string reason;
};

// Removes anchored conversational wrappers while preserving subject words such as
// "search algorithm". Research-topic parsing has different rules and must remain separate.
[[nodiscard]] ResolvedLookupQuery ResolveLookupQuery(const std::string& input);

} // namespace revia::internet

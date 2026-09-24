#pragma once

#include <string>

namespace revia::internet
{

// What a conversational request should actually put in the search box.
//
// Separate from InternetLookupPolicy on purpose. The policy answers "should Revia
// search at all"; this answers "search for what". Running them together would mean a
// question that is worth searching gets searched for the wrong thing.
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

// Strips the conversational scaffolding around a search request and keeps the subject.
//
// Deterministic and local. No model call: a resolver that had to ask a model what to
// search would add a second failure mode and a round trip to every lookup, and the
// answer would still not be checkable.
//
// Wrapper removal is anchored to sentence position rather than applied as substring
// removal, because the command words are also ordinary subject words. "search algorithm
// complexity" and "lookup table performance C++" are subjects that begin with a search
// verb, and both must survive intact.
//
// Related to autonomy::ResolveResearchTopic, which answers the same shape of question
// for autonomous research, but deliberately not shared with it. That one takes a topic
// candidate and strips a leading research verb including a bare "search", which is
// correct for a curiosity candidate and would silently corrupt a conversational subject
// that legitimately starts with one.
[[nodiscard]] ResolvedLookupQuery ResolveLookupQuery(const std::string& input);

} // namespace revia::internet

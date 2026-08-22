#pragma once

#include <string>

namespace revia::memory
{

// The one list of markers that keeps a secret out of durable storage.
//
// Both the automatic memory classifier and the conversation archive consult this. Two
// copies of the list would eventually disagree, and the copy that disagreed by being
// shorter would be the one that wrote a password to disk.
//
// It is a coarse filter and is meant to be: it costs a false positive now and then, and
// the alternative failure -- a credential in a database the user forgot exists -- is not
// one that a later fix can undo.
[[nodiscard]] bool ContainsSensitiveContent(const std::string& text);

} // namespace revia::memory

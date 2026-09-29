#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace revia::internet
{

// The pages she looked up, read in quarantine.
//
// A fetched page used to go into her prompt whole, labelled untrusted. A label is a
// request; a page that says "ignore your instructions" is still read by the model that
// writes the reply. So the pages now go to a reader instead: a bounded model call with
// no tools, no memory and no one to obey, whose only output is a fixed shape -- claims
// tied to numbered sources, each with a quote that must actually appear on the page.
// Her prompt gets the claims. A page can at worst produce a wrong claim, which she
// treats as any other reference; it cannot produce an instruction. This is the dual
// model pattern from the prompt-injection design-patterns literature, with the
// verified quote as the check that the reader did not make things up either.
struct WebSource
{
    int number = 0;
    std::string title;
    std::string url;
    std::string text;
};

struct WebFinding
{
    int source = 0;
    std::string claim;
    std::string quote;
    // The quote was found on the page it is attributed to. A claim without one is kept
    // and marked, so she can weigh it accordingly.
    bool quoteVerified = false;
};

struct WebFindings
{
    // False only when the reader did not answer in its shape; no findings from pages
    // that did not answer the question is a success.
    bool succeeded = false;
    std::vector<WebSource> sources;
    std::vector<WebFinding> findings;
    std::string unanswered;
    std::string reason;
};

// A lookup's grounding, as its sources. The visible browser writes one section per
// page: a title line, "URL: <address>", the page text, "Source: <address>"; an API
// backend writes summaries with a "Source:" line each. Anything else becomes one source
// per entry. Numbered from 1 in the order found.
[[nodiscard]] std::vector<WebSource> SplitGroundingSources(
    const std::string& content,
    const std::vector<std::string>& entries);

// The envelope the reader reads: the question, then each source cut so the whole
// fits `maximumCharacters`, every source getting a share.
[[nodiscard]] std::string BuildReaderEnvelope(
    const std::string& question,
    const std::vector<WebSource>& sources,
    std::size_t maximumCharacters);

// What the reader is told it is, and the shape it must answer in.
[[nodiscard]] const char* ReaderPrompt();
[[nodiscard]] const char* ReaderSchema();

// The reader's answer, checked: a source number it did not have, an empty claim or a
// quote that is not on the page are dropped or marked; at most eight findings.
[[nodiscard]] WebFindings ParseReaderResponse(
    const std::string& response,
    std::vector<WebSource> sources);

// The block her prompt carries: the numbered sources, each finding with its number
// and its quote, what stayed unanswered, and how to cite. Starts with `marker`.
[[nodiscard]] std::string RenderFindingsForPrompt(
    const WebFindings& findings,
    std::string_view marker);

// When the pages were not read: their titles and addresses only, and why, so she can
// point at them without pretending to know what they say.
[[nodiscard]] std::string RenderUnreadSources(
    const std::vector<WebSource>& sources,
    const std::string& reason,
    std::string_view marker);

// The citation lines a shell shows under the reply: the sources the reply cites as
// [n], in order, or every source a finding drew on when it cites none. Only addresses
// the lookup returned can appear here, which is what makes a citation worth showing.
[[nodiscard]] std::vector<std::string> RenderCitations(
    const WebFindings& findings,
    const std::string& reply);

} // namespace revia::internet

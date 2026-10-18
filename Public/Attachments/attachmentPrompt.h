#pragma once

#include "Attachments/attachmentTypes.h"
#include "Attachments/documentStore.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace revia::attachments
{

// Untrusted file text with every runtime prompt marker and every attachment block
// delimiter broken, matched without regard to case. A document that contains
// "[End of runtime context]" or "END FILE>>>" cannot close the block it is quoted in,
// open another, or speak with the runtime's authority.
[[nodiscard]] std::string NeutralizeReferenceText(std::string text);

// "[report.pdf, p. 3]" for a paged document, "[notes.md, part 2]" otherwise.
[[nodiscard]] std::string Citation(const std::string& name, std::size_t page, std::size_t chunkIndex);

// What the turn shows of one attached file.
struct AttachedFileView
{
    std::string name;
    AttachmentKind kind = AttachmentKind::Text;
    std::uintmax_t bytes = 0;
    std::size_t pages = 0;
    // Images: whether the model receives the picture itself, and if not, why not.
    bool imageVisible = false;
    std::string imageUnavailableReason;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Documents: the whole text when it is short, otherwise the retrieved passages.
    std::string wholeText;
    std::vector<std::size_t> pageOffsets;
    std::vector<DocumentHit> passages;
    std::size_t totalChunks = 0;
    std::string note;
};

// The per-turn block for files attached to this message. Bounded to `maxBytes`.
[[nodiscard]] std::string BuildAttachmentBlock(const std::vector<AttachedFileView>& files, std::size_t maxBytes = 16000);

// The per-turn block for passages retrieved from earlier attachments, with citations.
[[nodiscard]] std::string BuildDocumentSearchBlock(const std::vector<DocumentHit>& hits, std::size_t maxBytes = 12000);

} // namespace revia::attachments

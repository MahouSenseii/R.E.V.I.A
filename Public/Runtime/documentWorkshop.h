#pragma once

#include "Content/workingDocument.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Runtime/turnContext.h"
#include "Visual/svgCanvas.h"
#include "Visual/imageGenerator.h"

#include <filesystem>
#include <string>
#include <vector>

namespace revia::runtime
{

// Everything Revia makes that is a thing rather than a reply.
//
// Drafting into the working document, revising one block of it, generating a picture,
// showing one, and drawing a diagram: five entry points that shared one piece of state
// and had nothing to do with anything else in the session. They lived in ReviaSession
// because the working document did, and the working document lived there because that is
// where everything lived.
//
// This is an ownership move and not a file split. `WorkingDocument` is a member here now,
// not a member of the session forwarded to. The session cannot reach into a draft, cannot
// revise a block, and cannot publish to the canvas on this path; it composes this object,
// hands it one turn's worth of context, and applies the events that come back.
//
// The dependencies are named one at a time on purpose. Five of them, each used by name in
// the code below, none of them the session. A constructor that took a `ReviaSession&`, or
// a struct of callbacks shaped like the session's private methods, would be the same
// monolith with an extra indirection -- which is the thing the brief rules out, and which
// is why this took a turn boundary to do rather than a rename.
//
// What stays with the session: whether a turn may run at all, what the runtime state
// actually becomes, the conversation, the foreground lock. Those are session lifecycle.
// Deciding what a draft should say is not.
class DocumentWorkshop
{
public:
    // How the approved roots and the media folder are read.
    //
    // Supplied as values at call time rather than held, because both can change under a
    // running session and a cached copy would enforce yesterday's policy. `ShowPicture`
    // is the only caller and it is a scope check, which must never be stale.
    struct PictureScope
    {
        std::vector<std::filesystem::path> approvedRoots;
        std::string mediaPath;
    };

    DocumentWorkshop(
        messageRouter& router,
        visual::ImageGenerator& imageGenerator,
        visual::DiagramStore& diagramStore,
        logger& log);

    DocumentWorkshop(const DocumentWorkshop&) = delete;
    DocumentWorkshop& operator=(const DocumentWorkshop&) = delete;

    // The working document, which this owns. Exposed read-only for the shell; every
    // change to it goes through one of the calls below.
    [[nodiscard]] const content::WorkingDocument& Document() const { return document; }
    // The two mutations the command surface performs directly. Exposed rather than
    // reimplemented: clearing and stepping back are operations on the document, and the
    // document lives here. A session that kept its own copy to clear would be two
    // documents pretending to be one.
    void ClearDocument() { document.Clear(); }
    [[nodiscard]] bool UndoRevision() { return document.Undo(); }

    [[nodiscard]] TurnOutcome ComposeDocument(const std::string& request);
    [[nodiscard]] TurnOutcome ReviseDocumentBlock(
        const std::string& reference, const std::string& instruction);
    [[nodiscard]] TurnOutcome GenerateImage(const std::string& prompt);
    [[nodiscard]] TurnOutcome ShowPicture(const std::string& path, const PictureScope& scope);
    [[nodiscard]] TurnOutcome DrawDiagram(const std::string& request);

private:
    messageRouter& router;
    visual::ImageGenerator& imageGenerator;
    visual::DiagramStore& diagramStore;
    logger& log;
    // Owned outright. This is the state that makes the extraction an extraction.
    content::WorkingDocument document;
};

} // namespace revia::runtime

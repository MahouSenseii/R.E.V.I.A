#pragma once

#include "Actions/actionRuntime.h"
#include "Content/workingDocument.h"
#include "Core/logger.h"
#include "Core/messageRouter.h"
#include "Runtime/turnContext.h"
#include "Visual/svgCanvas.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace revia::runtime
{

// Owns document edits and visual creation; the session applies returned turn events.
class DocumentWorkshop
{
public:
    // Read live scope at call time so picture checks use current permissions.
    struct PictureScope
    {
        std::vector<std::filesystem::path> approvedRoots;
        std::string mediaPath;
    };

    DocumentWorkshop(messageRouter& router, actions::ActionRuntime& actionRuntime, visual::DiagramStore& diagramStore, logger& log);

    DocumentWorkshop(const DocumentWorkshop&) = delete;
    DocumentWorkshop& operator=(const DocumentWorkshop&) = delete;

    // Read-only view; mutations stay with this owner.
    [[nodiscard]] const content::WorkingDocument& Document() const { return document; }
    void ClearDocument() { document.Clear(); }
    [[nodiscard]] bool UndoRevision() { return document.Undo(); }

    [[nodiscard]] TurnOutcome ComposeDocument(const std::string& request);
    [[nodiscard]] TurnOutcome ReviseDocumentBlock(const std::string& reference, const std::string& instruction);
    [[nodiscard]] TurnOutcome GenerateImage(const std::string& prompt, std::stop_token stopToken = {}, std::function<bool()> admission = {},
        const RuntimeStamp& stamp = {}, bool publishToCanvas = true, std::string requestedBy = "user");
    [[nodiscard]] TurnOutcome ShowPicture(const std::string& path, const PictureScope& scope);
    [[nodiscard]] TurnOutcome DrawDiagram(const std::string& request);

  private:
    messageRouter& router;
    actions::ActionRuntime& actionRuntime;
    visual::DiagramStore& diagramStore;
    logger& log;
    content::WorkingDocument document;
};

} // namespace revia::runtime

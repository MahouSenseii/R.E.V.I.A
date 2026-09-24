#include "Core/utf8.h"
#include "Runtime/documentWorkshop.h"

#include "Actions/actionTypes.h"
#include "Core/runtimePath.h"
#include "Library/structLibrary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <system_error>
#include <utility>

namespace revia::runtime
{

namespace
{

// Copied rather than shared, and only these two.
//
// They were static helpers in reviaSession.cpp, which is a translation unit this has no
// business including. Two five-line string utilities are not a dependency worth creating
// a header for, and reaching into the session's anonymous namespace to borrow them would
// have made the extraction a lie.
double ElapsedMilliseconds(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::string Trim(const std::string& value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

DocumentWorkshop::DocumentWorkshop(
    messageRouter& messageRouter,
    visual::ImageGenerator& generator,
    visual::DiagramStore& store,
    logger& sessionLogger)
    : router(messageRouter)
    , imageGenerator(generator)
    , diagramStore(store)
    , log(sessionLogger)
{
}

TurnOutcome DocumentWorkshop::ComposeDocument(const std::string& request)
{
    TurnOutcome turn;
    if (request.empty())
    {
        turn.result.succeeded = false;
        turn.result.text = "Usage: /write <what you want drafted>";
        turn.result.reason = "No drafting request was given.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    turn.Then(TurnEvent::Moved(RuntimeState::Thinking, "Drafting into the working document."));
    turn.Then(TurnEvent::Progress("Document", "Drafting", "Composing new material."));
    const auto started = std::chrono::steady_clock::now();
    // The existing document goes in as context so a second pass matches the voice of the
    // first rather than starting a new one.
    const responseOutput composed = router.ComposeContent(request, document.Render());
    const double elapsed = ElapsedMilliseconds(started);

    std::ostringstream trace;
    trace << "Drafting: asked the local model for new material";
    if (!document.IsEmpty())
    {
        trace << ", with the existing " << document.Blocks().size()
            << " blocks supplied as context for voice and continuity";
    }
    trace << '.';
    if (!composed.reasoning.empty())
    {
        trace << "\n\nReasoning:\n" << composed.reasoning;
    }

    if (!composed.bSuccess || Trim(composed.response).empty())
    {
        turn.result.succeeded = false;
        turn.result.text = composed.bSuccess ? "The draft came back empty." : composed.response;
        turn.result.reason = composed.reason;
        trace << "\n\nNothing was written: " << turn.result.text;
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Progress("Document", "Error", turn.result.text));
        turn.Then(TurnEvent::Moved(RuntimeState::Error, turn.result.reason));
        return turn;
    }

    // Blank lines separate blocks. That is the contract the compose prompt states, and it
    // is what makes each line separately editable afterwards.
    std::vector<std::string> paragraphs;
    std::istringstream lines(composed.response);
    std::string line;
    std::string current;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (Trim(line).empty())
        {
            if (!Trim(current).empty())
            {
                paragraphs.push_back(Trim(current));
            }
            current.clear();
            continue;
        }
        current += current.empty() ? line : "\n" + line;
    }
    if (!Trim(current).empty())
    {
        paragraphs.push_back(Trim(current));
    }

    const bool extending = !document.IsEmpty();
    if (extending)
    {
        for (std::string& paragraph : paragraphs)
        {
            document.Append(std::move(paragraph));
        }
    }
    else
    {
        document.Compose(request, std::move(paragraphs));
    }

    trace << "\nThe model returned " << composed.response.size() << " characters in "
        << static_cast<long long>(elapsed) << "ms, split into blocks on blank lines.";
    trace << "\n" << (extending ? "Appended to" : "Composed") << " the working document; "
        << "it now holds " << document.Blocks().size() << " editable blocks.";

    turn.result.succeeded = true;
    turn.result.reasoning = trace.str();
    turn.result.text = document.RenderNumbered() +
        "\n\n/revise <n> <what to change> rewrites one line and leaves the rest exactly "
        "as it is. /undo steps back.";
    turn.Then(TurnEvent::Progress("Document", "Ready",
        std::to_string(document.Blocks().size()) + " blocks in the working document.",
        elapsed));
    turn.Then(TurnEvent::Moved(RuntimeState::Idle));
    return turn;
}

TurnOutcome DocumentWorkshop::ReviseDocumentBlock(
    const std::string& reference,
    const std::string& instruction)
{
    TurnOutcome turn;
    const content::Block* target = document.Find(reference);
    if (target == nullptr)
    {
        turn.result.succeeded = false;
        turn.result.text = document.IsEmpty()
            ? "There is no working document yet. /write starts one."
            : "There is no block " + reference + ". /scene lists them.";
        turn.result.reason = turn.result.text;
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    const std::string targetId = target->id;
    const std::string before = target->text;
    const std::string neighbourhood = document.RenderNeighbourhood(reference);

    turn.Then(TurnEvent::Moved(RuntimeState::Thinking, "Revising one line."));
    turn.Then(TurnEvent::Progress("Document", "Revising",
        "Rewriting block " + std::to_string(target->ordinal) + " only."));
    const auto started = std::chrono::steady_clock::now();
    const responseOutput revised =
        router.ReviseBlock(instruction, neighbourhood, before);
    const double elapsed = ElapsedMilliseconds(started);

    std::ostringstream trace;
    trace << "Precise edit: sent block " << target->ordinal
        << " with two lines either side for continuity, and asked for that line only. "
           "The reply can only ever be written into that one block -- the edit path has "
           "no expression for touching another.";
    if (!revised.reasoning.empty())
    {
        trace << "\n\nReasoning:\n" << revised.reasoning;
    }

    if (!revised.bSuccess)
    {
        turn.result.succeeded = false;
        turn.result.text = revised.response;
        turn.result.reason = revised.reason;
        trace << "\n\nThe model could not answer: " << revised.reason;
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Progress("Document", "Error", revised.reason));
        turn.Then(TurnEvent::Moved(RuntimeState::Error, turn.result.reason));
        return turn;
    }

    const std::string cleaned =
        content::PreciseEditGuard::CleanReplacement(revised.response);
    trace << "\nThe model returned " << revised.response.size() << " characters in "
        << static_cast<long long>(elapsed) << "ms.";

    // The one failure the block model cannot prevent by itself: a model that was asked
    // for a line and returned the scene. Storing it would collapse the document into one
    // paragraph rather than corrupt the others, but that is its own kind of broken.
    if (content::PreciseEditGuard::LooksLikeWholeDocument(
            cleaned, document.Blocks(), targetId))
    {
        turn.result.succeeded = false;
        turn.result.text = "That came back as a rewrite of the surrounding lines rather than "
            "the one line, so I left the document alone. Try naming the change more "
            "narrowly.";
        turn.result.reason = "The replacement contained neighbouring blocks verbatim.";
        trace << "\nRefused: the replacement contained other blocks verbatim, so it was "
                 "a scene rewrite wearing the shape of a line edit. Nothing was changed.";
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Progress("Document", "Refused", turn.result.reason));
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    const content::EditOutcome outcome =
        document.ReplaceBlock(reference, cleaned);
    if (!outcome.succeeded)
    {
        turn.result.succeeded = false;
        turn.result.text = outcome.message;
        turn.result.reason = outcome.message;
        trace << "\nThe edit was rejected: " << outcome.message;
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    trace << "\n" << outcome.message;
    turn.result.succeeded = true;
    turn.result.reasoning = trace.str();
    turn.result.text = "Was:  " + outcome.before + "\nNow:  " + outcome.after + "\n\n" +
        outcome.message + " /undo puts it back.";
    turn.Then(TurnEvent::Progress("Document", "Ready", outcome.message, elapsed));
    turn.Then(TurnEvent::Moved(RuntimeState::Idle));
    return turn;
}

TurnOutcome DocumentWorkshop::GenerateImage(const std::string& prompt)
{
    TurnOutcome turn;
    if (prompt.empty())
    {
        turn.result.succeeded = false;
        turn.result.text = "Usage: /imagine <what you want pictured>";
        turn.result.reason = "No image request was given.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    std::string availability;
    if (!imageGenerator.IsAvailable(availability))
    {
        turn.result.succeeded = false;
        turn.result.text = availability;
        turn.result.reason = availability;
        turn.result.reasoning = "Image generation was asked for but the local runtime is not "
            "ready. This is a separate optional model from the diagram path: /draw can "
            "still produce a diagram, which is a different thing from a picture.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    turn.Then(TurnEvent::Moved(RuntimeState::Thinking, "Generating a picture."));
    turn.Then(TurnEvent::Progress("Image", "Generating",
        "Running the local image model. The first request also loads it."));

    const visual::ImageResult generated = imageGenerator.Generate(prompt);
    std::ostringstream trace;
    trace << "Picture: sent \"" << prompt << "\" to the local image model. This is a "
             "diffusion model in an owned Python worker, not the language model.";
    if (!generated.detail.empty())
    {
        trace << "\n" << generated.detail << '.';
    }

    if (!generated.succeeded)
    {
        turn.result.succeeded = false;
        turn.result.text = generated.message;
        turn.result.reason = generated.message;
        trace << "\n\nNothing was produced: " << generated.message;
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Progress("Image", "Error", generated.message));
        turn.Then(TurnEvent::Moved(RuntimeState::Error, turn.result.reason));
        return turn;
    }

    RuntimeEvent event;
    event.kind = RuntimeEventKind::Diagram;
    event.state = RuntimeState::Responding;
    event.component = "Canvas";
    event.phase = "Image";
    event.message = prompt.size() > 60 ? revia::utf8::Prefix(prompt, 60) + "..." : prompt;
    event.resource = actions::PathToUtf8(generated.path);
    turn.Then(TurnEvent::Published(std::move(event)));

    trace << "\nSaved to " << actions::PathToUtf8(generated.path)
        << " and published to the Canvas tab. Took "
        << static_cast<long long>(generated.elapsedMilliseconds / 1000.0) << "s.";
    turn.result.succeeded = true;
    turn.result.reasoning = trace.str();
    turn.result.text = "Pictured that - it's on the Canvas tab. " + generated.message;
    turn.Then(TurnEvent::Progress("Image", "Ready", generated.detail, generated.elapsedMilliseconds));
    turn.Then(TurnEvent::Moved(RuntimeState::Idle));
    return turn;
}

TurnOutcome DocumentWorkshop::ShowPicture(
    const std::string& path, const PictureScope& scope)
{
    TurnOutcome turn;
    if (path.empty())
    {
        turn.result.succeeded = false;
        turn.result.text = "Usage: /show <path to an image>";
        turn.result.reason = "No picture was named.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    std::error_code error;
    const std::filesystem::path requested =
        std::filesystem::weakly_canonical(std::filesystem::path(path), error);
    if (error || !std::filesystem::is_regular_file(requested, error))
    {
        turn.result.succeeded = false;
        turn.result.text = "There is no file at " + path + '.';
        turn.result.reason = turn.result.text;
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    std::string extension = requested.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    const bool isPicture = extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".bmp" || extension == ".gif" ||
        extension == ".webp" || extension == ".svg";
    if (!isPicture)
    {
        turn.result.succeeded = false;
        turn.result.text = extension.empty()
            ? "That file has no extension, so I cannot tell whether it is a picture."
            : "I can show images, not " + extension + " files.";
        turn.result.reason = turn.result.text;
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    // Displaying a file is reading it, so it is bounded by the same approved roots that
    // govern reading one. Revia's own output folder is included because she wrote it.
    std::vector<std::filesystem::path> allowed = scope.approvedRoots;
    allowed.push_back(std::filesystem::weakly_canonical(diagramStore.Root(), error));
    // Must match however the capture side resolves the same setting, or an image
    // Revia just captured under the canonical root could be rejected as out of scope.
    allowed.push_back(std::filesystem::weakly_canonical(
        revia::core::ResolveRuntimeWritePath(scope.mediaPath), error));
    const bool inScope = std::any_of(allowed.begin(), allowed.end(),
        [&requested](const std::filesystem::path& root)
        {
            if (root.empty())
            {
                return false;
            }
            std::error_code rootError;
            const std::filesystem::path canonicalRoot =
                std::filesystem::weakly_canonical(root, rootError);
            if (rootError)
            {
                return false;
            }
            const std::filesystem::path relative =
                requested.lexically_relative(canonicalRoot);
            // An empty result means unrelated paths; a leading ".." means the file sits
            // outside the root and only looks like it is inside it.
            return !relative.empty() && *relative.begin() != "..";
        });
    if (!inScope)
    {
        turn.result.succeeded = false;
        turn.result.text = "That picture is outside every approved folder, so I will not open "
            "it. Add its folder as an approved root if you want me to see it.";
        turn.result.reason = "The picture is outside the approved roots.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    RuntimeEvent event;
    event.kind = RuntimeEventKind::Diagram;
    event.state = RuntimeState::Responding;
    event.component = "Canvas";
    // The phase is what tells the canvas whether to parse markup or load a file.
    event.phase = "Image";
    event.message = requested.filename().string();
    event.resource = actions::PathToUtf8(requested);
    turn.Then(TurnEvent::Published(std::move(event)));

    turn.result.succeeded = true;
    turn.result.reasoning = "Showing a picture: checked that " +
        actions::PathToUtf8(requested) +
        " exists, is an image, and sits inside an approved folder, then published it to "
        "the Canvas tab. The file is displayed from disk and is not copied or altered.";
    turn.result.text = "Put " + requested.filename().string() + " on the Canvas tab.";
    turn.Then(TurnEvent::Moved(RuntimeState::Idle));
    return turn;
}

TurnOutcome DocumentWorkshop::DrawDiagram(const std::string& request)
{
    TurnOutcome turn;
    if (request.empty())
    {
        turn.result.succeeded = false;
        turn.result.text = "Usage: /draw <what you want drawn>";
        turn.result.reason = "No drawing request was given.";
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    turn.Then(TurnEvent::Moved(RuntimeState::Thinking, "Drawing a diagram."));
    turn.Then(TurnEvent::Progress("Canvas", "Drawing", "Generating an SVG diagram."));
    const auto drawingStarted = std::chrono::steady_clock::now();
    const responseOutput drawn = router.DrawDiagram(request);
    const double drawingMilliseconds = ElapsedMilliseconds(drawingStarted);

    // The Thought process is where "what is she actually doing" gets answered, so the
    // drawing path narrates itself the same way a conversation turn does.
    std::ostringstream trace;
    trace << "Drawing: asked the local model for an SVG of \"" << request << "\".";
    if (!drawn.reasoning.empty())
    {
        trace << "\n\nReasoning:\n" << drawn.reasoning;
    }
    if (!drawn.bSuccess)
    {
        turn.result.succeeded = false;
        turn.result.text = drawn.response;
        turn.result.reason = drawn.reason;
        trace << "\n\nThe model could not answer: " << drawn.reason;
        turn.result.reasoning = trace.str();
        turn.Then(TurnEvent::Progress("Canvas", "Error", drawn.reason));
        turn.Then(TurnEvent::Moved(RuntimeState::Error, turn.result.reason));
        return turn;
    }
    trace << "\nThe model returned " << drawn.response.size() << " characters in "
        << static_cast<long long>(drawingMilliseconds) << "ms.";

    std::string title = request.size() > 60 ? revia::utf8::Prefix(request, 60) : request;
    std::string markup = drawn.response;
    try
    {
        const nlohmann::json document = nlohmann::json::parse(drawn.response);
        if (document.is_object())
        {
            title = document.value("title", title);
            markup = document.value("svg", markup);
        }
    }
    catch (const std::exception&)
    {
        // The structured request is a request, not a guarantee. A model that answered
        // with bare SVG still produced something drawable, so the extractor gets a turn
        // before this is called a failure.
    }

    const visual::SvgValidation validation = visual::SvgSanitizer::Sanitize(markup);
    trace << "\nSafety check: " << validation.reason;
    if (!validation.accepted)
    {
        turn.result.succeeded = false;
        turn.result.text = "That drawing was refused. " + validation.reason;
        turn.result.reason = validation.reason;
        turn.result.reasoning = trace.str();
        log.Warning("Diagram refused: " + validation.reason);
        turn.Then(TurnEvent::Progress("Canvas", "Refused", validation.reason));
        turn.Then(TurnEvent::Moved(RuntimeState::Blocked, turn.result.reason));
        return turn;
    }

    visual::Diagram diagram;
    std::string saveError;
    if (!diagramStore.Save(title, validation.markup, diagram, saveError))
    {
        turn.result.succeeded = false;
        turn.result.text = saveError;
        turn.result.reason = saveError;
        turn.Then(TurnEvent::Progress("Canvas", "Error", saveError));
        turn.Then(TurnEvent::Moved(RuntimeState::Error, turn.result.reason));
        return turn;
    }

    RuntimeEvent event;
    event.kind = RuntimeEventKind::Diagram;
    event.state = RuntimeState::Responding;
    event.component = "Canvas";
    event.phase = "Ready";
    event.message = diagram.title;
    event.detail = validation.markup;
    event.resource = actions::PathToUtf8(diagram.path);
    turn.Then(TurnEvent::Published(std::move(event)));

    trace << "\nSaved to " << actions::PathToUtf8(diagram.path)
        << " and published to the Canvas tab.";
    turn.result.succeeded = true;
    turn.result.reasoning = trace.str();
    turn.result.text = "Drew \"" + diagram.title + "\" - it's on the Canvas tab.";
    turn.Then(TurnEvent::Moved(RuntimeState::Idle));
    return turn;
}

} // namespace revia::runtime

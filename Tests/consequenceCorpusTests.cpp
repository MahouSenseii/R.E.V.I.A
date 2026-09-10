#include "testSupport.h"

#include "Policy/desktopAuthorization.h"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace revia::policy;
using revia::tests::Check;

// A measured corpus rather than a claim.
//
// "The classifier is conservative" is the kind of statement that is easy to make and
// hard to check, so this is a hundred-odd labelled controls with an expected answer
// each. Two numbers come out: how often a dangerous control was read as ordinary, and
// how often an ordinary one was read as dangerous.
//
// Those two are not symmetric. A dangerous control read as ordinary is a thing that
// happens without being asked about, so the target for that is zero. An ordinary control
// read as dangerous costs a question and nothing else, so it is measured and reported
// rather than driven down -- driving it down means loosening the other one.

// What the case is expected to do. Deliberately coarse: the corpus is about "did the
// dangerous ones read as dangerous", not about exact bit patterns.
enum class Expectation
{
    // Nothing beyond ordinary interaction.
    Ordinary,
    // Changes the user's own content, and nothing further.
    Content,
    // At least one of: external, financial, destructive, credentials, commands.
    Consequential,
    // No usable label. Must not read as Ordinary for a committing operation.
    Unknown
};

struct Case
{
    const char* label;
    const char* window;
    Expectation expected;
};

// Grouped by what they are, so a gap in coverage is visible as a short section rather
// than hidden in a flat list.
const std::vector<Case>& Corpus()
{
    static const std::vector<Case> cases = {
        // --- navigation and ordinary interaction -------------------------------
        {"Zoom in", "Viewer", Expectation::Ordinary},
        {"Zoom out", "Viewer", Expectation::Ordinary},
        {"Next page", "Viewer", Expectation::Ordinary},
        {"Previous page", "Viewer", Expectation::Ordinary},
        {"Back", "Browser", Expectation::Ordinary},
        {"Forward", "Browser", Expectation::Ordinary},
        {"Reload", "Browser", Expectation::Ordinary},
        {"Search", "Browser", Expectation::Ordinary},
        {"Filter", "Table", Expectation::Ordinary},
        {"Sort ascending", "Table", Expectation::Ordinary},
        {"Expand row", "Table", Expectation::Ordinary},
        {"Collapse all", "Tree", Expectation::Ordinary},
        {"Show details", "Panel", Expectation::Ordinary},
        {"Toggle sidebar", "Editor", Expectation::Ordinary},
        {"Full screen", "Player", Expectation::Ordinary},
        {"Play", "Player", Expectation::Ordinary},
        {"Pause", "Player", Expectation::Ordinary},
        {"Volume", "Player", Expectation::Ordinary},
        {"Select all", "Editor", Expectation::Ordinary},
        {"Find in page", "Browser", Expectation::Ordinary},
        {"Go to line", "Editor", Expectation::Ordinary},
        {"Word wrap", "Editor", Expectation::Ordinary},
        {"Line numbers", "Editor", Expectation::Ordinary},
        {"Split view", "Editor", Expectation::Ordinary},
        {"New tab", "Browser", Expectation::Ordinary},
        {"Bookmarks", "Browser", Expectation::Ordinary},
        {"History", "Browser", Expectation::Ordinary},
        {"Downloads", "Browser", Expectation::Ordinary},
        {"Help", "Application", Expectation::Ordinary},
        {"About", "Application", Expectation::Ordinary},
        {"Minimise", "Window", Expectation::Ordinary},
        {"Run tests", "IDE", Expectation::Ordinary},
        {"Build", "IDE", Expectation::Ordinary},
        {"Step over", "Debugger", Expectation::Ordinary},
        {"Breakpoint", "Debugger", Expectation::Ordinary},

        // --- editing and saving -------------------------------------------------
        {"Save", "Document", Expectation::Content},
        {"Save as", "Document", Expectation::Content},
        {"Rename", "Explorer", Expectation::Content},
        {"Apply", "Preferences", Expectation::Content},
        {"Commit", "Version control", Expectation::Content},
        {"Export", "Report", Expectation::Content},
        {"Import", "Report", Expectation::Content},
        {"Merge", "Version control", Expectation::Content},
        {"Overwrite", "Save dialog", Expectation::Content},
        {"Replace", "Find and replace", Expectation::Content},
        {"Move", "Explorer", Expectation::Content},

        // --- external communication --------------------------------------------
        {"Send", "Compose", Expectation::Consequential},
        {"Send message", "Chat", Expectation::Consequential},
        {"Reply", "Mail", Expectation::Consequential},
        {"Reply all", "Mail", Expectation::Consequential},
        {"Post", "Social", Expectation::Consequential},
        {"Publish", "Blog", Expectation::Consequential},
        {"Share", "Document", Expectation::Consequential},
        {"Tweet", "Social", Expectation::Consequential},
        {"Upload", "Storage", Expectation::Consequential},
        {"Submit", "Form", Expectation::Consequential},
        {"Invite", "Calendar", Expectation::Consequential},
        {"Go live", "Streaming", Expectation::Consequential},
        {"Broadcast", "Streaming", Expectation::Consequential},
        {"Email report", "Report", Expectation::Consequential},

        // --- financial -----------------------------------------------------------
        {"Buy now", "Store", Expectation::Consequential},
        {"Purchase", "Store", Expectation::Consequential},
        {"Place order", "Checkout", Expectation::Consequential},
        {"Confirm order", "Checkout", Expectation::Consequential},
        {"Pay", "Checkout", Expectation::Consequential},
        {"Payment method", "Checkout", Expectation::Consequential},
        {"Checkout", "Cart", Expectation::Consequential},
        {"Subscribe", "Plans", Expectation::Consequential},
        {"Donate", "Charity", Expectation::Consequential},
        {"Send money", "Banking", Expectation::Consequential},
        {"Transfer", "Banking", Expectation::Consequential},

        // --- destructive ---------------------------------------------------------
        {"Delete", "Explorer", Expectation::Consequential},
        {"Delete all", "Explorer", Expectation::Consequential},
        {"Remove", "List", Expectation::Consequential},
        {"Erase", "Disk tool", Expectation::Consequential},
        {"Format", "Disk tool", Expectation::Consequential},
        {"Wipe", "Disk tool", Expectation::Consequential},
        {"Empty trash", "Explorer", Expectation::Consequential},
        {"Empty recycle bin", "Explorer", Expectation::Consequential},
        {"Delete permanently", "Explorer", Expectation::Consequential},
        {"Uninstall", "Programs", Expectation::Consequential},
        {"Destroy", "Admin tool", Expectation::Consequential},

        // --- account and security ------------------------------------------------
        {"Delete account", "Settings", Expectation::Consequential},
        {"Deactivate account", "Settings", Expectation::Consequential},
        {"Close account", "Settings", Expectation::Consequential},
        {"Change email", "Settings", Expectation::Consequential},
        {"Change password", "Settings", Expectation::Consequential},
        {"Sign out", "Account", Expectation::Consequential},
        {"Log in", "Account", Expectation::Consequential},
        {"API key", "Developer", Expectation::Consequential},
        {"Access token", "Developer", Expectation::Consequential},
        {"Private key", "Security", Expectation::Consequential},
        {"Grant access", "Sharing", Expectation::Consequential},
        {"Authorize", "OAuth", Expectation::Consequential},
        {"Permissions", "Settings", Expectation::Consequential},
        {"Two-factor", "Security", Expectation::Consequential},
        {"Privacy settings", "Settings", Expectation::Consequential},

        // --- command surfaces ----------------------------------------------------
        {"Command prompt", "Tools", Expectation::Consequential},
        {"PowerShell", "Tools", Expectation::Consequential},
        {"Terminal", "Tools", Expectation::Consequential},
        {"Run command", "Tools", Expectation::Consequential},
        {"Developer console", "Browser", Expectation::Consequential},

        // --- context carries the meaning ------------------------------------------
        // Same word, different window. If context were ignored these would all read the
        // same, which is exactly the failure the context read exists to prevent.
        {"Confirm", "Delete account", Expectation::Consequential},
        {"Confirm", "Confirm purchase", Expectation::Consequential},
        {"Confirm", "Delete 40 files", Expectation::Consequential},
        {"Confirm", "Send to 200 recipients", Expectation::Consequential},
        {"OK", "Erase disk", Expectation::Consequential},
        {"Yes", "Uninstall application", Expectation::Consequential},

        // --- ambiguous, with nothing dangerous in context -------------------------
        // These are documented as ambiguous rather than ordinary: the label names the
        // gesture, so the evidence is Ambiguous even where no effect is detected.
        {"Confirm", "Preferences", Expectation::Unknown},
        {"OK", "Preferences", Expectation::Unknown},
        {"Yes", "Reopen last file?", Expectation::Unknown},
        {"Continue", "Setup", Expectation::Unknown},
        {"Proceed", "Wizard", Expectation::Unknown},
        {"Accept", "Preferences", Expectation::Unknown},
        {"Next", "Wizard", Expectation::Unknown},
        {"Finish", "Wizard", Expectation::Unknown},
        {"Done", "Editor", Expectation::Unknown},

        // --- missing evidence -----------------------------------------------------
        {"", "Dialog", Expectation::Unknown},
        {"", "", Expectation::Unknown},
    };
    return cases;
}

const DesktopEffects ConsequentialBits =
    static_cast<DesktopEffects>(DesktopEffect::ExternalMessage) |
    DesktopEffect::Financial | DesktopEffect::Destructive |
    DesktopEffect::AccountOrSecurity | DesktopEffect::CommandSurface;

AuthorizationRequest Ask(const Case& item)
{
    AuthorizationRequest request;
    request.operation = DesktopOperation::PointerActivate;
    request.evidence.resolved = item.label[0] != '\0';
    request.evidence.controlName = item.label;
    request.evidence.windowTitle = item.window;
    request.evidence.executable = "fixture.exe";
    request.autonomousOrigin = true;
    return request;
}

} // namespace

void RunConsequenceCorpusTests()
{
    revia::actions::CapabilitySettings::DesktopControl settings;
    settings.pointer = true;
    settings.keyboard = true;
    // The shipped default. Anything above ordinary has to be asked about.
    settings.maxUnconfirmedConsequence = revia::actions::ConsequenceClass::Routine;

    std::vector<std::string> criticalFalseNegatives;
    std::vector<std::string> falsePositives;
    std::vector<std::string> ambiguousDocumented;
    std::size_t consequentialCases = 0;
    std::size_t ordinaryCases = 0;

    for (const Case& item : Corpus())
    {
        const AuthorizationRequest request = Ask(item);
        const AuthorizationDecision decision = AuthorizeDesktopEffect(request, settings);
        const bool readAsConsequential = (decision.effects & ConsequentialBits) != 0u;
        const bool allowed = decision.verdict == AuthorizationVerdict::Allow;
        const std::string name =
            std::string("\"") + item.label + "\" in \"" + item.window + "\"";

        switch (item.expected)
        {
            case Expectation::Consequential:
                ++consequentialCases;
                // The one that must never happen: something that sends, spends,
                // destroys, or touches credentials proceeding without being asked about.
                if (allowed || !readAsConsequential)
                {
                    criticalFalseNegatives.push_back(name);
                }
                break;

            case Expectation::Unknown:
                ++ordinaryCases;
                // Not required to be refused outright, but must not be waved through as
                // though the absence of a scary word were evidence of safety.
                if (allowed)
                {
                    criticalFalseNegatives.push_back(name + " (unknown treated as safe)");
                }
                ambiguousDocumented.push_back(name);
                break;

            case Expectation::Ordinary:
            case Expectation::Content:
                ++ordinaryCases;
                if (readAsConsequential)
                {
                    falsePositives.push_back(name);
                }
                break;
        }
    }

    const std::size_t total = Corpus().size();
    std::cout << "\nConsequence corpus: " << total << " cases ("
              << consequentialCases << " expected consequential, " << ordinaryCases
              << " expected ordinary or unknown)\n";
    std::cout << "  critical false negatives: " << criticalFalseNegatives.size()
              << "  (target 0)\n";
    std::cout << "  false positives:          " << falsePositives.size()
              << "  (measured, not minimised)\n";
    std::cout << "  documented ambiguous:     " << ambiguousDocumented.size() << "\n";
    for (const std::string& entry : falsePositives)
    {
        std::cout << "    false positive: " << entry << "\n";
    }
    for (const std::string& entry : criticalFalseNegatives)
    {
        std::cout << "    FALSE NEGATIVE: " << entry << "\n";
    }

    Check(total >= 100,
        "The corpus is smaller than the hundred cases it is supposed to cover.");
    Check(criticalFalseNegatives.empty(),
        "A consequential control was classified as ordinary permitted behaviour.");
    std::cout << "Consequence corpus passed: no consequential case read as ordinary.\n";
}

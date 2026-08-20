#pragma once

#include "Runtime/reviaSession.h"

// Thin terminal shell. ReviaSession is the only runtime owner used by both desktop and
// CLI, so behavior, workers, cancellation, memory, and shutdown cannot drift between two
// separate application implementations.
class reviaApp
{
public:
    void Run();

private:
    bool ConfirmAction(
        const revia::actions::ActionRequest& request,
        const revia::actions::PolicyDecision& decision) const;

    revia::runtime::ReviaSession session;
};

#pragma once

struct visionSettings
{
    bool bEnabled = true;
    bool bRequireConfirmation = true;
    int maxResponseTokens = 768;
    // Event-driven continuous awareness is separate from action authority. It may keep a
    // short local description of the virtual desktop, but it can never click or type.
    bool bContinuousAwareness = false;
    int awarenessDebounceMs = 1500;
    int awarenessMinimumIntervalMs = 6000;
    // Refresh when apps repaint without focus/title events, preventing stale screen context.
    int awarenessRefreshSeconds = 30;
    int awarenessMaxResponseTokens = 160;
    double resolutionConfidence = 0.72;
    double minimumNameAgreement = 0.35;
    double ambiguityMargin = 0.08;
    int maxResolverElements = 500;
};

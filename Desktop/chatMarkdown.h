#pragma once

#include <QString>

namespace revia::desktop
{
// Renders a reply as an HTML fragment for the transcript. Reply text is untrusted, so it
// is parsed as GitHub Markdown with raw HTML disabled: tags show as the text they are,
// links lose their target and show their address instead (only the window's own links
// may act), images become their address, and code is set in a monospace face.
[[nodiscard]] QString RenderUntrustedMarkdown(const QString& text);
}

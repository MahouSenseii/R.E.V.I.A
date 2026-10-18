#pragma once

// The local OpenAI-compatible API other programs on this PC can talk to Revia through.
// Off by default; loopback only; every request needs the install's bearer token.
struct localApiSettings
{
    bool bEnabled = false;
    int port = 17865;
    // Requests are a shared audience by default: no private memory, no screen or
    // clipboard, no tools, no identity or relationship writes. True makes each request
    // an owner turn instead, with everything that implies. Any program holding the
    // token then speaks as the owner.
    bool bTrustedPrivate = false;
};

#!/usr/bin/env python3
"""A fake Agent Client Protocol agent for Revia's tests.

Speaks JSON-RPC 2.0, one message per line, over stdin and stdout the way an editor-hosted
coding agent does, and does what the prompt says so the client's side of every exchange
can be exercised without a real agent: streams message chunks, asks permission for a
tool call, reads and writes files through the client, asks for a terminal, refuses, and
honours session/cancel.
"""
import json
import sys
import time

_next_id = 100
_pending = {}


def send(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def notify(method, params):
    send({"jsonrpc": "2.0", "method": method, "params": params})


def read_message():
    line = sys.stdin.readline()
    if not line:
        return None
    line = line.strip()
    if not line:
        return {}
    return json.loads(line)


def request(method, params):
    """Sends a request to the client and waits for its response, handling nothing else
    but a cancel notification meanwhile (recorded, not acted on here)."""
    global _next_id
    rid = _next_id
    _next_id += 1
    send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    while True:
        message = read_message()
        if message is None:
            sys.exit(0)
        if message.get("id") == rid and "method" not in message:
            return message
        if message.get("method") == "session/cancel":
            _pending["cancelled"] = True


def update(session_id, body):
    notify("session/update", {"sessionId": session_id, "update": body})


def chunk(session_id, text):
    update(session_id, {"sessionUpdate": "agent_message_chunk",
                        "content": {"type": "text", "text": text}})


def handle_prompt(rid, params):
    session_id = params["sessionId"]
    text = " ".join(block.get("text", "") for block in params.get("prompt", [])
                    if block.get("type") == "text")
    _pending["cancelled"] = False
    update(session_id, {"sessionUpdate": "agent_thought_chunk",
                        "content": {"type": "text", "text": "thinking about: " + text}})
    if text.startswith("say hello"):
        chunk(session_id, "Hello from ")
        chunk(session_id, "the fake agent.")
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
        return
    if text.startswith("sleep"):
        # Streams slowly until cancelled, then acknowledges the cancellation.
        for step in range(50):
            if _pending.get("cancelled"):
                send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "cancelled"}})
                return
            chunk(session_id, "tick %d " % step)
            # Poll stdin for a cancel without blocking the stream.
            import select
            ready, _, _ = select.select([sys.stdin], [], [], 0.1)
            if ready:
                message = read_message()
                if message is None:
                    sys.exit(0)
                if message.get("method") == "session/cancel":
                    _pending["cancelled"] = True
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
        return
    if text.startswith("write "):
        # "write <absolute path> <content words...>": asks permission, then writes through
        # the client, then reads the file back through the client.
        parts = text.split(" ", 2)
        path = parts[1]
        content = parts[2] if len(parts) > 2 else ""
        update(session_id, {"sessionUpdate": "plan", "entries": [
            {"content": "Ask before writing", "priority": "high", "status": "pending"},
            {"content": "Write the file", "priority": "medium", "status": "pending"}]})
        update(session_id, {"sessionUpdate": "tool_call", "toolCallId": "call_1",
                            "title": "Write " + path, "kind": "edit", "status": "pending",
                            "rawInput": {"path": path}})
        answer = request("session/request_permission", {
            "sessionId": session_id,
            "toolCall": {"toolCallId": "call_1", "title": "Write " + path, "kind": "edit",
                         "rawInput": {"path": path, "content": content}},
            "options": [
                {"optionId": "allow-once", "name": "Allow once", "kind": "allow_once"},
                {"optionId": "allow-always", "name": "Allow always", "kind": "allow_always"},
                {"optionId": "reject-once", "name": "Reject", "kind": "reject_once"}]})
        outcome = answer.get("result", {}).get("outcome", {})
        if outcome.get("outcome") != "selected" or not outcome.get("optionId", "").startswith("allow"):
            update(session_id, {"sessionUpdate": "tool_call_update", "toolCallId": "call_1",
                                "status": "failed"})
            chunk(session_id, "You declined, so nothing was written.")
            send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
            return
        written = request("fs/write_text_file", {"sessionId": session_id, "path": path,
                                                 "content": content})
        if "error" in written:
            update(session_id, {"sessionUpdate": "tool_call_update", "toolCallId": "call_1",
                                "status": "failed"})
            chunk(session_id, "The write was refused: " + written["error"].get("message", ""))
            send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
            return
        update(session_id, {"sessionUpdate": "tool_call_update", "toolCallId": "call_1",
                            "status": "completed"})
        read = request("fs/read_text_file", {"sessionId": session_id, "path": path,
                                             "line": 1, "limit": 1})
        first_line = read.get("result", {}).get("content", "")
        chunk(session_id, "Wrote it. First line reads: " + first_line.strip())
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
        return
    if text.startswith("read "):
        path = text.split(" ", 1)[1]
        read = request("fs/read_text_file", {"sessionId": session_id, "path": path})
        if "error" in read:
            chunk(session_id, "The read was refused: " + read["error"].get("message", ""))
        else:
            chunk(session_id, "Read: " + read["result"].get("content", ""))
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
        return
    if text.startswith("terminal"):
        answer = request("terminal/create", {"sessionId": session_id, "command": "ls", "args": []})
        if "error" in answer:
            chunk(session_id, "No terminal: " + answer["error"].get("message", ""))
        else:
            chunk(session_id, "Got a terminal, which the client should not have given.")
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})
        return
    if text.startswith("refuse"):
        send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "refusal"}})
        return
    chunk(session_id, "I do not know that task.")
    send({"jsonrpc": "2.0", "id": rid, "result": {"stopReason": "end_turn"}})


def main():
    # A banner on stdout, which a real client must survive.
    print("fake acp agent starting")
    sys.stdout.flush()
    while True:
        message = read_message()
        if message is None:
            return
        if not message:
            continue
        method = message.get("method")
        rid = message.get("id")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": 1,
                "agentCapabilities": {"loadSession": False,
                                      "promptCapabilities": {"image": False, "audio": False,
                                                             "embeddedContext": True}},
                "agentInfo": {"name": "fake-acp", "title": "Fake ACP agent", "version": "0.1"},
                "authMethods": []}})
        elif method == "session/new":
            _pending["cwd"] = message.get("params", {}).get("cwd", "")
            send({"jsonrpc": "2.0", "id": rid, "result": {"sessionId": "sess_fake_1"}})
        elif method == "session/prompt":
            handle_prompt(rid, message.get("params", {}))
        elif method == "session/cancel":
            _pending["cancelled"] = True
        elif rid is not None:
            send({"jsonrpc": "2.0", "id": rid,
                  "error": {"code": -32601, "message": "unknown method " + str(method)}})


if __name__ == "__main__":
    main()

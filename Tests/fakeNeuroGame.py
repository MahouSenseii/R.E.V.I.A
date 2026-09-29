#!/usr/bin/env python3
"""A fake game speaking the Neuro SDK protocol, for Revia's tests.

A raw-socket WebSocket client (no third-party package), so the server's handshake and
framing are exercised by an independent implementation. Connects to 127.0.0.1:<port>,
registers two actions, gives context, forces a choice, reports the action it got, then
does it again with one action allowed, and prints one line per step to stdout.
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

GAME = "Fake Card Game"


def send_frame(sock, text):
    payload = text.encode("utf-8")
    header = bytearray([0x81])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    key = os.urandom(4)
    masked = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(header) + key + masked)


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


def recv_frame(sock):
    first, second = recv_exact(sock, 2)
    opcode = first & 0x0F
    masked = second & 0x80
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    key = recv_exact(sock, 4) if masked else None
    payload = recv_exact(sock, length)
    if key:
        payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def recv_message(sock, timeout=10.0):
    sock.settimeout(timeout)
    while True:
        opcode, payload = recv_frame(sock)
        if opcode == 0x1:
            return json.loads(payload.decode("utf-8"))
        if opcode == 0x8:
            raise ConnectionError("closed by server")
        if opcode == 0x9:
            # Pong back a ping, masked as a client must.
            send_frame(sock, payload.decode("utf-8", "replace"))


def say(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def command(sock, name, data=None):
    message = {"command": name, "game": GAME}
    if data is not None:
        message["data"] = data
    send_frame(sock, json.dumps(message))


def wait_for_action(sock):
    while True:
        message = recv_message(sock)
        if message.get("command") == "action":
            return message["data"]
        say("OTHER " + message.get("command", "?"))


def main():
    port = int(sys.argv[1])
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    request = ("GET /neuro HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % (port, key))
    sock.sendall(request.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
    head = response.split(b"\r\n\r\n", 1)[0].decode()
    expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    if "101" not in head.split("\r\n")[0] or ("Sec-WebSocket-Accept: " + expected) not in head:
        say("HANDSHAKE FAILED " + head.replace("\r\n", " | "))
        return
    say("HANDSHAKE OK")

    command(sock, "startup")
    command(sock, "actions/register", {"actions": [
        {"name": "play_card", "description": "Play a card from your hand.",
         "schema": {"type": "object", "properties": {"card": {"type": "string", "enum": ["queen", "king"]}},
                    "required": ["card"], "additionalProperties": False}},
        {"name": "fold", "description": "Give up this round."},
        {"name": "Bad Name!", "description": "Should be refused by the server."}]})
    command(sock, "context", {"message": "It is your turn. You hold a queen and a king.", "silent": False})
    command(sock, "actions/force", {"state": "Round 1", "query": "Play a card or fold.",
                                    "ephemeral_context": False, "priority": "high",
                                    "action_names": ["play_card", "fold", "not_registered"]})
    action = wait_for_action(sock)
    data = json.loads(action.get("data") or "{}")
    say("ACTION %s %s" % (action["name"], json.dumps(data, sort_keys=True)))
    ok = (action["name"] == "play_card" and data.get("card") in ("queen", "king")) or action["name"] == "fold"
    command(sock, "action/result", {"id": action["id"], "success": ok,
                                    "message": "played" if ok else "bad data"})

    command(sock, "context", {"message": "Round 2. Only folding is allowed now.", "silent": False})
    command(sock, "actions/force", {"query": "Fold.", "priority": "medium", "action_names": ["fold"]})
    action = wait_for_action(sock)
    say("ACTION %s %s" % (action["name"], action.get("data") or "{}"))
    command(sock, "action/result", {"id": action["id"], "success": True, "message": "folded"})

    # A silent note, then wait briefly for anything else (speech_finished, for one).
    command(sock, "context", {"message": "Scores updated.", "silent": True})
    deadline = time.time() + 2.0
    while time.time() < deadline:
        try:
            message = recv_message(sock, timeout=0.5)
            say("OTHER " + message.get("command", "?"))
        except socket.timeout:
            continue
        except ConnectionError:
            break
    say("DONE")
    try:
        sock.close()
    except OSError:
        pass


if __name__ == "__main__":
    main()

#include "Net/webSocket.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace revia::net
{

namespace
{
constexpr const char* WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t MaximumMessage = 1024 * 1024;

std::uint32_t RotateLeft(const std::uint32_t value, const unsigned bits)
{
    return (value << bits) | (value >> (32 - bits));
}

void CloseSocket(const SocketHandle socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool SendAll(const SocketHandle socket, const std::string& bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
#ifdef _WIN32
        const int count = send(socket, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
        if (count == SOCKET_ERROR) return false;
#else
        const ssize_t count = send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void EnsureSocketsStarted()
{
#ifdef _WIN32
    static const bool started = []
    {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

std::string Lower(std::string text)
{
    for (char& character : text)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

std::string Trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
} // namespace

std::string Sha1(const std::string& bytes)
{
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string message = bytes;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        message.push_back(static_cast<char>((bitLength >> shift) & 0xFF));
    }
    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64)
    {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            const auto* at = reinterpret_cast<const unsigned char*>(message.data() + chunk + index * 4);
            words[index] = (static_cast<std::uint32_t>(at[0]) << 24) | (static_cast<std::uint32_t>(at[1]) << 16) |
                (static_cast<std::uint32_t>(at[2]) << 8) | static_cast<std::uint32_t>(at[3]);
        }
        for (std::size_t index = 16; index < 80; ++index)
        {
            words[index] = RotateLeft(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        }
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (std::size_t index = 0; index < 80; ++index)
        {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (index < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (index < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (index < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t temporary = RotateLeft(a, 5) + f + e + k + words[index];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temporary;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::string digest;
    for (const std::uint32_t word : {h0, h1, h2, h3, h4})
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            digest.push_back(static_cast<char>((word >> shift) & 0xFF));
        }
    }
    return digest;
}

std::string Base64Encode(const std::string& bytes)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0;
    while (index + 2 < bytes.size())
    {
        const std::uint32_t triple = (static_cast<unsigned char>(bytes[index]) << 16) |
            (static_cast<unsigned char>(bytes[index + 1]) << 8) | static_cast<unsigned char>(bytes[index + 2]);
        out.push_back(alphabet[(triple >> 18) & 63]);
        out.push_back(alphabet[(triple >> 12) & 63]);
        out.push_back(alphabet[(triple >> 6) & 63]);
        out.push_back(alphabet[triple & 63]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1)
    {
        const std::uint32_t triple = static_cast<unsigned char>(bytes[index]) << 16;
        out.push_back(alphabet[(triple >> 18) & 63]);
        out.push_back(alphabet[(triple >> 12) & 63]);
        out += "==";
    }
    else if (remaining == 2)
    {
        const std::uint32_t triple = (static_cast<unsigned char>(bytes[index]) << 16) |
            (static_cast<unsigned char>(bytes[index + 1]) << 8);
        out.push_back(alphabet[(triple >> 18) & 63]);
        out.push_back(alphabet[(triple >> 12) & 63]);
        out.push_back(alphabet[(triple >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string WebSocketAcceptKey(const std::string& clientKey)
{
    return Base64Encode(Sha1(clientKey + WebSocketGuid));
}

std::string EncodeFrame(const Opcode opcode, const std::string& payload, const bool mask, const bool fin)
{
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode)));
    const std::uint8_t maskBit = mask ? 0x80 : 0x00;
    if (payload.size() < 126)
    {
        frame.push_back(static_cast<char>(maskBit | static_cast<std::uint8_t>(payload.size())));
    }
    else if (payload.size() <= 0xFFFF)
    {
        frame.push_back(static_cast<char>(maskBit | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    else
    {
        frame.push_back(static_cast<char>(maskBit | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFF));
        }
    }
    if (!mask)
    {
        frame += payload;
        return frame;
    }
    // A fixed key is not a weakness here: masking exists to defeat proxy cache
    // poisoning, not to hide anything, and this end only ever talks to loopback.
    const std::array<unsigned char, 4> key = {0x37, 0xFA, 0x21, 0x3D};
    frame.append(reinterpret_cast<const char*>(key.data()), key.size());
    for (std::size_t index = 0; index < payload.size(); ++index)
    {
        frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ key[index % 4]));
    }
    return frame;
}

DecodeResult DecodeFrame(std::string& buffer, Frame& outFrame, std::string& outError, const std::size_t maximumPayload)
{
    if (buffer.size() < 2) return DecodeResult::Incomplete;
    const auto* bytes = reinterpret_cast<const unsigned char*>(buffer.data());
    const bool fin = (bytes[0] & 0x80) != 0;
    if ((bytes[0] & 0x70) != 0)
    {
        outError = "a reserved bit was set";
        return DecodeResult::Error;
    }
    const std::uint8_t opcode = bytes[0] & 0x0F;
    const bool masked = (bytes[1] & 0x80) != 0;
    std::uint64_t length = bytes[1] & 0x7F;
    std::size_t offset = 2;
    if (length == 126)
    {
        if (buffer.size() < 4) return DecodeResult::Incomplete;
        length = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
        offset = 4;
    }
    else if (length == 127)
    {
        if (buffer.size() < 10) return DecodeResult::Incomplete;
        length = 0;
        for (std::size_t index = 2; index < 10; ++index) length = (length << 8) | bytes[index];
        offset = 10;
    }
    if (length > maximumPayload)
    {
        outError = "the frame is larger than allowed";
        return DecodeResult::Error;
    }
    std::array<unsigned char, 4> key{};
    if (masked)
    {
        if (buffer.size() < offset + 4) return DecodeResult::Incomplete;
        std::copy_n(bytes + offset, 4, key.begin());
        offset += 4;
    }
    if (buffer.size() < offset + length) return DecodeResult::Incomplete;
    outFrame.fin = fin;
    outFrame.opcode = static_cast<Opcode>(opcode);
    outFrame.payload.assign(buffer.data() + offset, static_cast<std::size_t>(length));
    if (masked)
    {
        for (std::size_t index = 0; index < outFrame.payload.size(); ++index)
        {
            outFrame.payload[index] = static_cast<char>(
                static_cast<unsigned char>(outFrame.payload[index]) ^ key[index % 4]);
        }
    }
    buffer.erase(0, offset + static_cast<std::size_t>(length));
    switch (outFrame.opcode)
    {
        case Opcode::Continuation:
        case Opcode::Text:
        case Opcode::Binary:
        case Opcode::Close:
        case Opcode::Ping:
        case Opcode::Pong:
            return DecodeResult::Frame;
    }
    outError = "an unknown opcode";
    return DecodeResult::Error;
}

bool ParseUpgradeRequest(const std::string& request, std::string& outKey, std::string& outPath, std::string& outError)
{
    std::istringstream stream(request);
    std::string line;
    if (!std::getline(stream, line))
    {
        outError = "empty request";
        return false;
    }
    std::istringstream requestLine(Trim(line));
    std::string method;
    std::string path;
    std::string version;
    requestLine >> method >> path >> version;
    if (method != "GET" || version.rfind("HTTP/1.1", 0) != 0)
    {
        outError = "not an HTTP/1.1 GET";
        return false;
    }
    outPath = path;
    bool upgrade = false;
    bool connectionUpgrade = false;
    bool version13 = false;
    outKey.clear();
    while (std::getline(stream, line))
    {
        line = Trim(line);
        if (line.empty()) break;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = Lower(Trim(line.substr(0, colon)));
        const std::string value = Trim(line.substr(colon + 1));
        if (name == "upgrade" && Lower(value) == "websocket") upgrade = true;
        else if (name == "connection" && Lower(value).find("upgrade") != std::string::npos) connectionUpgrade = true;
        else if (name == "sec-websocket-version" && value == "13") version13 = true;
        else if (name == "sec-websocket-key") outKey = value;
    }
    if (!upgrade || !connectionUpgrade)
    {
        outError = "not a WebSocket upgrade";
        return false;
    }
    if (!version13)
    {
        outError = "unsupported WebSocket version";
        return false;
    }
    if (outKey.empty())
    {
        outError = "no Sec-WebSocket-Key";
        return false;
    }
    return true;
}

std::string UpgradeResponse(const std::string& clientKey)
{
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + WebSocketAcceptKey(clientKey) + "\r\n\r\n";
}

struct WebSocketServer::Connection
{
    ConnectionId id = 0;
    SocketHandle socket = InvalidSocket;
    std::thread worker;
    std::mutex sendMutex;
    std::atomic<bool> open = false;
};

WebSocketServer::~WebSocketServer()
{
    Stop();
}

void WebSocketServer::SetHandlers(MessageHandler inputOnMessage, ConnectionHandler inputOnConnection)
{
    onMessage = std::move(inputOnMessage);
    onConnection = std::move(inputOnConnection);
}

bool WebSocketServer::Start(const std::string& host, const std::uint16_t port, std::string& outError)
{
    outError.clear();
    if (running.load())
    {
        outError = "The server is already running.";
        return false;
    }
    EnsureSocketsStarted();
    const SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == InvalidSocket)
    {
        outError = "A listening socket could not be created.";
        return false;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
    {
        CloseSocket(sock);
        outError = "The host must be an IPv4 address such as 127.0.0.1.";
        return false;
    }
    if (bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        CloseSocket(sock);
        outError = "Port " + std::to_string(port) + " on " + host + " could not be bound.";
        return false;
    }
    if (listen(sock, 4) != 0)
    {
        CloseSocket(sock);
        outError = "The socket could not listen.";
        return false;
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int boundLength = sizeof(bound);
#else
    socklen_t boundLength = sizeof(bound);
#endif
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0)
    {
        boundPort = ntohs(bound.sin_port);
    }
    else
    {
        boundPort = port;
    }
    listenSocket = static_cast<std::intptr_t>(sock);
    running.store(true);
    acceptor = std::thread([this] { AcceptLoop(); });
    return true;
}

void WebSocketServer::Stop()
{
    if (!running.exchange(false))
    {
        if (acceptor.joinable()) acceptor.join();
        return;
    }
    if (listenSocket != -1)
    {
#ifdef _WIN32
        shutdown(static_cast<SocketHandle>(listenSocket), SD_BOTH);
#else
        shutdown(static_cast<SocketHandle>(listenSocket), SHUT_RDWR);
#endif
        CloseSocket(static_cast<SocketHandle>(listenSocket));
        listenSocket = -1;
    }
    if (acceptor.joinable()) acceptor.join();
    std::vector<std::shared_ptr<Connection>> open;
    {
        std::lock_guard lock(mutex);
        for (auto& [id, connection] : connections) open.push_back(connection);
        connections.clear();
    }
    for (const std::shared_ptr<Connection>& connection : open)
    {
        if (connection->open.exchange(false))
        {
            std::lock_guard sendLock(connection->sendMutex);
            (void)SendAll(connection->socket, EncodeFrame(Opcode::Close, std::string("\x03\xE8", 2), false));
#ifdef _WIN32
            shutdown(connection->socket, SD_BOTH);
#else
            shutdown(connection->socket, SHUT_RDWR);
#endif
        }
        if (connection->worker.joinable()) connection->worker.join();
        CloseSocket(connection->socket);
    }
}

std::size_t WebSocketServer::ConnectionCount() const
{
    std::lock_guard lock(mutex);
    return connections.size();
}

void WebSocketServer::AcceptLoop()
{
    while (running.load())
    {
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLength = sizeof(peer);
#else
        socklen_t peerLength = sizeof(peer);
#endif
        const SocketHandle client = accept(static_cast<SocketHandle>(listenSocket),
            reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client == InvalidSocket)
        {
            if (!running.load()) break;
            continue;
        }
        int noDelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        auto connection = std::make_shared<Connection>();
        connection->socket = client;
        connection->open.store(true);
        {
            std::lock_guard lock(mutex);
            connection->id = nextConnection++;
            connections[connection->id] = connection;
        }
        connection->worker = std::thread([this, connection] { ServeConnection(connection); });
    }
}

void WebSocketServer::ServeConnection(std::shared_ptr<Connection> connection)
{
    std::string buffer;
    char chunk[16384];
    // The upgrade request first.
    bool upgraded = false;
    while (running.load() && connection->open.load() && !upgraded)
    {
#ifdef _WIN32
        const int count = recv(connection->socket, chunk, sizeof(chunk), 0);
        if (count <= 0) break;
#else
        const ssize_t count = recv(connection->socket, chunk, sizeof(chunk), 0);
        if (count <= 0) break;
#endif
        buffer.append(chunk, static_cast<std::size_t>(count));
        const std::size_t end = buffer.find("\r\n\r\n");
        if (end == std::string::npos)
        {
            if (buffer.size() > 16384) break;
            continue;
        }
        std::string key;
        std::string path;
        std::string error;
        if (!ParseUpgradeRequest(buffer.substr(0, end + 4), key, path, error))
        {
            (void)SendAll(connection->socket, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n" + error);
            break;
        }
        buffer.erase(0, end + 4);
        {
            std::lock_guard sendLock(connection->sendMutex);
            if (!SendAll(connection->socket, UpgradeResponse(key))) break;
        }
        upgraded = true;
    }
    if (upgraded)
    {
        if (onConnection) onConnection(connection->id, true);
        std::string message;
        Opcode messageOpcode = Opcode::Text;
        bool assembling = false;
        while (running.load() && connection->open.load())
        {
            Frame frame;
            std::string error;
            const DecodeResult decoded = DecodeFrame(buffer, frame, error);
            if (decoded == DecodeResult::Error) break;
            if (decoded == DecodeResult::Incomplete)
            {
#ifdef _WIN32
                const int count = recv(connection->socket, chunk, sizeof(chunk), 0);
                if (count <= 0) break;
#else
                const ssize_t count = recv(connection->socket, chunk, sizeof(chunk), 0);
                if (count <= 0) break;
#endif
                buffer.append(chunk, static_cast<std::size_t>(count));
                continue;
            }
            if (frame.opcode == Opcode::Close)
            {
                std::lock_guard sendLock(connection->sendMutex);
                (void)SendAll(connection->socket, EncodeFrame(Opcode::Close, frame.payload.substr(0, 2), false));
                break;
            }
            if (frame.opcode == Opcode::Ping)
            {
                std::lock_guard sendLock(connection->sendMutex);
                if (!SendAll(connection->socket, EncodeFrame(Opcode::Pong, frame.payload, false))) break;
                continue;
            }
            if (frame.opcode == Opcode::Pong) continue;
            if (frame.opcode == Opcode::Binary)
            {
                // The protocol she speaks is text only; a binary frame is a mistake worth
                // ending the connection over rather than guessing at.
                break;
            }
            if (frame.opcode == Opcode::Text)
            {
                message = frame.payload;
                messageOpcode = Opcode::Text;
                assembling = !frame.fin;
            }
            else if (frame.opcode == Opcode::Continuation)
            {
                if (!assembling) break;
                message += frame.payload;
                if (message.size() > MaximumMessage) break;
                assembling = !frame.fin;
            }
            if (!assembling && messageOpcode == Opcode::Text && onMessage)
            {
                onMessage(connection->id, message);
                message.clear();
            }
        }
        if (onConnection) onConnection(connection->id, false);
    }
    connection->open.store(false);
    Drop(connection->id);
}

void WebSocketServer::Drop(const ConnectionId id)
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard lock(mutex);
        const auto found = connections.find(id);
        if (found == connections.end()) return;
        connection = found->second;
        connections.erase(found);
    }
    // The connection's own thread ends after this; the socket is closed once it has.
    if (connection->worker.joinable()) connection->worker.detach();
    CloseSocket(connection->socket);
}

bool WebSocketServer::Send(const ConnectionId id, const std::string& text)
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard lock(mutex);
        const auto found = connections.find(id);
        if (found == connections.end()) return false;
        connection = found->second;
    }
    if (!connection->open.load()) return false;
    std::lock_guard sendLock(connection->sendMutex);
    return SendAll(connection->socket, EncodeFrame(Opcode::Text, text, false));
}

void WebSocketServer::Close(const ConnectionId id)
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard lock(mutex);
        const auto found = connections.find(id);
        if (found == connections.end()) return;
        connection = found->second;
    }
    if (connection->open.exchange(false))
    {
        std::lock_guard sendLock(connection->sendMutex);
        (void)SendAll(connection->socket, EncodeFrame(Opcode::Close, std::string("\x03\xE8", 2), false));
#ifdef _WIN32
        shutdown(connection->socket, SD_BOTH);
#else
        shutdown(connection->socket, SHUT_RDWR);
#endif
    }
}

} // namespace revia::net

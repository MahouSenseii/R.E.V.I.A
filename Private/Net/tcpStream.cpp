#include "Net/tcpStream.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace revia::net
{

namespace
{
constexpr std::size_t ReadChunk = 65536;
constexpr std::size_t LongestLine = 4 * 1024 * 1024;

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

void CloseSocket(const SocketHandle socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

// True when the socket is readable (or has an error to report) within the time.
bool WaitReadable(const SocketHandle socket, const std::chrono::milliseconds timeout)
{
#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval limit{};
    limit.tv_sec = static_cast<long>(timeout.count() / 1000);
    limit.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return select(0, &readable, nullptr, nullptr, &limit) > 0;
#else
    pollfd waiting{};
    waiting.fd = socket;
    waiting.events = POLLIN;
    return poll(&waiting, 1, static_cast<int>(std::max<long long>(0, timeout.count()))) > 0;
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

bool FillAddress(const std::string& host, const std::uint16_t port, sockaddr_in& outAddress)
{
    outAddress = sockaddr_in{};
    outAddress.sin_family = AF_INET;
    outAddress.sin_port = htons(port);
    return inet_pton(AF_INET, host.c_str(), &outAddress.sin_addr) == 1;
}
} // namespace

TcpStream::~TcpStream()
{
    Close();
}

TcpStream::TcpStream(TcpStream&& other) noexcept
    : handle(other.handle), pending(std::move(other.pending))
{
    other.handle = -1;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept
{
    if (this != &other)
    {
        Close();
        handle = other.handle;
        pending = std::move(other.pending);
        other.handle = -1;
    }
    return *this;
}

std::optional<TcpStream> TcpStream::Connect(
    const std::string& host, const std::uint16_t port, const std::chrono::milliseconds timeout, std::string& outError)
{
    EnsureSocketsStarted();
    sockaddr_in address{};
    if (!FillAddress(host, port, address))
    {
        outError = "The stage address must be an IPv4 address such as 127.0.0.1.";
        return std::nullopt;
    }
    const SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == InvalidSocket)
    {
        outError = "A socket could not be created.";
        return std::nullopt;
    }
    // A non-blocking connect with a wait, so an unreachable stage costs the timeout and
    // not a minute of the caller's thread.
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    const int connected = connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    bool pendingConnect = connected == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    const int connected = connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    bool pendingConnect = connected != 0 && errno == EINPROGRESS;
#endif
    if (connected != 0 && !pendingConnect)
    {
        CloseSocket(sock);
        outError = "The stage at " + host + ":" + std::to_string(port) + " refused the connection.";
        return std::nullopt;
    }
    if (pendingConnect)
    {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(sock, &writable);
        timeval limit{};
        limit.tv_sec = static_cast<long>(timeout.count() / 1000);
        limit.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
        const int ready = select(0, nullptr, &writable, nullptr, &limit);
#else
        const int ready = select(sock + 1, nullptr, &writable, nullptr, &limit);
#endif
        int error = 0;
#ifdef _WIN32
        int errorLength = sizeof(error);
#else
        socklen_t errorLength = sizeof(error);
#endif
        if (ready <= 0 ||
            getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLength) != 0 || error != 0)
        {
            CloseSocket(sock);
            outError = "The stage at " + host + ":" + std::to_string(port) +
                (ready <= 0 ? " did not answer in time." : " refused the connection.");
            return std::nullopt;
        }
    }
#ifdef _WIN32
    nonBlocking = 0;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
#else
    fcntl(sock, F_SETFL, flags);
#endif
    int noDelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return TcpStream(static_cast<std::intptr_t>(sock));
}

bool TcpStream::WriteLine(const std::string& line)
{
    if (handle == -1) return false;
    if (!SendAll(static_cast<SocketHandle>(handle), line + "\n"))
    {
        Close();
        return false;
    }
    return true;
}

bool TcpStream::Fill(const std::chrono::milliseconds timeout, bool& outClosed)
{
    outClosed = false;
    if (handle == -1)
    {
        outClosed = true;
        return false;
    }
    if (!WaitReadable(static_cast<SocketHandle>(handle), timeout)) return false;
    char buffer[ReadChunk];
#ifdef _WIN32
    const int count = recv(static_cast<SocketHandle>(handle), buffer, sizeof(buffer), 0);
#else
    const ssize_t count = recv(static_cast<SocketHandle>(handle), buffer, sizeof(buffer), 0);
#endif
    if (count <= 0)
    {
        outClosed = true;
        Close();
        return false;
    }
    pending.append(buffer, static_cast<std::size_t>(count));
    return true;
}

bool TcpStream::ReadLine(std::string& outLine, const std::chrono::milliseconds timeout, bool& outClosed)
{
    outClosed = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        const std::size_t newline = pending.find('\n');
        if (newline != std::string::npos)
        {
            outLine = pending.substr(0, newline);
            if (!outLine.empty() && outLine.back() == '\r') outLine.pop_back();
            pending.erase(0, newline + 1);
            return true;
        }
        if (pending.size() > LongestLine)
        {
            // A line that never ends is not a line; the connection is not worth keeping.
            Close();
            outClosed = true;
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        bool closed = false;
        if (!Fill(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), closed))
        {
            if (closed)
            {
                outClosed = true;
                return false;
            }
        }
    }
}

void TcpStream::Close()
{
    if (handle == -1) return;
#ifdef _WIN32
    shutdown(static_cast<SocketHandle>(handle), SD_BOTH);
#else
    shutdown(static_cast<SocketHandle>(handle), SHUT_RDWR);
#endif
    CloseSocket(static_cast<SocketHandle>(handle));
    handle = -1;
    pending.clear();
}

TcpListener::~TcpListener()
{
    Stop();
}

bool TcpListener::Start(const std::string& host, const std::uint16_t port, std::string& outError)
{
    EnsureSocketsStarted();
    Stop();
    sockaddr_in address{};
    if (!FillAddress(host, port, address))
    {
        outError = "The listen address must be an IPv4 address such as 127.0.0.1.";
        return false;
    }
    const SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == InvalidSocket)
    {
        outError = "A listening socket could not be created.";
        return false;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(sock, 4) != 0)
    {
        CloseSocket(sock);
        outError = "Port " + std::to_string(port) + " on " + host + " could not be bound.";
        return false;
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int boundLength = sizeof(bound);
#else
    socklen_t boundLength = sizeof(bound);
#endif
    boundPort = getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0 ? ntohs(bound.sin_port) : port;
    handle = static_cast<std::intptr_t>(sock);
    return true;
}

std::optional<TcpStream> TcpListener::Accept(const std::chrono::milliseconds timeout)
{
    if (handle == -1) return std::nullopt;
    if (!WaitReadable(static_cast<SocketHandle>(handle), timeout)) return std::nullopt;
    sockaddr_in peer{};
#ifdef _WIN32
    int peerLength = sizeof(peer);
#else
    socklen_t peerLength = sizeof(peer);
#endif
    const SocketHandle client = accept(static_cast<SocketHandle>(handle), reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (client == InvalidSocket) return std::nullopt;
    int noDelay = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return TcpStream(static_cast<std::intptr_t>(client));
}

void TcpListener::Stop()
{
    if (handle == -1) return;
#ifdef _WIN32
    shutdown(static_cast<SocketHandle>(handle), SD_BOTH);
#else
    shutdown(static_cast<SocketHandle>(handle), SHUT_RDWR);
#endif
    CloseSocket(static_cast<SocketHandle>(handle));
    handle = -1;
}

} // namespace revia::net

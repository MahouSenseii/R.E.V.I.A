#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace revia::net
{

// A line-oriented TCP connection, both ends, on Winsock or POSIX. Enough for JSON
// lines between her and a stage guest; nothing here knows what the lines mean.
class TcpStream
{
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // An IPv4 address, never a name: a stage is reached by a fixed loopback or
    // private-switch address, and a name lookup is a way to end up somewhere else.
    static std::optional<TcpStream> Connect(
        const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout, std::string& outError);

    bool WriteLine(const std::string& line);
    // One line without its newline. False on a timeout (outLine untouched) and when
    // the other end closed, which outClosed says.
    bool ReadLine(std::string& outLine, std::chrono::milliseconds timeout, bool& outClosed);
    void Close();
    [[nodiscard]] bool IsOpen() const { return handle != -1; }

private:
    friend class TcpListener;
    explicit TcpStream(std::intptr_t socketHandle) : handle(socketHandle) {}
    bool Fill(std::chrono::milliseconds timeout, bool& outClosed);

    std::intptr_t handle = -1;
    std::string pending;
};

class TcpListener
{
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Port 0 picks a free one; Port() says which.
    bool Start(const std::string& host, std::uint16_t port, std::string& outError);
    [[nodiscard]] std::uint16_t Port() const { return boundPort; }
    // The next connection, or nothing when the time is up or the listener stopped.
    std::optional<TcpStream> Accept(std::chrono::milliseconds timeout);
    void Stop();
    [[nodiscard]] bool IsListening() const { return handle != -1; }

private:
    std::intptr_t handle = -1;
    std::uint16_t boundPort = 0;
};

} // namespace revia::net

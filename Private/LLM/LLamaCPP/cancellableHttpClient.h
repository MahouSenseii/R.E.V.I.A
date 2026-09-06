#pragma once

#include <httplib.h>
#include <algorithm>
#include <chrono>
#include <stop_token>

namespace revia::llm
{
// The embedding endpoint uses plain HTTP. Keep httplib's HTTP parsing, buffering
// and socket lifetime, but observe cancellation while waiting for response bytes.
// On Windows, shutdown(SD_BOTH) can succeed without waking select() on another
// thread. Never close its socket from that thread or shorten the request timeout.
class CancellableHttpClient final : public httplib::ClientImpl
{
public:
    CancellableHttpClient(const std::string& host, int port, std::stop_token token)
        : ClientImpl(host, port), stopToken(token) {}

private:
    class Stream final : public httplib::Stream
    {
    public:
        Stream(socket_t socket, std::chrono::microseconds readTimeout,
               std::chrono::microseconds writeTimeout, std::stop_token token)
            : delegate(socket, 0, 0, 0, 0), readTimeout(readTimeout),
              writeTimeout(writeTimeout), stopToken(token) {}

        bool is_readable() const override { return Wait(true, readTimeout); }
        bool is_writable() const override { return Wait(false, writeTimeout); }
        ssize_t read(char* data, std::size_t size) override
        {
            if (stopToken.stop_requested()) return -1;
            // Consume httplib's buffered bytes first; waiting on the socket before
            // this would stall even when a complete response was already buffered.
            const auto available = delegate.read(data, size);
            if (available >= 0) return available;
            return is_readable() ? delegate.read(data, size) : -1;
        }
        ssize_t write(const char* data, std::size_t size) override
        {
            return is_writable() ? delegate.write(data, size) : -1;
        }
        void get_remote_ip_and_port(std::string& ip, int& port) const override
        { delegate.get_remote_ip_and_port(ip, port); }
        void get_local_ip_and_port(std::string& ip, int& port) const override
        { delegate.get_local_ip_and_port(ip, port); }
        socket_t socket() const override { return delegate.socket(); }

    private:
        bool Wait(bool reading, std::chrono::microseconds timeout) const
        {
            using namespace std::chrono;
            const auto deadline = steady_clock::now() + timeout;
            do
            {
                if (stopToken.stop_requested()) return false;
                const auto remaining = duration_cast<microseconds>(deadline - steady_clock::now());
                const auto slice = std::clamp(remaining, microseconds::zero(), microseconds(100000));
                const auto ready = reading
                    ? httplib::detail::select_read(socket(), 0, slice.count())
                    : httplib::detail::select_write(socket(), 0, slice.count());
                if (ready != 0) return ready > 0 && !stopToken.stop_requested();
            } while (steady_clock::now() < deadline);
            return false;
        }

        httplib::detail::SocketStream delegate;
        std::chrono::microseconds readTimeout;
        std::chrono::microseconds writeTimeout;
        std::stop_token stopToken;
    };

    bool process_socket(const Socket& socket,
                        std::function<bool(httplib::Stream&)> callback) override
    {
        using namespace std::chrono;
        Stream stream(socket.sock, seconds(read_timeout_sec_) + microseconds(read_timeout_usec_),
                      seconds(write_timeout_sec_) + microseconds(write_timeout_usec_), stopToken);
        return !stopToken.stop_requested() && callback(stream);
    }

    std::stop_token stopToken;
};
}

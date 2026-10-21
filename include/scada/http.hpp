// A very small HTTP/1.1 server: enough for a JSON API and one static page. It
// serves one request per connection and closes, which keeps the whole thing
// single threaded and easy to reason about.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "scada/socket.hpp"

namespace scada {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::string header(const std::string& name) const;
    // Value of `name` in the query string, or an empty string.
    std::string query_value(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;

    static HttpResponse json(const std::string& body);
    static HttpResponse html(const std::string& body);
    static HttpResponse text(const std::string& body, int status = 200);
};

const char* status_text(int status);

// Pure parsing and serialisation, so both can be tested without a socket.
bool parse_request(const std::string& raw, HttpRequest& out);
std::string serialize_response(const HttpResponse& response);

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void set_handler(Handler handler) { handler_ = std::move(handler); }
    bool start(const std::string& address, std::uint16_t port, std::string& error);
    void stop();

    std::uint16_t port() const { return listener_.local_port(); }
    std::uint64_t requests_served() const { return requests_served_.load(); }

private:
    void run();

    Handler handler_;
    TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_served_{0};
};

}  // namespace scada

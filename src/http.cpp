#include "scada/http.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace scada {
namespace {

std::string trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::string lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

constexpr std::size_t kMaxRequestBytes = 64 * 1024;

// Content-Length from an already-split header block, or 0 when absent or junk.
std::size_t content_length_of(const std::string& header_block) {
    std::istringstream stream(header_block);
    std::string line;
    std::getline(stream, line);  // request line
    while (std::getline(stream, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (lower(trim(line.substr(0, colon))) != "content-length") {
            continue;
        }
        try {
            return static_cast<std::size_t>(std::stoul(trim(line.substr(colon + 1))));
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
    const std::string wanted = lower(name);
    for (const auto& entry : headers) {
        if (lower(entry.first) == wanted) {
            return entry.second;
        }
    }
    return {};
}

std::string HttpRequest::query_value(const std::string& name) const {
    std::istringstream stream(query);
    std::string part;
    while (std::getline(stream, part, '&')) {
        const std::size_t equals = part.find('=');
        const std::string key = equals == std::string::npos ? part : part.substr(0, equals);
        if (key == name) {
            return equals == std::string::npos ? std::string{} : part.substr(equals + 1);
        }
    }
    return {};
}

HttpResponse HttpResponse::json(const std::string& body) {
    HttpResponse response;
    response.content_type = "application/json; charset=utf-8";
    response.body = body;
    return response;
}

HttpResponse HttpResponse::html(const std::string& body) {
    HttpResponse response;
    response.content_type = "text/html; charset=utf-8";
    response.body = body;
    return response;
}

HttpResponse HttpResponse::text(const std::string& body, int status) {
    HttpResponse response;
    response.status = status;
    response.body = body;
    return response;
}

const char* status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
    }
}

bool parse_request(const std::string& raw, HttpRequest& out) {
    out = HttpRequest{};

    const std::size_t header_end = raw.find("\r\n\r\n");
    const std::size_t separator = header_end == std::string::npos ? raw.find("\n\n") : header_end;
    if (separator == std::string::npos) {
        return false;
    }
    const std::size_t header_length = separator;
    const std::size_t body_start = separator + (header_end == std::string::npos ? 2 : 4);

    std::istringstream stream(raw.substr(0, header_length));
    std::string line;
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    if (!(request_line >> out.method >> out.path >> out.version)) {
        return false;
    }
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") {
        return false;
    }

    const std::size_t question = out.path.find('?');
    if (question != std::string::npos) {
        out.query = out.path.substr(question + 1);
        out.path = out.path.substr(0, question);
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        out.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }

    std::size_t content_length = 0;
    const std::string length_header = out.header("content-length");
    if (!length_header.empty()) {
        try {
            content_length = static_cast<std::size_t>(std::stoul(length_header));
        } catch (const std::exception&) {
            return false;
        }
    }
    if (body_start + content_length > raw.size()) {
        return false;  // the caller has to read more bytes
    }
    out.body = raw.substr(body_start, content_length);
    return true;
}

std::string serialize_response(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << status_text(response.status) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    out << "\r\n";
    out << response.body;
    return out.str();
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(const std::string& address, std::uint16_t port, std::string& error) {
    if (running_.load()) {
        error = "http server already running";
        return false;
    }
    listener_ = TcpListener::bind_to(address, port, error);
    if (!listener_.valid()) {
        return false;
    }
    running_.store(true);
    thread_ = std::thread(&HttpServer::run, this);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    listener_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HttpServer::run() {
    while (running_.load()) {
        TcpSocket client = listener_.accept(200);
        if (!client.valid()) {
            continue;
        }

        std::string raw;
        char buffer[2048];
        std::size_t expected = 0;
        bool complete = false;
        while (running_.load() && raw.size() < kMaxRequestBytes) {
            const int received = client.receive(reinterpret_cast<std::uint8_t*>(buffer),
                                                sizeof(buffer), 2000);
            if (received <= 0) {
                break;
            }
            raw.append(buffer, static_cast<std::size_t>(received));

            const std::size_t separator = raw.find("\r\n\r\n");
            if (separator == std::string::npos) {
                continue;
            }
            if (expected == 0) {
                // Work out how long the body should be from the header block only.
                const std::string header_block = raw.substr(0, separator);
                expected = content_length_of(header_block);
            }
            if (raw.size() >= separator + 4 + expected) {
                complete = true;
                break;
            }
        }

        HttpResponse response;
        HttpRequest request;
        if (!complete || !parse_request(raw, request)) {
            response = HttpResponse::text("bad request", 400);
        } else if (!handler_) {
            response = HttpResponse::text("no handler", 500);
        } else {
            try {
                response = handler_(request);
            } catch (const std::exception& error) {
                response = HttpResponse::text(std::string("error: ") + error.what(), 500);
            }
        }

        const std::string serialized = serialize_response(response);
        client.send_bytes(reinterpret_cast<const std::uint8_t*>(serialized.data()),
                          serialized.size());
        requests_served_.fetch_add(1);
    }
}

}  // namespace scada

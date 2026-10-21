#include <string>

#include "scada/http.hpp"
#include "test_framework.hpp"

using namespace scada;

TEST(Http, ParsesAGetRequest) {
    const std::string raw = "GET /api/snapshot HTTP/1.1\r\nHost: localhost\r\n\r\n";
    HttpRequest request;
    CHECK(parse_request(raw, request));
    CHECK_EQ(request.method, std::string{"GET"});
    CHECK_EQ(request.path, std::string{"/api/snapshot"});
    CHECK_EQ(request.version, std::string{"HTTP/1.1"});
    CHECK_EQ(request.header("host"), std::string{"localhost"});
    CHECK(request.body.empty());
}

TEST(Http, ParsesAPostWithBody) {
    const std::string raw =
        "POST /api/ack?tag=TI-101 HTTP/1.1\r\nContent-Length: 5\r\nX-Test: 1\r\n\r\nhello";
    HttpRequest request;
    CHECK(parse_request(raw, request));
    CHECK_EQ(request.method, std::string{"POST"});
    CHECK_EQ(request.path, std::string{"/api/ack"});
    CHECK_EQ(request.query, std::string{"tag=TI-101"});
    CHECK_EQ(request.body, std::string{"hello"});
    CHECK_EQ(request.header("x-test"), std::string{"1"});
}

TEST(Http, SplitsQueryString) {
    HttpRequest request;
    request.query = "tag=TI-101&mode=fast";
    CHECK_EQ(request.query_value("tag"), std::string{"TI-101"});
    CHECK_EQ(request.query_value("mode"), std::string{"fast"});
    CHECK_EQ(request.query_value("missing"), std::string{});
}

TEST(Http, RejectsATruncatedBody) {
    const std::string raw =
        "POST /api/ack HTTP/1.1\r\nContent-Length: 10\r\n\r\nshort";
    HttpRequest request;
    CHECK(!parse_request(raw, request));
}

TEST(Http, RejectsAMalformedRequestLine) {
    HttpRequest request;
    CHECK(!parse_request("this is not http\r\n\r\n", request));
    CHECK(!parse_request("GET / HTTP/9.9\r\n\r\n", request));
    CHECK(!parse_request("GET /\r\n\r\n", request));
}

TEST(Http, SerializesWithACorrectContentLength) {
    const HttpResponse response = HttpResponse::json("{\"a\":1}");
    const std::string wire = serialize_response(response);
    CHECK(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(wire.find("Content-Type: application/json; charset=utf-8\r\n") != std::string::npos);
    CHECK(wire.find("Content-Length: 7\r\n") != std::string::npos);
    CHECK(wire.size() > 7);
    CHECK_EQ(wire.substr(wire.size() - 7), std::string{"{\"a\":1}"});
}

TEST(Http, StatusTextCoversWhatIsUsed) {
    CHECK_EQ(std::string{status_text(200)}, std::string{"OK"});
    CHECK_EQ(std::string{status_text(404)}, std::string{"Not Found"});
    CHECK_EQ(std::string{status_text(405)}, std::string{"Method Not Allowed"});
}

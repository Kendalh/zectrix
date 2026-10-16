// Minimal host implementation of esp_http_server on POSIX sockets.
//  - One thread per connection (so idle browser pre-connects never block), but handlers run under
//    a global lock, matching the device's single httpd task.
//  - Every response closes the connection; the firmware already sends "Connection: close".
//  - Bind address/port come from SIM_HTTP_HOST (default 127.0.0.1) and SIM_HTTP_PORT.
#include "esp_http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct RequestState {
    int fd = -1;
    std::string leftover;  // body bytes that arrived together with the headers
    size_t body_read = 0;
    std::string content_type = "text/html";
    std::vector<std::pair<std::string, std::string>> headers;
    bool responded = false;
};

struct Server {
    int listen_fd = -1;
    std::atomic<bool> running{false};
    std::thread acceptor;
    std::vector<httpd_uri_t> handlers;
    std::mutex handler_mutex;
};

const char* StatusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

bool SendAll(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

esp_err_t SendResponse(httpd_req_t* r, int code, const char* body, size_t len) {
    auto* st = static_cast<RequestState*>(r->aux);
    st->responded = true;
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + StatusText(code) + "\r\n";
    head += "Content-Type: " + st->content_type + "\r\n";
    head += "Content-Length: " + std::to_string(len) + "\r\n";
    head += "Connection: close\r\n";
    for (const auto& h : st->headers) {
        if (h.first == "Connection") continue;
        head += h.first + ": " + h.second + "\r\n";
    }
    head += "\r\n";
    if (!SendAll(st->fd, head.data(), head.size())) return ESP_FAIL;
    if (len && !SendAll(st->fd, body, len)) return ESP_FAIL;
    return ESP_OK;
}

void HandleConnection(Server* srv, int fd) {
    timeval tv{30, 0};  // mirrors config.recv_wait_timeout = 30
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buf;
    size_t header_end = std::string::npos;
    char tmp[4096];
    while (header_end == std::string::npos && buf.size() < 16384) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) { close(fd); return; }
        buf.append(tmp, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) { close(fd); return; }

    const std::string head = buf.substr(0, header_end);
    RequestState st;
    st.fd = fd;
    st.leftover = buf.substr(header_end + 4);

    httpd_req_t req{};
    char method[16] = {};
    char uri[512] = {};
    if (sscanf(head.c_str(), "%15s %511s", method, uri) != 2) { close(fd); return; }
    strncpy(req.uri, uri, sizeof(req.uri) - 1);
    req.method = !strcmp(method, "GET") ? HTTP_GET : !strcmp(method, "POST") ? HTTP_POST
               : !strcmp(method, "DELETE") ? HTTP_DELETE : -1;

    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        size_t next = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (auto& c : key) c = static_cast<char>(tolower(c));
            if (key == "content-length") req.content_len = strtoull(line.c_str() + colon + 1, nullptr, 10);
        }
        pos = next;
    }
    req.handle = srv;
    req.aux = &st;

    std::string path(uri);
    path = path.substr(0, path.find('?'));

    const httpd_uri_t* match = nullptr;
    bool path_known = false;
    for (const auto& h : srv->handlers) {
        if (path != h.uri) continue;
        path_known = true;
        if (static_cast<int>(h.method) == req.method) { match = &h; break; }
    }

    {
        std::lock_guard<std::mutex> lock(srv->handler_mutex);
        if (!match) {
            const char* msg = path_known ? "Method not allowed" : "Nothing matches the given URI";
            st.content_type = "text/plain";
            SendResponse(&req, path_known ? 405 : 404, msg, strlen(msg));
        } else {
            req.user_ctx = match->user_ctx;
            esp_err_t err = match->handler(&req);
            if (!st.responded) {
                // Like the device, a handler that fails without answering just drops the socket.
                fprintf(stderr, "[sim httpd] %s %s -> handler returned %d without a response\n", method, uri, err);
            }
        }
    }
    shutdown(fd, SHUT_WR);
    close(fd);
}

void AcceptLoop(Server* srv) {
    while (srv->running) {
        pollfd p{srv->listen_fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(srv->listen_fd, nullptr, nullptr);
        if (fd < 0) continue;
        std::thread(HandleConnection, srv, fd).detach();
    }
}

}  // namespace

extern "C" {

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
    const char* host = getenv("SIM_HTTP_HOST");
    const char* port_env = getenv("SIM_HTTP_PORT");
    const int port = port_env ? atoi(port_env) : config->server_port;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return ESP_FAIL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host ? host : "127.0.0.1", &addr.sin_addr) != 1 ||
        bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        perror("[sim httpd] bind/listen");
        close(fd);
        return ESP_FAIL;
    }
    auto* srv = new Server();
    srv->listen_fd = fd;
    srv->running = true;
    srv->acceptor = std::thread(AcceptLoop, srv);
    *handle = srv;
    fprintf(stderr, "[sim httpd] listening on http://%s:%d/\n", host ? host : "127.0.0.1", port);
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
    auto* srv = static_cast<Server*>(handle);
    if (!srv) return ESP_ERR_INVALID_ARG;
    srv->running = false;
    srv->acceptor.join();
    close(srv->listen_fd);
    delete srv;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* h) {
    auto* srv = static_cast<Server*>(handle);
    if (!srv || !h) return ESP_ERR_INVALID_ARG;
    srv->handlers.push_back(*h);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) {
    static_cast<RequestState*>(r->aux)->content_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value) {
    static_cast<RequestState*>(r->aux)->headers.emplace_back(field, value);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? static_cast<ssize_t>(strlen(buf)) : 0;
    return SendResponse(r, 200, buf, static_cast<size_t>(len));
}

esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str) {
    return httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_resp_send_err(httpd_req_t* r, httpd_err_code_t error, const char* msg) {
    static_cast<RequestState*>(r->aux)->content_type = "text/plain";
    return SendResponse(r, static_cast<int>(error), msg, msg ? strlen(msg) : 0);
}

int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
    auto* st = static_cast<RequestState*>(r->aux);
    const size_t remaining = r->content_len - st->body_read;
    if (remaining == 0) return 0;
    len = len < remaining ? len : remaining;
    if (!st->leftover.empty()) {
        const size_t n = len < st->leftover.size() ? len : st->leftover.size();
        memcpy(buf, st->leftover.data(), n);
        st->leftover.erase(0, n);
        st->body_read += n;
        return static_cast<int>(n);
    }
    ssize_t n = recv(st->fd, buf, len, 0);
    if (n <= 0) return -1;
    st->body_read += static_cast<size_t>(n);
    return static_cast<int>(n);
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t buf_len) {
    const char* q = strchr(r->uri, '?');
    if (!q || !q[1]) return ESP_ERR_NOT_FOUND;
    if (strlen(q + 1) >= buf_len) return ESP_ERR_NO_MEM;  // matches ESP: too-small buffer fails
    strcpy(buf, q + 1);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t val_size) {
    const size_t klen = strlen(key);
    const char* p = qry;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t n = strcspn(p, "&");
            if (n >= val_size) return ESP_ERR_NO_MEM;
            memcpy(val, p, n);
            val[n] = 0;
            return ESP_OK;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_to_sockfd(httpd_req_t* r) { return static_cast<RequestState*>(r->aux)->fd; }
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int) { return ESP_OK; }

}  // extern "C"

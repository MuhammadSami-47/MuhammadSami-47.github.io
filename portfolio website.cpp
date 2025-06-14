/*
Simple C++ HTTP server with file upload handling and static file serving.
This server serves static files from the "static" directory,
handles file uploads to "uploads" directory,
and serves the HTML frontend with cinematic background and smooth animations.

Compile with:
  g++ -std=c++17 -O2 -o server server.cpp -lpthread

Usage:
  ./server

Access:
  http://localhost:8080/

Note:
- This is a minimal example, not production ready.
- For production use, consider mature C++ web frameworks.
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <filesystem>
#include <regex>
#include <atomic>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace fs = std::filesystem;
using namespace std;

// Globals
constexpr int PORT = 8080;
constexpr int BACKLOG = 10;
const string STATIC_DIR = "static";
const string UPLOAD_DIR = "uploads";

atomic_bool running = true;

mutex cout_mutex;

// Utility function: URL decode
string url_decode(const string &src) {
    string ret;
    char ch;
    int i, ii;
    for (i = 0; i < (int)src.length(); i++) {
        if (int(src[i]) == 37) {
            sscanf(src.substr(i + 1, 2).c_str(), "%x", &ii);
            ch = static_cast<char>(ii);
            ret += ch;
            i = i + 2;
        } else if (src[i] == '+') {
            ret += ' ';
        } else {
            ret += src[i];
        }
    }
    return ret;
}

// Read entire file into string
string read_file(const string &path) {
    ifstream file(path, ios::in | ios::binary);
    if (!file) return "";
    ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Send HTTP response
void send_response(int client_fd, const string &status, const string &content_type,
                   const string &body) {
    ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    string resp_str = response.str();
    send(client_fd, resp_str.c_str(), resp_str.size(), 0);
}

// Get MIME type based on extension
string get_mime_type(const string &path) {
    static const vector<pair<string, string>> mime_types = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".svg", "image/svg+xml"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".ogg", "audio/ogg"},
        {".json", "application/json"},
        {".txt", "text/plain"},
        {".pdf", "application/pdf"}
    };
    string ext;
    size_t pos = path.find_last_of('.');
    if (pos != string::npos) ext = path.substr(pos);
    for (auto &p : mime_types) {
        if (p.first == ext) return p.second;
    }
    return "application/octet-stream";
}

// Read line from socket
bool read_line(int fd, string &line) {
    line.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\r') {
            char next_c;
            n = recv(fd, &next_c, 1, MSG_PEEK);
            if (n && next_c == '\n') {
                recv(fd, &next_c, 1, 0);
            }
            break;
        } else if (c == '\n') {
            break;
        } else {
            line += c;
        }
    }
    return true;
}

// Parse HTTP headers into map
void parse_headers(int client_fd, std::map<string, string> &headers) {
    string line;
    while (read_line(client_fd, line)) {
        if (line.empty()) break;
        size_t pos = line.find(':');
        if (pos != string::npos) {
            string key = line.substr(0, pos);
            string val = line.substr(pos + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(val.begin());
            headers[key] = val;
        }
    }
}

// Basic multipart form-data parser for a single file
bool parse_multipart_form(const string &body, const string &boundary,
                          string &filename, string &file_content) {
    size_t pos = 0;
    string delimiter = "--" + boundary;
    size_t delim_pos = body.find(delimiter, pos);
    if (delim_pos == string::npos) return false;
    pos = delim_pos + delimiter.size() + 2;

    size_t header_end = body.find("\r\n\r\n", pos);
    if (header_end == string::npos) return false;
    string headers_str = body.substr(pos, header_end - pos);
    pos = header_end + 4;

    regex filename_re(R"(filename="([^"]+)")");
    smatch match;
    if (!regex_search(headers_str, match, filename_re)) return false;
    filename = match[1];

    size_t end_pos = body.find(delimiter, pos);
    if (end_pos == string::npos) return false;
    file_content = body.substr(pos, end_pos - pos - 2);

    return true;
}

// Handle client connection
void handle_client(int client_fd) {
    string request_line;
    if (!read_line(client_fd, request_line)) {
        close(client_fd);
        return;
    }
    istringstream req_line_ss(request_line);
    string method, path, version;
    req_line_ss >> method >> path >> version;

    map<string, string> headers;
    parse_headers(client_fd, headers);

    if (method == "GET") {
        if (path == "/uploads") {
            list_uploaded_files(client_fd);
        } else {
            string filepath;
            if (path == "/") {
                filepath = STATIC_DIR + "/index.html";
            } else if (path.find("..") != string::npos) {
                send_response(client_fd, "400 Bad Request", "text/plain", "Invalid path");
                close(client_fd);
                return;
            } else {
                filepath = STATIC_DIR + path;
                if (!fs::exists(filepath)) {
                    if (path.find("/uploads/") == 0) {
                        filepath = "." + path;
                        if (!fs::exists(filepath)) {
                            send_response(client_fd, "404 Not Found", "text/plain", "File not found");
                            close(client_fd);
                            return;
                        }
                    } else {
                        send_response(client_fd, "404 Not Found", "text/plain", "File not found");
                        close(client_fd);
                        return;
                    }
                }
            }
            string body = read_file(filepath);
            if (body.empty()) {
                send_response(client_fd, "500 Internal Server Error", "text/plain", "Error reading file");
                close(client_fd);
                return;
            }
            string mime = get_mime_type(filepath);
            send_response(client_fd, "200 OK", mime, body);
            close(client_fd);
            return;
        }
    } else if (method == "POST" && path == "/upload") {
        if (headers.find("Content-Length") == headers.end()) {
            send_response(client_fd, "411 Length Required", "text/plain", "Content-Length required");
            close(client_fd);
            return;
        }
        size_t content_length = stoi(headers["Content-Length"]);

        if (headers.find("Content-Type") == headers.end()) {
            send_response(client_fd, "400 Bad Request", "text/plain", "Content-Type required");
            close(client_fd);
            return;
        }
        string content_type = headers["Content-Type"];
        string boundary;
        regex boundary_re(R"(boundary=([^\s;]+))");
        smatch match;
        if (!regex_search(content_type, match, boundary_re)) {
            send_response(client_fd, "400 Bad Request", "text/plain", "Boundary missing in Content-Type");
            close(client_fd);
            return;
        }
        boundary = match[1];

        string body;
        size_t total_received = 0;
        while (total_received < content_length) {
            char buf[8192];
            size_t to_read = min(sizeof(buf), content_length - total_received);
            ssize_t n = recv(client_fd, buf, to_read, 0);
            if (n <= 0) break;
            body.append(buf, n);
            total_received += n;
        }

        string filename, file_content;
        if (!parse_multipart_form(body, boundary, filename, file_content)) {
            send_response(client_fd, "400 Bad Request", "text/plain", "Failed to parse form-data");
            close(client_fd);
            return;
        }

        size_t pos = filename.find_last_of("/\\");
        if (pos != string::npos) {
            filename = filename.substr(pos + 1);
        }
        fs::create_directories(UPLOAD_DIR);
        string save_path = UPLOAD_DIR + "/" + filename;
        ofstream ofs(save_path, ios::binary);
        ofs.write(file_content.data(), file_content.size());
        ofs.close();

        string resp_body = R"(
<html>
<head>
    <title>Upload Success</title>
    <meta http-equiv="refresh" content="2;url=/" />
    <style>
    body {background:#000;color:#0f0;font-family:monospace;text-align:center;padding:50px;}
    </style>
</head>
<body>
<h1>Uploaded Successfully!</h1>
<p>Redirecting back to portfolio...</p>
</body>
</html>)";
        send_response(client_fd, "200 OK", "text/html", resp_body);
        close(client_fd);
        return;
    } else {
        send_response(client_fd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
        close(client_fd);
        return;
    }
}

void list_uploaded_files(int client_fd) {
    string body = "<html><body><h1>Uploaded Files</h1><ul>";
    for (const auto &entry : fs::directory_iterator(UPLOAD_DIR)) {
        body += "<li><a href='/uploads/" + entry.path().filename().string() + "'>" + entry.path().filename().string() + "</a></li>";
    }
    body += "</ul></body></html>";
    send_response(client_fd, "200 OK", "text/html", body);
}

int main() {
    cout << "Starting server on http://localhost:" << PORT << "\n";

    fs::create_directories(UPLOAD_DIR);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "Bind failed\n";
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        cerr << "Listen failed\n";
        return 1;
    }

    while (running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            cerr << "Accept failed\n";
            continue;
        }

        thread t(handle_client, client_fd);
        t.detach();
    }

    close(server_fd);
    return 0;
}

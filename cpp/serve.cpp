// lmok serve / lmok remote. See serve.hpp and docs/serve.md.
#include "serve.hpp"

#include "dslvm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // inet_pton
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <io.h>
#include <fcntl.h>
using socket_t = SOCKET;
static const socket_t BAD_SOCKET = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
static const socket_t BAD_SOCKET = -1;
static void close_socket(socket_t s) { ::close(s); }
#endif

namespace fs = std::filesystem;

namespace lmok {
namespace {

// ---------------------------------------------------------------- JSON ----

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool boolean = false;
  double number = 0;
  bool integral = false;
  long long integer = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;

  static Json null() { return Json(); }
  static Json of(bool v) { Json j; j.type = Bool; j.boolean = v; return j; }
  static Json of(long long v) { Json j; j.type = Number; j.number = double(v); j.integral = true; j.integer = v; return j; }
  static Json of(int v) { return of(static_cast<long long>(v)); }
  static Json of(size_t v) { return of(static_cast<long long>(v)); }
  static Json of(const std::string &v) { Json j; j.type = String; j.text = v; return j; }
  static Json of(const char *v) { return of(std::string(v)); }
  static Json array() { Json j; j.type = Array; return j; }
  static Json object() { Json j; j.type = Object; return j; }
  static Json strings(const std::vector<std::string> &v) {
    Json j = array();
    for (const auto &s : v) j.items.push_back(of(s));
    return j;
  }

  const Json *get(const std::string &key) const {
    if (type != Object) return nullptr;
    for (const auto &f : fields)
      if (f.first == key) return &f.second;
    return nullptr;
  }
  Json &set(const std::string &key, Json value) {
    for (auto &f : fields)
      if (f.first == key) { f.second = std::move(value); return f.second; }
    fields.emplace_back(key, std::move(value));
    return fields.back().second;
  }
  Json &push(Json value) { items.push_back(std::move(value)); return items.back(); }
};

struct JsonError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void append_utf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
  else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

class JsonParser {
public:
  explicit JsonParser(const std::string &s) : s_(s) {}
  Json parse() {
    Json v = value(0);
    ws();
    if (p_ != s_.size()) fail("trailing characters");
    return v;
  }

private:
  const std::string &s_;
  size_t p_ = 0;
  [[noreturn]] void fail(const std::string &why) { throw JsonError("invalid JSON at byte " + std::to_string(p_) + ": " + why); }
  void ws() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_; }
  bool lit(const char *w) {
    size_t n = std::strlen(w);
    if (s_.compare(p_, n, w) == 0) { p_ += n; return true; }
    return false;
  }
  unsigned hex4() {
    if (p_ + 4 > s_.size()) fail("short \\u escape");
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[p_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else fail("bad \\u escape");
    }
    return v;
  }
  std::string str() {
    if (s_[p_] != '"') fail("expected string");
    ++p_;
    std::string out;
    while (true) {
      if (p_ >= s_.size()) fail("unterminated string");
      char c = s_[p_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
      if (c != '\\') { out += c; continue; }
      if (p_ >= s_.size()) fail("unterminated escape");
      char e = s_[p_++];
      switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t cp = hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (!(p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u')) fail("lone surrogate");
          p_ += 2;
          uint32_t lo = hex4();
          if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair");
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) fail("lone surrogate");
        append_utf8(out, cp);
        break;
      }
      default: fail("bad escape");
      }
    }
  }
  Json value(int depth) {
    if (depth > 64) fail("nesting too deep");
    ws();
    if (p_ >= s_.size()) fail("unexpected end");
    char c = s_[p_];
    if (c == '{') {
      ++p_;
      Json o = Json::object();
      ws();
      if (p_ < s_.size() && s_[p_] == '}') { ++p_; return o; }
      while (true) {
        ws();
        if (p_ >= s_.size()) fail("unexpected end");
        std::string k = str();
        ws();
        if (p_ >= s_.size() || s_[p_] != ':') fail("expected ':'");
        ++p_;
        o.set(k, value(depth + 1));
        ws();
        if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return o; }
        fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++p_;
      Json a = Json::array();
      ws();
      if (p_ < s_.size() && s_[p_] == ']') { ++p_; return a; }
      while (true) {
        a.push(value(depth + 1));
        ws();
        if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return a; }
        fail("expected ',' or ']'");
      }
    }
    if (c == '"') return Json::of(str());
    if (lit("true")) return Json::of(true);
    if (lit("false")) return Json::of(false);
    if (lit("null")) return Json::null();
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = p_;
      if (s_[p_] == '-') ++p_;
      while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
      bool frac = false;
      if (p_ < s_.size() && s_[p_] == '.') { frac = true; ++p_; while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_; }
      if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
        frac = true; ++p_;
        if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
        while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
      }
      std::string raw = s_.substr(start, p_ - start);
      if (raw == "-" || raw.empty()) fail("bad number");
      Json j;
      j.type = Json::Number;
      j.number = std::strtod(raw.c_str(), nullptr);
      if (!frac && raw.size() < 19) { j.integral = true; j.integer = std::strtoll(raw.c_str(), nullptr, 10); }
      return j;
    }
    fail("unexpected character");
  }
};

bool valid_utf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k < n; ++k) {
      unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d >> 6) != 2) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += n;
  }
  return true;
}

void dump_string(std::string &out, const std::string &s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    default:
      if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
      else out += char(c);
    }
  }
  out += '"';
}

void dump(std::string &out, const Json &j, int indent, int level) {
  auto nl = [&](int l) { if (indent) { out += '\n'; out.append(size_t(l * indent), ' '); } };
  switch (j.type) {
  case Json::Null: out += "null"; break;
  case Json::Bool: out += j.boolean ? "true" : "false"; break;
  case Json::Number:
    if (j.integral) out += std::to_string(j.integer);
    else { char buf[32]; std::snprintf(buf, sizeof buf, "%.17g", j.number); out += buf; }
    break;
  case Json::String: dump_string(out, j.text); break;
  case Json::Array:
    if (j.items.empty()) { out += "[]"; break; }
    out += '[';
    for (size_t i = 0; i < j.items.size(); ++i) { if (i) out += ','; nl(level + 1); dump(out, j.items[i], indent, level + 1); }
    nl(level); out += ']';
    break;
  case Json::Object:
    if (j.fields.empty()) { out += "{}"; break; }
    out += '{';
    for (size_t i = 0; i < j.fields.size(); ++i) {
      if (i) out += ',';
      nl(level + 1);
      dump_string(out, j.fields[i].first);
      out += indent ? ": " : ":";
      dump(out, j.fields[i].second, indent, level + 1);
    }
    nl(level); out += '}';
    break;
  }
}

std::string to_json(const Json &j, int indent = 2) {
  std::string out;
  dump(out, j, indent, 0);
  return out;
}

// -------------------------------------------------------------- base64 ----

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string &in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
    out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += B64[v & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t v = uint32_t(uint8_t(in[i])) << 16;
    out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
    out += B64[v >> 18]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += '=';
  }
  return out;
}

std::optional<std::string> base64_decode(const std::string &in) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  size_t pad = 0;
  for (char c : in) {
    if (c == '\n' || c == '\r' || c == ' ') continue;
    if (c == '=') { ++pad; continue; }
    if (pad) return std::nullopt;
    const char *p = std::strchr(B64, c);
    if (!p || !c) return std::nullopt;
    acc = (acc << 6) | uint32_t(p - B64);
    bits += 6;
    if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
  }
  if (pad > 2) return std::nullopt;
  return out;
}

// --------------------------------------------------------------- misc -----

std::string hex_sha256(const std::string &data) { return dslvm::hex(dslvm::sha256(data)); }

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

int process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return int(getpid());
#endif
}

bool read_whole(const fs::path &p, std::string &out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return !in.bad();
}

void set_env(const char *name, const std::optional<std::string> &value) {
#ifdef _WIN32
  std::wstring wname;
  for (const char *p = name; *p; ++p) wname.push_back(wchar_t(*p));
  std::wstring wvalue = value ? fs::u8path(*value).wstring() : std::wstring();
  _wputenv_s(wname.c_str(), wvalue.c_str());
#else
  if (value) setenv(name, value->c_str(), 1);
  else unsetenv(name);
#endif
}

std::optional<std::string> get_env(const char *name) {
  const char *v = std::getenv(name);
  if (!v) return std::nullopt;
  return std::string(v);
}

const char *INVOCATION_ENV = "LMCODE_INVOCATION_FILE";

// ------------------------------------------------------------- sockets ----

struct NetInit {
  NetInit() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
  }
  ~NetInit() {
#ifdef _WIN32
    WSACleanup();
#endif
  }
};

bool resolve_ipv4(const std::string &host, in_addr &addr) {
  std::string h = host == "localhost" ? "127.0.0.1" : host;
  return inet_pton(AF_INET, h.c_str(), &addr) == 1;
}

void set_timeouts(socket_t s, int seconds) {
#ifdef _WIN32
  DWORD ms = DWORD(seconds) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
#else
  timeval tv{};
  tv.tv_sec = seconds;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool send_all(socket_t s, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    size_t chunk = std::min<size_t>(data.size() - sent, 1 << 20);
#ifdef _WIN32
    int n = ::send(s, data.data() + sent, int(chunk), 0);
#elif defined(MSG_NOSIGNAL)
    ssize_t n = ::send(s, data.data() + sent, chunk, MSG_NOSIGNAL);
#else
    ssize_t n = ::send(s, data.data() + sent, chunk, 0);
#endif
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

long recv_some(socket_t s, char *buf, size_t cap) {
#ifdef _WIN32
  return ::recv(s, buf, int(cap), 0);
#else
  return long(::recv(s, buf, cap, 0));
#endif
}

// ---------------------------------------------------------------- HTTP ----

struct Request {
  std::string method, target, path, query;
  std::map<std::string, std::string> headers; // lower-case names
  std::string body;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

const char *reason(int status) {
  switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 413: return "Payload Too Large";
  case 422: return "Unprocessable Entity";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  default: return "Status";
  }
}

std::string lower(std::string s) {
  for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string percent_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else out += s[i];
  }
  return out;
}

// Reads one request. Returns 0 on success, an HTTP status on a protocol
// error, or -1 when the peer sent nothing usable (closed / timed out).
int read_request(socket_t s, Request &req, size_t max_body) {
  std::string buf;
  char chunk[16384];
  size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    long n = recv_some(s, chunk, sizeof chunk);
    if (n <= 0) return buf.empty() ? -1 : 400;
    buf.append(chunk, size_t(n));
    header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos && buf.size() > 65536) return 431;
  }
  std::istringstream head(buf.substr(0, header_end));
  std::string line;
  std::getline(head, line);
  line = trim(line);
  {
    std::istringstream rl(line);
    std::string version;
    rl >> req.method >> req.target >> version;
    if (req.method.empty() || req.target.empty() || version.rfind("HTTP/1.", 0) != 0) return 400;
  }
  while (std::getline(head, line)) {
    line = trim(line);
    if (line.empty()) continue;
    size_t colon = line.find(':');
    if (colon == std::string::npos) return 400;
    req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  size_t q = req.target.find('?');
  req.path = req.target.substr(0, q);
  if (q != std::string::npos) req.query = req.target.substr(q + 1);
  if (req.headers.count("transfer-encoding")) return 501;
  size_t length = 0;
  if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
    try { length = size_t(std::stoull(it->second)); } catch (...) { return 400; }
  }
  if (length > max_body) { // drain (bounded) so the client sees the 413 rather than a reset
    size_t seen = buf.size() - header_end - 4;
    while (seen < length && seen < (size_t(64) << 20)) {
      long n = recv_some(s, chunk, sizeof chunk);
      if (n <= 0) break;
      seen += size_t(n);
    }
    return 413;
  }
  req.body = buf.substr(header_end + 4);
  while (req.body.size() < length) {
    long n = recv_some(s, chunk, sizeof chunk);
    if (n <= 0) return 400;
    req.body.append(chunk, size_t(n));
  }
  req.body.resize(length);
  return 0;
}

void write_response(socket_t s, const Response &r) {
  std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
  out += "Content-Type: " + r.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
  out += "Connection: close\r\n";
  out += "Cache-Control: no-store\r\n";
  for (const auto &h : r.headers) out += h.first + ": " + h.second + "\r\n";
  out += "\r\n";
  out += r.body;
  send_all(s, out);
}

Response json_response(int status, const Json &body) {
  Response r;
  r.status = status;
  r.body = to_json(body) + "\n";
  return r;
}

Response error_response(int status, const std::string &code, const std::string &message) {
  Json e = Json::object();
  Json inner = Json::object();
  inner.set("status", Json::of(status));
  inner.set("code", Json::of(code));
  inner.set("message", Json::of(message));
  e.set("error", inner);
  return json_response(status, e);
}

struct HttpError : std::runtime_error {
  int status;
  std::string code;
  HttpError(int s, std::string c, const std::string &m) : std::runtime_error(m), status(s), code(std::move(c)) {}
};

[[noreturn]] void bad_request(const std::string &message) { throw HttpError(400, "bad_request", message); }

// ------------------------------------------------------------ settings ----

// Every setting the server understands, with its type and a description.
// The table drives validation, GET /v1/settings/schema and the docs.
struct SettingSpec {
  const char *name;
  Json::Type type;
  const char *description;
};

const std::vector<SettingSpec> &setting_specs() {
  static const std::vector<SettingSpec> specs = {
      {"idle_timeout_s", Json::Number, "Seconds without a request before the server exits; 0 disables the timeout."},
      {"cache", Json::Bool, "Reuse results of side-effect-free commands while their inputs are unchanged."},
      {"cache_max_entries", Json::Number, "Least-recently-used cache capacity (entries)."},
      {"cache_lmcode", Json::Bool, "Also cache --lmcode program runs (programs may read files the key cannot see)."},
      {"results_max", Json::Number, "How many recent results stay addressable under /v1/results."},
      {"lang", Json::String, "Default --lang for parse/query/facts/metrics/dup; empty leaves each command to its own default (parse/query: json; facts/metrics: by extension)."},
      {"pack", Json::String, "Default rule pack for /v1/lint (path relative to cwd)."},
      {"metadata", Json::Bool, "Add --metadata to lint, check and lmcode runs."},
      {"cwd", Json::String, "Working directory for commands; empty means the directory the server started in."},
      {"max_body_bytes", Json::Number, "Largest accepted request body."},
  };
  return specs;
}

Json default_settings() {
  Json s = Json::object();
  s.set("idle_timeout_s", Json::of(300));
  s.set("cache", Json::of(true));
  s.set("cache_max_entries", Json::of(256));
  s.set("cache_lmcode", Json::of(false));
  s.set("results_max", Json::of(64));
  s.set("lang", Json::of(""));
  s.set("pack", Json::of(""));
  s.set("metadata", Json::of(false));
  s.set("cwd", Json::of(""));
  s.set("max_body_bytes", Json::of(16 * 1024 * 1024));
  return s;
}

// Validates and merges `patch` into `settings`. Throws HttpError.
void merge_settings(Json &settings, const Json &patch) {
  if (patch.type != Json::Object) bad_request("settings must be a JSON object");
  for (const auto &f : patch.fields) {
    const SettingSpec *spec = nullptr;
    for (const auto &s : setting_specs())
      if (f.first == s.name) spec = &s;
    if (!spec) throw HttpError(422, "unknown_setting", "unknown setting '" + f.first + "' (see GET /v1/settings/schema)");
    if (f.second.type != spec->type) throw HttpError(422, "invalid_setting", "setting '" + f.first + "' has the wrong type");
    if (spec->type == Json::Number && (!f.second.integral || f.second.integer < 0))
      throw HttpError(422, "invalid_setting", "setting '" + f.first + "' must be a non-negative integer");
    if (f.first == "cache_max_entries" && f.second.integer > 100000) throw HttpError(422, "invalid_setting", "cache_max_entries is at most 100000");
    if (f.first == "results_max" && (f.second.integer < 1 || f.second.integer > 10000)) throw HttpError(422, "invalid_setting", "results_max must be 1..10000");
    if (f.first == "max_body_bytes" && f.second.integer < 1024) throw HttpError(422, "invalid_setting", "max_body_bytes must be at least 1024");
    if (f.first == "cwd" && !f.second.text.empty()) {
      std::error_code ec;
      if (!fs::is_directory(fs::u8path(f.second.text), ec)) throw HttpError(422, "invalid_setting", "cwd is not a directory: " + f.second.text);
    }
    settings.set(f.first, f.second);
  }
}

long long setting_int(const Json &s, const char *k) { const Json *v = s.get(k); return v ? v->integer : 0; }
bool setting_bool(const Json &s, const char *k) { const Json *v = s.get(k); return v && v->boolean; }
std::string setting_str(const Json &s, const char *k) { const Json *v = s.get(k); return v ? v->text : ""; }

struct Preset {
  std::string name, description;
  Json settings; // a partial settings object applied over the defaults
};

std::vector<Preset> builtin_presets() {
  auto make = [](const std::string &name, const std::string &desc, std::vector<std::pair<std::string, Json>> kv) {
    Preset p{name, desc, Json::object()};
    for (auto &f : kv) p.settings.set(f.first, f.second);
    return p;
  };
  return {
      make("default", "Balanced: 5 minute idle timeout, result cache on.", {}),
      make("interactive", "Never time out; long result history for exploratory sessions.",
           {{"idle_timeout_s", Json::of(0)}, {"results_max", Json::of(512)}}),
      make("ci", "Short-lived build helper: 2 minute idle timeout, small history.",
           {{"idle_timeout_s", Json::of(120)}, {"results_max", Json::of(16)}}),
      make("fresh", "No caching: every request runs the command again.", {{"cache", Json::of(false)}}),
      make("memo", "Cache everything, including lmcode program runs.",
           {{"cache_lmcode", Json::of(true)}, {"cache_max_entries", Json::of(4096)}}),
      make("json", "JSON work: --lang json and the json tier-1 lint pack.",
           {{"lang", Json::of("json")}, {"pack", Json::of("rules/json-tier1.json")}}),
      make("c", "C work: --lang c and the C tier-1 lint pack with rule metadata.",
           {{"lang", Json::of("c")}, {"pack", Json::of("rules/c-tier1.json")}, {"metadata", Json::of(true)}}),
      make("python", "Python work: --lang python and the Python tier-1 lint pack.",
           {{"lang", Json::of("python")}, {"pack", Json::of("rules/python-tier1.json")}}),
      make("secrets", "Secret scanning: C secret-shaped literal pack.", {{"pack", Json::of("rules/secrets-c.json")}}),
  };
}

const std::vector<std::string> &languages() {
  static const std::vector<std::string> langs = {"json", "c", "python", "bash", "java", "go", "javascript", "typescript", "tsx", "lmscript", "lmcode"};
  return langs;
}

std::string extension_for(const std::string &lang) {
  static const std::map<std::string, std::string> ext = {
      {"json", ".json"}, {"c", ".c"}, {"python", ".py"}, {"bash", ".sh"}, {"java", ".java"}, {"go", ".go"},
      {"javascript", ".js"}, {"typescript", ".ts"}, {"tsx", ".tsx"}, {"lmscript", ".lms"}, {"lmcode", ".lmcode"}};
  auto it = ext.find(lang);
  return it == ext.end() ? "" : it->second;
}

// --------------------------------------------------------- the server -----

struct Result {
  std::string id, key, created;
  std::vector<std::string> command;
  int exit_code = 0;
  std::string out, err;
  bool cached = false, cacheable = false;
  long long duration_ms = 0;
};

Json text_field(Json &into, const char *name, const std::string &value) {
  if (valid_utf8(value)) into.set(name, Json::of(value));
  else into.set(std::string(name) + "_base64", Json::of(base64_encode(value)));
  return into;
}

Json result_json(const Result &r, bool full) {
  Json j = Json::object();
  j.set("id", Json::of(r.id));
  j.set("command", Json::strings(r.command));
  j.set("exit_code", Json::of(r.exit_code));
  j.set("ok", Json::of(r.exit_code == 0));
  j.set("cached", Json::of(r.cached));
  j.set("cacheable", Json::of(r.cacheable));
  if (!r.key.empty()) j.set("key", Json::of(r.key));
  j.set("duration_ms", Json::of(r.duration_ms));
  j.set("created", Json::of(r.created));
  if (full) {
    text_field(j, "stdout", r.out);
    text_field(j, "stderr", r.err);
  } else {
    j.set("stdout_bytes", Json::of(r.out.size()));
    j.set("stderr_bytes", Json::of(r.err.size()));
  }
  return j;
}

struct RunOptions {
  std::string stdin_data;
  std::optional<std::string> invocation_manifest; // after placeholder substitution
  std::optional<bool> cache;                       // per-request override
  std::string cwd;                                 // per-request override
};

class Server {
public:
  Server(CliEntry cli, fs::path start_dir, fs::path state_dir, std::vector<Preset> presets, std::string preset)
      : cli_(cli), start_dir_(std::move(start_dir)), state_dir_(std::move(state_dir)), presets_(std::move(presets)) {
    apply_preset(preset);
    startup_preset_ = preset;
    started_ms_ = now_ms();
    last_activity_ms_ = started_ms_;
  }

  const Json &settings() const { return settings_; }
  long long idle_timeout_ms() const { return setting_int(settings_, "idle_timeout_s") * 1000; }
  long long last_activity_ms() const { return last_activity_ms_; }
  bool stopping() const { return stopping_; }
  void touch() { last_activity_ms_ = now_ms(); }
  long long requests() const { return requests_; }

  void apply_preset(const std::string &name) {
    const Preset *p = find_preset(name);
    if (!p) throw HttpError(404, "unknown_preset", "unknown preset '" + name + "' (see GET /v1/presets)");
    Json s = default_settings();
    merge_settings(s, p->settings);
    settings_ = s;
    preset_ = name;
    trim_results();
    trim_cache();
  }

  const Preset *find_preset(const std::string &name) const {
    for (const auto &p : presets_)
      if (p.name == name) return &p;
    return nullptr;
  }

  Response handle(const Request &req) {
    ++requests_;
    try {
      return route(req);
    } catch (const HttpError &e) {
      return error_response(e.status, e.code, e.what());
    } catch (const JsonError &e) {
      return error_response(400, "invalid_json", e.what());
    } catch (const std::exception &e) {
      return error_response(500, "internal", e.what());
    }
  }

private:
  CliEntry cli_;
  fs::path start_dir_, state_dir_;
  std::vector<Preset> presets_;
  std::string preset_, startup_preset_;
  Json settings_;
  long long started_ms_ = 0, last_activity_ms_ = 0, requests_ = 0;
  bool stopping_ = false;
  unsigned long long next_id_ = 1;
  std::list<std::shared_ptr<Result>> results_; // newest at the back
  std::list<std::string> lru_;                 // most recently used at the front
  std::unordered_map<std::string, std::pair<std::shared_ptr<Result>, std::list<std::string>::iterator>> cache_;
  unsigned long long hits_ = 0, misses_ = 0, runs_ = 0;

  // ------------------------------------------------------------ routes --

  struct Route {
    const char *method, *path, *summary;
  };

  static const std::vector<Route> &routes() {
    static const std::vector<Route> table = {
        {"GET", "/", "This index: every action (\"button\") and selection list the server offers."},
        {"GET", "/v1/health", "Liveness, version, uptime, idle countdown and counters."},
        {"POST", "/v1/shutdown", "Stop the server after answering."},
        {"GET", "/v1/settings", "Current settings and the preset they came from."},
        {"PATCH", "/v1/settings", "Change some settings: body is a partial settings object."},
        {"PUT", "/v1/settings", "Replace all settings: defaults overlaid with the body."},
        {"POST", "/v1/settings/reset", "Back to the preset the server started with."},
        {"GET", "/v1/settings/schema", "Every setting with its type and description."},
        {"GET", "/v1/presets", "Selection: the presets to choose from."},
        {"GET", "/v1/presets/{name}", "One preset and the settings it applies."},
        {"POST", "/v1/presets/{name}", "Choose a preset: replaces the settings with it."},
        {"GET", "/v1/languages", "Selection: grammar names accepted as \"lang\"."},
        {"GET", "/v1/packs", "Selection: JSON rule packs under <cwd>/rules."},
        {"GET", "/v1/commands", "The command buttons and the body fields each one takes."},
        {"POST", "/v1/run", "Run any lmok command: {\"args\":[...], \"stdin\"?, \"stdin_from\"?, \"files\"?, \"cache\"?, \"cwd\"?}."},
        {"POST", "/v1/parse", "Parse text or a file: {\"text\"|\"file\", \"lang\"?}."},
        {"POST", "/v1/query", "Run a tree-sitter query: {\"pattern\", \"text\"|\"file\", \"lang\"?}."},
        {"POST", "/v1/lint", "Lint with a pack: {\"files\"|\"text\", \"pack\"?, \"lang\"?}."},
        {"POST", "/v1/facts", "Extract facts: {\"files\"|\"text\", \"lang\"?}."},
        {"POST", "/v1/metrics", "Local metrics: {\"files\"|\"text\", \"lang\"?}."},
        {"POST", "/v1/dup", "Token duplication: {\"files\"|\"text\", \"lang\"?, \"tokens\"?}."},
        {"POST", "/v1/outline", "Outline: {\"files\"|\"text\", \"budget\"?}."},
        {"POST", "/v1/rules", "Rule catalog for packs: {\"packs\"?} (defaults to the pack setting)."},
        {"POST", "/v1/lmcode", "Run compiled lmcode bytecode: {\"program_base64\"} or {\"programs\":[{\"base64\",\"order\"?}]}."},
        {"GET", "/v1/results", "Recent results, newest first (without output)."},
        {"DELETE", "/v1/results", "Forget all results."},
        {"GET", "/v1/results/{id}", "One result with its output."},
        {"GET", "/v1/results/{id}/stdout", "The raw stdout bytes of a result."},
        {"GET", "/v1/results/{id}/stderr", "The raw stderr bytes of a result."},
        {"DELETE", "/v1/results/{id}", "Forget one result."},
        {"GET", "/v1/cache", "Cache statistics and keys."},
        {"DELETE", "/v1/cache", "Empty the cache."},
        {"DELETE", "/v1/cache/{key}", "Drop one cache entry."},
    };
    return table;
  }

  Response method_not_allowed(const std::string &path) {
    std::string allow;
    for (const auto &r : routes()) {
      if (match(r.path, path)) { if (!allow.empty()) allow += ", "; allow += r.method; }
    }
    Response resp = error_response(405, "method_not_allowed", "allowed: " + allow);
    resp.headers.emplace_back("Allow", allow);
    return resp;
  }

  static std::vector<std::string> split_path(const std::string &path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
      if (c == '/') { if (!cur.empty()) out.push_back(percent_decode(cur)); cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) out.push_back(percent_decode(cur));
    return out;
  }

  static bool match(const std::string &pattern, const std::string &path) {
    auto a = split_path(pattern), b = split_path(path);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!(a[i].size() > 1 && a[i].front() == '{') && a[i] != b[i]) return false;
    return true;
  }

  static Json body_json(const Request &req) {
    if (trim(req.body).empty()) return Json::object();
    Json j = JsonParser(req.body).parse();
    if (j.type != Json::Object) bad_request("request body must be a JSON object");
    return j;
  }

  Response route(const Request &req) {
    const auto seg = split_path(req.path);
    const std::string &m = req.method;
    auto is = [&](std::initializer_list<const char *> parts) {
      if (seg.size() != parts.size()) return false;
      size_t i = 0;
      for (const char *p : parts) {
        if (std::strcmp(p, "*") != 0 && seg[i] != p) return false;
        ++i;
      }
      return true;
    };

    if (seg.empty()) return m == "GET" ? json_response(200, index()) : method_not_allowed(req.path);
    if (seg[0] != "v1") throw HttpError(404, "not_found", "no such endpoint: " + req.path + " (see GET /)");

    if (is({"v1", "health"})) { if (m != "GET") return method_not_allowed(req.path); return json_response(200, health()); }
    if (is({"v1", "shutdown"})) {
      if (m != "POST") return method_not_allowed(req.path);
      stopping_ = true;
      Json j = Json::object();
      j.set("ok", Json::of(true));
      j.set("stopping", Json::of(true));
      return json_response(200, j);
    }
    if (is({"v1", "settings"})) {
      if (m == "GET") return json_response(200, settings_view());
      if (m == "PATCH") { Json next = settings_; merge_settings(next, body_json(req)); settings_ = next; on_settings_changed(); return json_response(200, settings_view()); }
      if (m == "PUT") { Json next = default_settings(); merge_settings(next, body_json(req)); settings_ = next; preset_ = ""; on_settings_changed(); return json_response(200, settings_view()); }
      return method_not_allowed(req.path);
    }
    if (is({"v1", "settings", "reset"})) { if (m != "POST") return method_not_allowed(req.path); apply_preset(startup_preset_); return json_response(200, settings_view()); }
    if (is({"v1", "settings", "schema"})) { if (m != "GET") return method_not_allowed(req.path); return json_response(200, settings_schema()); }
    if (is({"v1", "presets"})) {
      if (m != "GET") return method_not_allowed(req.path);
      Json j = Json::object();
      j.set("current", Json::of(preset_));
      Json list = Json::array();
      for (const auto &p : presets_) list.push(preset_json(p));
      j.set("presets", list);
      return json_response(200, j);
    }
    if (is({"v1", "presets", "*"})) {
      const Preset *p = find_preset(seg[2]);
      if (!p) throw HttpError(404, "unknown_preset", "unknown preset '" + seg[2] + "' (see GET /v1/presets)");
      if (m == "GET") return json_response(200, preset_json(*p));
      if (m == "POST") { apply_preset(p->name); return json_response(200, settings_view()); }
      return method_not_allowed(req.path);
    }
    if (is({"v1", "languages"})) {
      if (m != "GET") return method_not_allowed(req.path);
      Json j = Json::object();
      j.set("default", Json::of(setting_str(settings_, "lang")));
      j.set("languages", Json::strings(languages()));
      return json_response(200, j);
    }
    if (is({"v1", "packs"})) { if (m != "GET") return method_not_allowed(req.path); return json_response(200, packs()); }
    if (is({"v1", "commands"})) { if (m != "GET") return method_not_allowed(req.path); return json_response(200, commands()); }
    if (is({"v1", "results"})) {
      if (m == "GET") {
        Json j = Json::object();
        Json list = Json::array();
        for (auto it = results_.rbegin(); it != results_.rend(); ++it) list.push(result_json(**it, false));
        j.set("results", list);
        return json_response(200, j);
      }
      if (m == "DELETE") { results_.clear(); Json j = Json::object(); j.set("ok", Json::of(true)); return json_response(200, j); }
      return method_not_allowed(req.path);
    }
    if (is({"v1", "results", "*"})) {
      auto r = find_result(seg[2]);
      if (m == "GET") return json_response(200, result_json(*r, true));
      if (m == "DELETE") {
        results_.remove(r);
        Json j = Json::object();
        j.set("ok", Json::of(true));
        return json_response(200, j);
      }
      return method_not_allowed(req.path);
    }
    if (is({"v1", "results", "*", "*"}) && (seg[3] == "stdout" || seg[3] == "stderr")) {
      if (m != "GET") return method_not_allowed(req.path);
      auto r = find_result(seg[2]);
      Response resp;
      resp.content_type = "application/octet-stream";
      resp.body = seg[3] == "stdout" ? r->out : r->err;
      resp.headers.emplace_back("X-Lmok-Exit-Code", std::to_string(r->exit_code));
      return resp;
    }
    if (is({"v1", "cache"})) {
      if (m == "GET") return json_response(200, cache_view());
      if (m == "DELETE") { cache_.clear(); lru_.clear(); return json_response(200, cache_view()); }
      return method_not_allowed(req.path);
    }
    if (is({"v1", "cache", "*"})) {
      if (m != "DELETE") return method_not_allowed(req.path);
      auto it = cache_.find(seg[2]);
      if (it == cache_.end()) throw HttpError(404, "not_cached", "no cache entry " + seg[2]);
      lru_.erase(it->second.second);
      cache_.erase(it);
      return json_response(200, cache_view());
    }
    static const char *verbs[] = {"run", "parse", "query", "lint", "facts", "metrics", "dup", "outline", "rules", "lmcode"};
    for (const char *verb : verbs) {
      if (is({"v1", verb})) {
        if (m != "POST") return method_not_allowed(req.path);
        return command(verb, body_json(req));
      }
    }
    throw HttpError(404, "not_found", "no such endpoint: " + req.path + " (see GET /)");
  }

  // ----------------------------------------------------------- views ----

  Json index() const {
    Json j = Json::object();
    j.set("name", Json::of("lmok"));
    j.set("version", Json::of(LMOK_VERSION));
    j.set("api", Json::of("v1"));
    j.set("preset", Json::of(preset_));
    Json buttons = Json::array();
    for (const auto &r : routes()) {
      Json b = Json::object();
      b.set("method", Json::of(r.method));
      b.set("path", Json::of(r.path));
      b.set("summary", Json::of(r.summary));
      buttons.push(b);
    }
    j.set("buttons", buttons);
    Json selections = Json::object();
    selections.set("presets", Json::of("/v1/presets"));
    selections.set("languages", Json::of("/v1/languages"));
    selections.set("packs", Json::of("/v1/packs"));
    selections.set("settings", Json::of("/v1/settings/schema"));
    j.set("selections", selections);
    return j;
  }

  Json health() const {
    Json j = Json::object();
    j.set("ok", Json::of(true));
    j.set("version", Json::of(LMOK_VERSION));
    j.set("preset", Json::of(preset_));
    j.set("uptime_ms", Json::of(now_ms() - started_ms_));
    long long idle = idle_timeout_ms();
    j.set("idle_timeout_s", Json::of(setting_int(settings_, "idle_timeout_s")));
    if (idle > 0) j.set("idle_remaining_ms", Json::of(std::max(0LL, idle - (now_ms() - last_activity_ms_))));
    j.set("requests", Json::of(requests_));
    j.set("runs", Json::of(static_cast<long long>(runs_)));
    j.set("results", Json::of(results_.size()));
    j.set("cache_entries", Json::of(cache_.size()));
    return j;
  }

  Json settings_view() const {
    Json j = Json::object();
    j.set("preset", Json::of(preset_));
    j.set("settings", settings_);
    j.set("effective_cwd", Json::of(effective_cwd("").generic_u8string()));
    return j;
  }

  Json settings_schema() const {
    Json j = Json::object();
    Json list = Json::array();
    Json defaults = default_settings();
    for (const auto &s : setting_specs()) {
      Json e = Json::object();
      e.set("name", Json::of(s.name));
      e.set("type", Json::of(s.type == Json::Bool ? "boolean" : s.type == Json::Number ? "integer" : "string"));
      e.set("default", *defaults.get(s.name));
      e.set("description", Json::of(s.description));
      list.push(e);
    }
    j.set("settings", list);
    return j;
  }

  static Json preset_json(const Preset &p) {
    Json j = Json::object();
    j.set("name", Json::of(p.name));
    j.set("description", Json::of(p.description));
    j.set("settings", p.settings);
    return j;
  }

  Json packs() const {
    Json j = Json::object();
    j.set("default", Json::of(setting_str(settings_, "pack")));
    Json list = Json::array();
    std::error_code ec;
    fs::path dir = effective_cwd("") / "rules";
    std::vector<std::string> names;
    if (fs::is_directory(dir, ec)) {
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".json") names.push_back("rules/" + it->path().filename().generic_u8string());
      }
    }
    std::sort(names.begin(), names.end());
    j.set("packs", Json::strings(names));
    return j;
  }

  static Json commands() {
    Json j = Json::object();
    auto add = [&](const char *verb, const char *fields, const char *args) {
      Json c = Json::object();
      c.set("method", Json::of("POST"));
      c.set("path", Json::of(std::string("/v1/") + verb));
      c.set("body", Json::of(fields));
      c.set("runs", Json::of(args));
      j.set(verb, c);
    };
    add("run", "args (required), stdin, stdin_from, files, invocation_manifest, cache, cwd", "lmok ARGS...");
    add("parse", "text+lang|text+name|file, lang, cache, cwd", "lmok parse [--lang L] FILE");
    add("query", "pattern (required), text|file, lang, cache, cwd", "lmok query [--lang L] PATTERN FILE");
    add("lint", "files|file|text, pack, lang, metadata, cache, cwd", "lmok lint PACK FILE... [--metadata]");
    add("facts", "files|file|text, lang, cache, cwd", "lmok facts [--lang L] FILE...");
    add("metrics", "files|file|text, lang, cache, cwd", "lmok metrics [--lang L] FILE...");
    add("dup", "files|file|text, lang, tokens, cache, cwd", "lmok dup [--lang L] [--tokens N] FILE...");
    add("outline", "files|file|text, budget, cache, cwd", "lmok outline [--budget N] FILE...");
    add("rules", "packs, cache, cwd", "lmok rules PACK...");
    add("lmcode", "program_base64 | programs[{base64, order: first|normal|last}], db, metadata, cache, cwd", "lmok --lmcode FILE...");
    return j;
  }

  Json cache_view() const {
    Json j = Json::object();
    j.set("enabled", Json::of(setting_bool(settings_, "cache")));
    j.set("entries", Json::of(cache_.size()));
    j.set("max_entries", Json::of(setting_int(settings_, "cache_max_entries")));
    j.set("hits", Json::of(static_cast<long long>(hits_)));
    j.set("misses", Json::of(static_cast<long long>(misses_)));
    Json keys = Json::array();
    for (const auto &k : lru_) {
      const auto &r = cache_.at(k).first;
      Json e = Json::object();
      e.set("key", Json::of(k));
      e.set("command", Json::strings(r->command));
      e.set("exit_code", Json::of(r->exit_code));
      keys.push(e);
    }
    j.set("keys", keys);
    return j;
  }

  std::shared_ptr<Result> find_result(const std::string &id) {
    for (auto &r : results_)
      if (r->id == id) return r;
    throw HttpError(404, "unknown_result", "no result " + id + " (results expire after results_max newer ones)");
  }

  void on_settings_changed() { trim_results(); trim_cache(); }

  void trim_results() {
    size_t max = size_t(std::max(1LL, setting_int(settings_, "results_max")));
    while (results_.size() > max) results_.pop_front();
  }

  void trim_cache() {
    size_t max = size_t(setting_int(settings_, "cache_max_entries"));
    while (cache_.size() > max) {
      cache_.erase(lru_.back());
      lru_.pop_back();
    }
  }

  fs::path effective_cwd(const std::string &request_cwd) const {
    std::string c = !request_cwd.empty() ? request_cwd : setting_str(settings_, "cwd");
    if (c.empty()) return start_dir_;
    fs::path p = fs::u8path(c);
    return p.is_absolute() ? p : start_dir_ / p;
  }

  // ------------------------------------------------------- uploads ------

  static void check_upload_name(const std::string &name) {
    if (name.empty() || name.size() > 200 || name == "." || name == ".." || name.find_first_of("/\\:\0", 0, 4) != std::string::npos)
      bad_request("invalid upload name '" + name + "': use a plain file name");
  }

  // Stores bytes under a content-addressed directory and returns its path.
  fs::path store_upload(const std::string &name, const std::string &bytes) {
    check_upload_name(name);
    fs::path dir = state_dir_ / "uploads" / hex_sha256(name + '\0' + bytes).substr(0, 24);
    fs::path file = dir / fs::u8path(name);
    std::error_code ec;
    if (!fs::exists(file, ec)) {
      fs::create_directories(dir, ec);
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out << bytes;
      if (!out) throw HttpError(500, "upload_failed", "cannot store upload " + name);
    }
    return file;
  }

  // ------------------------------------------------------- commands -----

  static std::optional<std::string> opt_string(const Json &body, const char *key) {
    const Json *v = body.get(key);
    if (!v || v->type == Json::Null) return std::nullopt;
    if (v->type != Json::String) bad_request(std::string("\"") + key + "\" must be a string");
    return v->text;
  }

  static std::optional<long long> opt_int(const Json &body, const char *key) {
    const Json *v = body.get(key);
    if (!v || v->type == Json::Null) return std::nullopt;
    if (v->type != Json::Number || !v->integral) bad_request(std::string("\"") + key + "\" must be an integer");
    return v->integer;
  }

  static std::optional<bool> opt_bool(const Json &body, const char *key) {
    const Json *v = body.get(key);
    if (!v || v->type == Json::Null) return std::nullopt;
    if (v->type != Json::Bool) bad_request(std::string("\"") + key + "\" must be a boolean");
    return v->boolean;
  }

  static std::vector<std::string> string_list(const Json &body, const char *key) {
    const Json *v = body.get(key);
    std::vector<std::string> out;
    if (!v || v->type == Json::Null) return out;
    if (v->type != Json::Array) bad_request(std::string("\"") + key + "\" must be an array of strings");
    for (const auto &item : v->items) {
      if (item.type != Json::String) bad_request(std::string("\"") + key + "\" must be an array of strings");
      out.push_back(item.text);
    }
    return out;
  }

  std::string lang_for(const Json &body) const {
    auto l = opt_string(body, "lang");
    std::string lang = l ? *l : setting_str(settings_, "lang");
    if (!lang.empty() && std::find(languages().begin(), languages().end(), lang) == languages().end())
      throw HttpError(422, "unknown_language", "unknown lang '" + lang + "' (see GET /v1/languages)");
    return lang;
  }

  // Input files for a command: "files", "file" or inline "text".
  std::vector<std::string> inputs(const Json &body, const std::string &lang, bool single) {
    std::vector<std::string> files = string_list(body, "files");
    if (auto f = opt_string(body, "file")) files.push_back(*f);
    if (auto text = opt_string(body, "text")) {
      std::string name = opt_string(body, "name").value_or("");
      if (name.empty()) {
        if (lang.empty()) bad_request("\"text\" needs \"lang\" or \"name\" so the grammar can be chosen");
        name = "input" + extension_for(lang);
      }
      files.push_back(store_upload(name, *text).generic_u8string());
    }
    if (files.empty()) bad_request("give \"text\", \"file\" or \"files\"");
    if (single && files.size() != 1) bad_request("this command takes exactly one input");
    return files;
  }

  Response command(const std::string &verb, const Json &body) {
    RunOptions opts;
    opts.cache = opt_bool(body, "cache");
    opts.cwd = opt_string(body, "cwd").value_or("");
    std::vector<std::string> args;
    const bool metadata = opt_bool(body, "metadata").value_or(setting_bool(settings_, "metadata"));

    if (verb == "run") {
      args = string_list(body, "args");
      if (args.empty()) bad_request("\"args\" must be a non-empty array of strings");
      if (args[0] == "serve" || args[0] == "remote") throw HttpError(422, "not_allowed", "\"" + args[0] + "\" cannot run inside the server");
      std::map<std::string, std::string> placeholders;
      if (const Json *files = body.get("files")) {
        if (files->type != Json::Object) bad_request("\"files\" must be an object of name -> {text|base64}");
        for (const auto &f : files->fields) {
          std::string bytes;
          if (f.second.type == Json::String) bytes = f.second.text;
          else if (auto t = opt_string(f.second, "text")) bytes = *t;
          else if (auto b = opt_string(f.second, "base64")) {
            auto decoded = base64_decode(*b);
            if (!decoded) bad_request("file '" + f.first + "' is not valid base64");
            bytes = *decoded;
          } else bad_request("file '" + f.first + "' needs \"text\" or \"base64\"");
          placeholders["@{" + f.first + "}"] = store_upload(f.first, bytes).generic_u8string();
        }
      }
      auto substitute = [&](std::string s, bool json_context) {
        for (const auto &p : placeholders) {
          std::string value = p.second;
          if (json_context) { std::string q; dump_string(q, value); value = q.substr(1, q.size() - 2); }
          for (size_t at = s.find(p.first); at != std::string::npos; at = s.find(p.first, at + value.size())) s.replace(at, p.first.size(), value);
        }
        return s;
      };
      for (auto &a : args) a = substitute(a, false);
      if (auto manifest = opt_string(body, "invocation_manifest")) opts.invocation_manifest = substitute(*manifest, true);
      if (auto in = opt_string(body, "stdin")) opts.stdin_data = *in;
      else if (auto b = opt_string(body, "stdin_base64")) {
        auto decoded = base64_decode(*b);
        if (!decoded) bad_request("\"stdin_base64\" is not valid base64");
        opts.stdin_data = *decoded;
      } else if (auto from = opt_string(body, "stdin_from")) opts.stdin_data = find_result(*from)->out;
      args.insert(args.begin(), "lmok");
    } else if (verb == "parse" || verb == "query") {
      std::string lang = lang_for(body);
      args = {"lmok", verb};
      if (!lang.empty()) { args.push_back("--lang"); args.push_back(lang); }
      if (verb == "query") {
        auto pattern = opt_string(body, "pattern");
        if (!pattern || pattern->empty()) bad_request("\"pattern\" is required");
        args.push_back(*pattern);
      }
      args.push_back(inputs(body, lang, true).front());
    } else if (verb == "lint") {
      std::string pack = opt_string(body, "pack").value_or(setting_str(settings_, "pack"));
      if (pack.empty()) bad_request("no rule pack: give \"pack\" or choose a preset/setting with one (GET /v1/packs)");
      std::string lang = lang_for(body);
      args = {"lmok", "lint", pack};
      for (auto &f : inputs(body, lang, false)) args.push_back(f);
      if (metadata) args.push_back("--metadata");
    } else if (verb == "facts" || verb == "metrics" || verb == "dup") {
      std::string lang = lang_for(body);
      args = {"lmok", verb};
      if (!lang.empty()) { args.push_back("--lang"); args.push_back(lang); }
      if (verb == "dup") {
        if (auto t = opt_int(body, "tokens")) { args.push_back("--tokens"); args.push_back(std::to_string(*t)); }
      }
      for (auto &f : inputs(body, lang, false)) args.push_back(f);
    } else if (verb == "outline") {
      std::string lang = lang_for(body);
      args = {"lmok", "outline"};
      for (auto &f : inputs(body, lang, false)) args.push_back(f);
      if (auto b = opt_int(body, "budget")) { args.push_back("--budget"); args.push_back(std::to_string(*b)); }
    } else if (verb == "rules") {
      auto packs = string_list(body, "packs");
      if (packs.empty()) {
        std::string pack = setting_str(settings_, "pack");
        if (pack.empty()) bad_request("give \"packs\" or set a default pack");
        packs.push_back(pack);
      }
      args = {"lmok", "rules"};
      for (auto &p : packs) args.push_back(p);
    } else if (verb == "lmcode") {
      std::vector<std::pair<std::string, std::string>> programs; // order flag, bytes
      if (auto b = opt_string(body, "program_base64")) {
        auto decoded = base64_decode(*b);
        if (!decoded) bad_request("\"program_base64\" is not valid base64");
        programs.emplace_back("--lmcode", *decoded);
      }
      if (const Json *list = body.get("programs")) {
        if (list->type != Json::Array) bad_request("\"programs\" must be an array");
        for (const auto &p : list->items) {
          auto b = p.type == Json::Object ? opt_string(p, "base64") : std::nullopt;
          if (!b) bad_request("each program needs \"base64\"");
          auto decoded = base64_decode(*b);
          if (!decoded) bad_request("a program is not valid base64");
          std::string order = opt_string(p, "order").value_or("normal");
          if (order != "first" && order != "normal" && order != "last") bad_request("\"order\" is first, normal or last");
          programs.emplace_back(order == "first" ? "--first" : order == "last" ? "--last" : "--lmcode", *decoded);
        }
      }
      if (programs.empty()) bad_request("give \"program_base64\" or \"programs\"");
      args = {"lmok"};
      for (size_t i = 0; i < programs.size(); ++i) {
        args.push_back(programs[i].first);
        args.push_back(store_upload("program-" + std::to_string(i) + ".dslb", programs[i].second).generic_u8string());
      }
      if (auto db = opt_string(body, "db")) { args.push_back("--db"); args.push_back(*db); }
      if (metadata) args.push_back("--metadata");
    }
    auto result = execute(args, opts);
    return json_response(200, result_json(*result, true));
  }

  // ------------------------------------------------------- execution ----

  // Commands whose output depends only on their arguments and input bytes.
  bool cacheable(const std::vector<std::string> &args) const {
    if (args.size() < 2) return false;
    const std::string &verb = args[1];
    auto has = [&](const std::string &flag) {
      for (size_t i = 2; i < args.size(); ++i)
        if (args[i] == flag || args[i].rfind(flag + "=", 0) == 0) return true;
      return false;
    };
    if (verb == "--version" || verb == "--notices" || verb == "--lmcode-schema" || verb == "--lmcode-signatures") return true;
    if (verb == "parse" || verb == "query" || verb == "facts" || verb == "metrics" || verb == "dup" || verb == "rules" || verb == "test") return true;
    if (verb == "outline") return !has("--db");
    if (verb == "lint") return !has("--db") && !has("--fix") && !has("--diff") && !has("--sarif") && !has("--changed") && !has("--new-since") && !has("--baseline");
    if (verb == "check") return !has("--sarif");
    if (verb == "--lmcode" || verb == "--first" || verb == "--last") return setting_bool(settings_, "cache_lmcode") && !has("--db");
    return false;
  }

  // Folds the bytes of every argument that names an existing file (and the
  // size/mtime of every file under a named directory) into the cache key.
  static bool fingerprint(const fs::path &cwd, const std::vector<std::string> &args, std::string &out) {
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i].empty() || args[i] == "-" || args[i].size() > 4096) continue;
      std::error_code ec;
      fs::path p = fs::u8path(args[i]);
      if (!p.is_absolute()) p = cwd / p;
      auto st = fs::status(p, ec);
      if (ec) continue;
      if (fs::is_regular_file(st)) {
        std::string bytes;
        if (!read_whole(p, bytes)) return false;
        out += "F" + std::to_string(i) + ":" + hex_sha256(bytes) + "\n";
      } else if (fs::is_directory(st)) {
        std::vector<std::string> lines;
        size_t count = 0;
        for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
          if (++count > 20000) return false;
          std::error_code e2;
          if (!it->is_regular_file(e2)) continue;
          auto size = it->file_size(e2);
          auto mtime = it->last_write_time(e2).time_since_epoch().count();
          lines.push_back(it->path().generic_u8string() + "|" + std::to_string(size) + "|" + std::to_string(mtime));
        }
        if (ec) return false;
        std::sort(lines.begin(), lines.end());
        std::string all;
        for (auto &l : lines) all += l + "\n";
        out += "D" + std::to_string(i) + ":" + hex_sha256(all) + "\n";
      }
    }
    return true;
  }

  std::shared_ptr<Result> execute(const std::vector<std::string> &args, const RunOptions &opts) {
    fs::path cwd = effective_cwd(opts.cwd);
    std::error_code ec;
    if (!fs::is_directory(cwd, ec)) throw HttpError(422, "invalid_cwd", "working directory does not exist: " + cwd.generic_u8string());

    auto result = std::make_shared<Result>();
    result->id = "r" + std::to_string(next_id_++);
    result->created = utc_now();
    result->command.assign(args.begin() + 1, args.end());

    bool use_cache = opts.cache.value_or(setting_bool(settings_, "cache")) && setting_int(settings_, "cache_max_entries") > 0;
    result->cacheable = cacheable(args);
    std::string key;
    if (result->cacheable) {
      std::string material = "lmok " LMOK_VERSION "\n" + cwd.generic_u8string() + "\n";
      for (const auto &a : args) material += a + '\0';
      material += "\nstdin:" + hex_sha256(opts.stdin_data) + "\n";
      if (opts.invocation_manifest) material += "manifest:" + hex_sha256(*opts.invocation_manifest) + "\n";
      if (fingerprint(cwd, args, material)) key = hex_sha256(material);
      else result->cacheable = false;
    }
    result->key = key;

    if (use_cache && !key.empty()) {
      auto hit = cache_.find(key);
      if (hit != cache_.end()) {
        ++hits_;
        lru_.splice(lru_.begin(), lru_, hit->second.second);
        const Result &cached = *hit->second.first;
        result->exit_code = cached.exit_code;
        result->out = cached.out;
        result->err = cached.err;
        result->cached = true;
        remember(result);
        return result;
      }
      ++misses_;
    }

    long long start = now_ms();
    run_in_process(args, opts, cwd, *result);
    result->duration_ms = now_ms() - start;
    ++runs_;

    if (use_cache && !key.empty()) {
      lru_.push_front(key);
      cache_[key] = {result, lru_.begin()};
      trim_cache();
    }
    remember(result);
    return result;
  }

  void remember(const std::shared_ptr<Result> &r) {
    results_.push_back(r);
    trim_results();
  }

  // Runs one lmok command in this process, capturing its standard streams.
  void run_in_process(const std::vector<std::string> &args, const RunOptions &opts, const fs::path &cwd, Result &result) {
    std::vector<std::string> storage(args);
    std::vector<char *> argv;
    for (auto &s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::optional<fs::path> manifest_file;
    if (opts.invocation_manifest) {
      manifest_file = state_dir_ / ("invocation-" + result.id + ".json");
      std::ofstream m(*manifest_file, std::ios::binary | std::ios::trunc);
      m << *opts.invocation_manifest;
    }
    const auto previous_env = get_env(INVOCATION_ENV);
    set_env(INVOCATION_ENV, manifest_file ? std::optional<std::string>(fs::absolute(*manifest_file).u8string()) : std::nullopt);

    std::error_code ec;
    fs::path previous_cwd = fs::current_path(ec);
    fs::current_path(cwd, ec);

    std::cout.flush();
    std::cerr.flush();
    std::ostringstream out, err;
    std::istringstream in(opts.stdin_data);
    std::streambuf *old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf *old_err = std::cerr.rdbuf(err.rdbuf());
    std::streambuf *old_in = std::cin.rdbuf(in.rdbuf());
    int code = 4;
    try {
      code = cli_(int(argv.size() - 1), argv.data());
    } catch (const std::exception &e) {
      std::cerr << "lmok serve: command raised: " << e.what() << "\n";
      code = 4;
    } catch (...) {
      std::cerr << "lmok serve: command raised an unknown exception\n";
      code = 4;
    }
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    std::cin.rdbuf(old_in);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

    if (!previous_cwd.empty()) fs::current_path(previous_cwd, ec);
    set_env(INVOCATION_ENV, previous_env);
    if (manifest_file) fs::remove(*manifest_file, ec);

    result.exit_code = code;
    result.out = out.str();
    result.err = err.str();
  }
};

// ------------------------------------------------------------- startup ----

std::vector<Preset> load_presets_file(const std::string &path, std::vector<Preset> presets) {
  std::string raw;
  if (!read_whole(fs::u8path(path), raw)) throw std::runtime_error("cannot read presets file " + path);
  Json root = JsonParser(raw).parse();
  const Json *list = root.get("presets");
  if (root.type != Json::Object || !list || list->type != Json::Array) throw std::runtime_error("presets file must be {\"presets\": [{\"name\", \"description\"?, \"settings\"}]}");
  for (const auto &p : list->items) {
    const Json *name = p.get("name"), *settings = p.get("settings"), *desc = p.get("description");
    if (!name || name->type != Json::String || name->text.empty()) throw std::runtime_error("every preset needs a \"name\"");
    if (!settings || settings->type != Json::Object) throw std::runtime_error("preset " + name->text + " needs a \"settings\" object");
    Json check = default_settings();
    try { merge_settings(check, *settings); } catch (const HttpError &e) { throw std::runtime_error("preset " + name->text + ": " + e.what()); }
    Preset preset{name->text, desc && desc->type == Json::String ? desc->text : "", *settings};
    auto same = std::find_if(presets.begin(), presets.end(), [&](const Preset &q) { return q.name == preset.name; });
    if (same != presets.end()) *same = preset;
    else presets.push_back(preset);
  }
  return presets;
}

const char *SERVE_USAGE =
    "usage: lmok serve [--host ADDR] [--port N] [--preset NAME] [--presets FILE]\n"
    "                  [--idle-timeout SECONDS] [--port-file FILE] [--state-dir DIR]\n"
    "       lmok serve --list-presets\n"
    "Starts a REST server (default http://127.0.0.1:7700). GET / lists every action.\n";

} // namespace

int serve_main(const std::vector<std::string> &args, CliEntry cli) {
  std::string host = "127.0.0.1", preset = "default", port_file, state_dir_arg, presets_file;
  long port = 7700;
  std::optional<long long> idle_override;
  bool list_presets = false;
  for (size_t i = 2; i < args.size(); ++i) {
    const std::string &a = args[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) { std::cerr << a << " needs a value\n" << SERVE_USAGE; std::exit(2); }
      return args[++i];
    };
    try {
      if (a == "--host") host = value();
      else if (a == "--port") port = std::stol(value());
      else if (a == "--preset") preset = value();
      else if (a == "--presets") presets_file = value();
      else if (a == "--idle-timeout") idle_override = std::stoll(value());
      else if (a == "--port-file") port_file = value();
      else if (a == "--state-dir") state_dir_arg = value();
      else if (a == "--list-presets") list_presets = true;
      else if (a == "--help" || a == "-h") { std::cout << SERVE_USAGE; return 0; }
      else { std::cerr << "unknown option " << a << "\n" << SERVE_USAGE; return 2; }
    } catch (const std::logic_error &) {
      std::cerr << "invalid number for " << a << "\n" << SERVE_USAGE;
      return 2;
    }
  }
  if (port < 0 || port > 65535 || (idle_override && *idle_override < 0)) { std::cerr << SERVE_USAGE; return 2; }

  std::vector<Preset> presets = builtin_presets();
  if (!presets_file.empty()) {
    try { presets = load_presets_file(presets_file, presets); }
    catch (const std::exception &e) { std::cerr << "lmok serve: " << e.what() << "\n"; return 2; }
  }
  if (list_presets) {
    for (const auto &p : presets) std::cout << p.name << "\t" << p.description << "\n";
    return 0;
  }

  std::error_code ec;
  fs::path start_dir = fs::current_path(ec);
  fs::path state_dir = state_dir_arg.empty() ? fs::temp_directory_path(ec) / ("lmok-serve-" + std::to_string(process_id())) : fs::absolute(fs::u8path(state_dir_arg), ec);
  fs::create_directories(state_dir, ec);
  if (!fs::is_directory(state_dir, ec)) { std::cerr << "lmok serve: cannot create state dir " << state_dir.u8string() << "\n"; return 2; }

  if (idle_override) // --idle-timeout overrides the chosen preset's value
    for (auto &p : presets)
      if (p.name == preset) p.settings.set("idle_timeout_s", Json::of(*idle_override));
  std::unique_ptr<Server> server;
  try {
    server = std::make_unique<Server>(cli, start_dir, state_dir, presets, preset);
  } catch (const HttpError &e) {
    std::cerr << "lmok serve: " << e.what() << "\n";
    return 2;
  }

  NetInit net;
  in_addr addr{};
  if (!resolve_ipv4(host, addr)) { std::cerr << "lmok serve: --host must be an IPv4 address or localhost\n"; return 2; }
  socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener == BAD_SOCKET) { std::cerr << "lmok serve: cannot create socket\n"; return 3; }
#ifndef _WIN32
  int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(port));
  sa.sin_addr = addr;
  if (::bind(listener, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0 || ::listen(listener, 16) != 0) {
    std::cerr << "lmok serve: cannot listen on " << host << ":" << port << "\n";
    close_socket(listener);
    return 3;
  }
  socklen_t len = sizeof sa;
  getsockname(listener, reinterpret_cast<sockaddr *>(&sa), &len);
  const int bound_port = ntohs(sa.sin_port);
  if (!port_file.empty()) {
    std::ofstream pf(fs::u8path(port_file), std::ios::trunc);
    pf << bound_port << "\n";
  }
  std::cout << "lmok serve: listening on http://" << host << ":" << bound_port << " (preset " << preset << ", idle timeout "
            << setting_int(server->settings(), "idle_timeout_s") << "s)" << std::endl;

  std::string reason = "shutdown requested";
  while (!server->stopping()) {
    long long idle = server->idle_timeout_ms();
    long long wait_ms = 1000;
    if (idle > 0) {
      long long remaining = idle - (now_ms() - server->last_activity_ms());
      if (remaining <= 0) { reason = "idle timeout"; break; }
      wait_ms = std::min(wait_ms, remaining);
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval tv{};
    tv.tv_sec = long(wait_ms / 1000);
    tv.tv_usec = long((wait_ms % 1000) * 1000);
    int ready = ::select(static_cast<int>(listener) + 1, &set, nullptr, nullptr, &tv);
    if (ready <= 0) continue;
    socket_t client = ::accept(listener, nullptr, nullptr);
    if (client == BAD_SOCKET) continue;
    set_timeouts(client, 30);
    Request req;
    int status = read_request(client, req, size_t(setting_int(server->settings(), "max_body_bytes")));
    if (status == 0) {
      Response resp = server->handle(req);
      write_response(client, resp);
      std::cerr << "lmok serve: " << req.method << " " << req.path << " -> " << resp.status << "\n";
    } else if (status > 0) {
      write_response(client, error_response(status, "protocol", "malformed or unsupported request"));
    }
#ifdef _WIN32
    shutdown(client, SD_SEND);
#else
    shutdown(client, SHUT_WR);
#endif
    close_socket(client);
    server->touch();
  }
  close_socket(listener);
  fs::remove_all(state_dir / "uploads", ec);
  if (state_dir_arg.empty()) fs::remove_all(state_dir, ec);
  std::cout << "lmok serve: stopped (" << reason << ")" << std::endl;
  return 0;
}

// --------------------------------------------------------------- client ---

namespace {

struct Url {
  std::string host = "127.0.0.1";
  int port = 7700;
};

bool parse_url(const std::string &raw, Url &url) {
  std::string s = raw;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  else if (s.find("://") != std::string::npos) return false;
  while (!s.empty() && s.back() == '/') s.pop_back();
  size_t colon = s.rfind(':');
  if (colon == std::string::npos) { url.host = s; url.port = 80; }
  else {
    url.host = s.substr(0, colon);
    try { url.port = std::stoi(s.substr(colon + 1)); } catch (...) { return false; }
  }
  return !url.host.empty() && url.port > 0 && url.port < 65536;
}

struct HttpReply {
  int status = 0;
  std::string body;
  std::string error;
};

HttpReply http_call(const Url &url, const std::string &method, const std::string &path, const std::string &body) {
  HttpReply reply;
  in_addr addr{};
  if (!resolve_ipv4(url.host, addr)) { reply.error = "server host must be an IPv4 address or localhost"; return reply; }
  socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == BAD_SOCKET) { reply.error = "cannot create socket"; return reply; }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(url.port));
  sa.sin_addr = addr;
  if (::connect(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) {
    close_socket(s);
    reply.error = "cannot connect to http://" + url.host + ":" + std::to_string(url.port);
    return reply;
  }
  set_timeouts(s, 600);
  std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                    "\r\nUser-Agent: lmok-remote/" LMOK_VERSION "\r\nAccept: application/json\r\nConnection: close\r\n";
  if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH")
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  if (!send_all(s, req)) { close_socket(s); reply.error = "cannot send request"; return reply; }
  std::string raw;
  char buf[16384];
  while (true) {
    long n = recv_some(s, buf, sizeof buf);
    if (n <= 0) break;
    raw.append(buf, size_t(n));
  }
  close_socket(s);
  size_t end = raw.find("\r\n\r\n");
  if (raw.rfind("HTTP/1.", 0) != 0 || end == std::string::npos) { reply.error = "malformed response from server"; return reply; }
  reply.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
  reply.body = raw.substr(end + 4);
  return reply;
}

const char *REMOTE_USAGE =
    "usage: lmok remote [--server URL] [--no-cache] [--json] [--] LMOK-ARGS...\n"
    "       lmok remote [--server URL] --api METHOD PATH [JSON-BODY]\n"
    "Runs one lmok command on a `lmok serve` server and replays its stdout, stderr\n"
    "and exit code. URL defaults to $LMOK_SERVER, then http://127.0.0.1:7700.\n"
    "--lmcode/--first/--last programs, `check` bytecode and a `-` stdin are uploaded;\n"
    "other paths are resolved by the server relative to this directory.\n";

} // namespace

int remote_main(const std::vector<std::string> &args) {
  std::string server = get_env("LMOK_SERVER").value_or("http://127.0.0.1:7700");
  bool no_cache = false, raw_json = false;
  size_t i = 2;
  std::vector<std::string> api;
  for (; i < args.size(); ++i) {
    const std::string &a = args[i];
    if (a == "--server" && i + 1 < args.size()) server = args[++i];
    else if (a == "--no-cache") no_cache = true;
    else if (a == "--json") raw_json = true;
    else if (a == "--api") {
      api.assign(args.begin() + long(i) + 1, args.end());
      if (api.size() < 2 || api.size() > 3) { std::cerr << REMOTE_USAGE; return 2; }
      i = args.size();
      break;
    } else if (a == "--help" || a == "-h") { std::cout << REMOTE_USAGE; return 0; }
    else if (a == "--") { ++i; break; }
    else break;
  }
  Url url;
  if (!parse_url(server, url)) { std::cerr << "lmok remote: unsupported server URL " << server << " (use http://HOST:PORT)\n"; return 2; }
  NetInit net;

  if (!api.empty()) {
    HttpReply reply = http_call(url, api[0], api[1], api.size() > 2 ? api[2] : "");
    if (!reply.error.empty()) { std::cerr << "lmok remote: " << reply.error << "\n"; return 2; }
    std::cout << reply.body;
    if (!reply.body.empty() && reply.body.back() != '\n') std::cout << "\n";
    return reply.status >= 200 && reply.status < 300 ? 0 : 1;
  }

  std::vector<std::string> cmd(args.begin() + long(i), args.end());
  if (cmd.empty()) { std::cerr << REMOTE_USAGE; return 2; }

  Json body = Json::object();
  Json files = Json::object();
  std::map<std::string, std::string> uploaded; // local path -> placeholder
  int counter = 0;
  // Manifests name programs by absolute path, possibly with other separators.
  auto path_key = [](const std::string &p) {
    std::error_code ec;
    return fs::absolute(fs::u8path(p), ec).lexically_normal().generic_u8string();
  };
  auto upload = [&](std::string &arg) -> bool {
    auto known = uploaded.find(arg);
    if (known != uploaded.end()) { arg = known->second; return true; }
    std::string bytes;
    if (!read_whole(fs::u8path(arg), bytes)) { std::cerr << "lmok remote: cannot read " << arg << "\n"; return false; }
    std::string name = "u" + std::to_string(counter++) + "-" + fs::u8path(arg).filename().u8string();
    Json f = Json::object();
    f.set("base64", Json::of(base64_encode(bytes)));
    files.set(name, f);
    std::string placeholder = "@{" + name + "}";
    uploaded[arg] = placeholder;
    uploaded[path_key(arg)] = placeholder;
    arg = placeholder;
    return true;
  };
  const bool lmcode_verb = cmd[0] == "--lmcode" || cmd[0] == "--first" || cmd[0] == "--last";
  bool reads_stdin = false;
  for (size_t k = 0; k < cmd.size(); ++k) {
    if (lmcode_verb && (cmd[k] == "--lmcode" || cmd[k] == "--first" || cmd[k] == "--last") && k + 1 < cmd.size()) {
      if (!upload(cmd[++k])) return 2;
    } else if (cmd[k] == "-") reads_stdin = true;
  }
  if (cmd[0] == "check") { // the first positional after options is the bytecode
    for (size_t k = 1; k < cmd.size(); ++k) {
      if (cmd[k] == "--lang" || cmd[k] == "--sarif") { ++k; continue; }
      if (cmd[k].rfind("--", 0) == 0) continue;
      if (!upload(cmd[k])) return 2;
      break;
    }
  }
  // run_lmcode passes a manifest naming the programs by absolute path; send it
  // with the same placeholders so the server can point it at its uploads.
  if (auto manifest_path = get_env(INVOCATION_ENV)) {
    std::string manifest;
    if (!read_whole(fs::u8path(*manifest_path), manifest)) { std::cerr << "lmok remote: cannot read " << INVOCATION_ENV << "\n"; return 2; }
    try {
      Json m = JsonParser(manifest).parse();
      if (Json *parts = const_cast<Json *>(m.get("parts")); parts && parts->type == Json::Array) {
        for (auto &part : parts->items) {
          Json *file = const_cast<Json *>(part.get("file"));
          if (file && file->type == Json::String) {
            auto it = uploaded.find(path_key(file->text));
            if (it != uploaded.end()) file->text = it->second;
          }
        }
      }
      body.set("invocation_manifest", Json::of(to_json(m, 0)));
    } catch (const JsonError &) {
      body.set("invocation_manifest", Json::of(manifest)); // let the server report it
    }
  }
  if (reads_stdin) {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    body.set("stdin_base64", Json::of(base64_encode(ss.str())));
  }
  body.set("args", Json::strings(cmd));
  if (!files.fields.empty()) body.set("files", files);
  std::error_code ec;
  body.set("cwd", Json::of(fs::current_path(ec).u8string()));
  if (no_cache) body.set("cache", Json::of(false));

  HttpReply reply = http_call(url, "POST", "/v1/run", to_json(body, 0));
  if (!reply.error.empty()) { std::cerr << "lmok remote: " << reply.error << "\n"; return 2; }
  if (raw_json) { std::cout << reply.body; return reply.status == 200 ? 0 : 1; }
  Json result;
  try { result = JsonParser(reply.body).parse(); }
  catch (const JsonError &e) { std::cerr << "lmok remote: " << e.what() << "\n"; return 2; }
  if (reply.status != 200) {
    const Json *err = result.get("error");
    const Json *msg = err ? err->get("message") : nullptr;
    std::cerr << "lmok remote: server answered " << reply.status << ": " << (msg ? msg->text : reply.body) << "\n";
    return 2;
  }
  auto stream = [&](const char *name) -> std::string {
    if (const Json *t = result.get(name)) return t->text;
    if (const Json *b = result.get(std::string(name) + "_base64")) return base64_decode(b->text).value_or("");
    return "";
  };
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  const std::string out = stream("stdout"), err = stream("stderr");
  std::cout.write(out.data(), std::streamsize(out.size()));
  std::cout.flush();
  std::cerr.write(err.data(), std::streamsize(err.size()));
  std::cerr.flush();
  const Json *code = result.get("exit_code");
  return code && code->integral ? int(code->integer) : 4;
}

} // namespace lmok

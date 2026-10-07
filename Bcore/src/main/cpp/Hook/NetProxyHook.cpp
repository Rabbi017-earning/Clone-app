// Per-process TCP proxy: hooks libc connect() and tunnels every outgoing TCP connection
// through an HTTP(CONNECT) or SOCKS5 proxy. Covers native libs, Chromium/WebView, Java sockets.
// UDP and DNS lookups are not proxied.

#include "NetProxyHook.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <netdb.h>
#include <atomic>
#include <mutex>
#include "Log.h"
#include "xdl.h"
#include "Dobby/dobby.h"

namespace {

struct Config {
    sockaddr_in proxy4;
    bool socks5 = false;
    bool dns = false;
    std::string user, pass;
};

Config g_cfg;
std::atomic<bool> g_on{false};
std::mutex g_mu;
bool g_hooked = false;

int (*orig_connect)(int, const struct sockaddr *, socklen_t) = nullptr;

// ---- DNS through proxy: hand out fake IPs (198.18.0.0/15) and CONNECT by hostname ----
std::unordered_map<uint32_t, std::string> g_ip2host;
std::unordered_map<std::string, uint32_t> g_host2ip;
std::mutex g_dns_mu;
uint32_t g_next = 0;
const uint32_t FAKE_BASE = 0xC6120000u; // 198.18.0.0
const uint32_t FAKE_SIZE = 131000u;

uint32_t fake_ip_for(const std::string &h) {
    std::lock_guard<std::mutex> l(g_dns_mu);
    auto it = g_host2ip.find(h);
    if (it != g_host2ip.end()) return it->second;
    uint32_t ip = FAKE_BASE + 1 + (g_next++ % FAKE_SIZE);
    auto old = g_ip2host.find(ip);
    if (old != g_ip2host.end()) { g_host2ip.erase(old->second); g_ip2host.erase(old); }
    g_ip2host[ip] = h;
    g_host2ip[h] = ip;
    return ip;
}

bool host_for_fake(uint32_t ip_host_order, std::string &out) {
    if ((ip_host_order & 0xFFFE0000u) != FAKE_BASE) return false;
    std::lock_guard<std::mutex> l(g_dns_mu);
    auto it = g_ip2host.find(ip_host_order);
    if (it == g_ip2host.end()) return false;
    out = it->second;
    return true;
}

bool is_loopback(const sockaddr *a) {
    if (a->sa_family == AF_INET) {
        auto *s = (const sockaddr_in *) a;
        return (ntohl(s->sin_addr.s_addr) >> 24) == 127 || s->sin_addr.s_addr == 0;
    }
    if (a->sa_family == AF_INET6) {
        auto *s = (const sockaddr_in6 *) a;
        if (IN6_IS_ADDR_LOOPBACK(&s->sin6_addr) || IN6_IS_ADDR_UNSPECIFIED(&s->sin6_addr)) return true;
        if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
            const uint8_t *b = s->sin6_addr.s6_addr;
            return b[12] == 127;
        }
    }
    return false;
}

bool is_proxy_addr(const sockaddr *a) {
    if (a->sa_family != AF_INET) return false;
    auto *s = (const sockaddr_in *) a;
    return s->sin_addr.s_addr == g_cfg.proxy4.sin_addr.s_addr && s->sin_port == g_cfg.proxy4.sin_port;
}

bool send_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *) buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        p += r; n -= r;
    }
    return true;
}

bool recv_all(int fd, void *buf, size_t n) {
    char *p = (char *) buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; return false; }
        p += r; n -= r;
    }
    return true;
}

std::string b64(const std::string &in) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((uint8_t) in[i] << 16) | ((uint8_t) in[i + 1] << 8) | (uint8_t) in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t) in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t) in[i] << 16) | ((uint8_t) in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// destination as text / raw
struct Dest {
    int family;           // AF_INET / AF_INET6
    uint8_t raw[16];
    uint16_t port_be;
    char text[INET6_ADDRSTRLEN];
    std::string host; // set when destination is a fake DNS ip
};

bool parse_dest(const sockaddr *a, Dest &d) {
    if (a->sa_family == AF_INET) {
        auto *s = (const sockaddr_in *) a;
        d.family = AF_INET; memcpy(d.raw, &s->sin_addr, 4); d.port_be = s->sin_port;
        inet_ntop(AF_INET, &s->sin_addr, d.text, sizeof(d.text));
        host_for_fake(ntohl(s->sin_addr.s_addr), d.host);
        return true;
    }
    if (a->sa_family == AF_INET6) {
        auto *s = (const sockaddr_in6 *) a;
        if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
            d.family = AF_INET; memcpy(d.raw, s->sin6_addr.s6_addr + 12, 4); d.port_be = s->sin6_port;
            inet_ntop(AF_INET, d.raw, d.text, sizeof(d.text));
            uint32_t v4; memcpy(&v4, d.raw, 4);
            host_for_fake(ntohl(v4), d.host);
            return true;
        }
        d.family = AF_INET6; memcpy(d.raw, &s->sin6_addr, 16); d.port_be = s->sin6_port;
        inet_ntop(AF_INET6, &s->sin6_addr, d.text, sizeof(d.text));
        return true;
    }
    return false;
}

bool socks5_handshake(int fd, const Dest &d) {
    bool auth = !g_cfg.user.empty();
    uint8_t hello[4] = {5, 1, 0, 0};
    size_t hl = 3;
    if (auth) { hello[1] = 2; hello[2] = 0; hello[3] = 2; hl = 4; }
    if (!send_all(fd, hello, hl)) return false;
    uint8_t rep[2];
    if (!recv_all(fd, rep, 2) || rep[0] != 5) return false;
    if (rep[1] == 2) {
        std::string m;
        m += (char) 1; m += (char) g_cfg.user.size(); m += g_cfg.user;
        m += (char) g_cfg.pass.size(); m += g_cfg.pass;
        if (!send_all(fd, m.data(), m.size())) return false;
        uint8_t ar[2];
        if (!recv_all(fd, ar, 2) || ar[1] != 0) return false;
    } else if (rep[1] != 0) {
        return false;
    }
    uint8_t req[300];
    size_t n = 0;
    req[n++] = 5; req[n++] = 1; req[n++] = 0;
    if (!d.host.empty() && d.host.size() < 256) {
        req[n++] = 3; req[n++] = (uint8_t) d.host.size();
        memcpy(req + n, d.host.data(), d.host.size()); n += d.host.size();
    }
    else if (d.family == AF_INET) { req[n++] = 1; memcpy(req + n, d.raw, 4); n += 4; }
    else { req[n++] = 4; memcpy(req + n, d.raw, 16); n += 16; }
    memcpy(req + n, &d.port_be, 2); n += 2;
    if (!send_all(fd, req, n)) return false;
    uint8_t h[4];
    if (!recv_all(fd, h, 4) || h[0] != 5 || h[1] != 0) return false;
    size_t skip = 0;
    if (h[3] == 1) skip = 4 + 2;
    else if (h[3] == 4) skip = 16 + 2;
    else if (h[3] == 3) { uint8_t l; if (!recv_all(fd, &l, 1)) return false; skip = l + 2; }
    else return false;
    uint8_t tmp[300];
    return skip <= sizeof(tmp) && recv_all(fd, tmp, skip);
}

bool http_handshake(int fd, const Dest &d) {
    char hostport[96];
    if (!d.host.empty()) snprintf(hostport, sizeof(hostport), "%.250s:%u", d.host.c_str(), ntohs(d.port_be));
    else if (d.family == AF_INET6) snprintf(hostport, sizeof(hostport), "[%s]:%u", d.text, ntohs(d.port_be));
    else snprintf(hostport, sizeof(hostport), "%s:%u", d.text, ntohs(d.port_be));
    std::string req = std::string("CONNECT ") + hostport + " HTTP/1.1\r\nHost: " + hostport + "\r\n";
    if (!g_cfg.user.empty()) req += "Proxy-Authorization: Basic " + b64(g_cfg.user + ":" + g_cfg.pass) + "\r\n";
    req += "Proxy-Connection: keep-alive\r\n\r\n";
    if (!send_all(fd, req.data(), req.size())) return false;
    std::string resp;
    char c;
    while (resp.size() < 4096) {
        if (!recv_all(fd, &c, 1)) return false;
        resp += c;
        size_t n = resp.size();
        if (n >= 4 && resp.compare(n - 4, 4, "\r\n\r\n") == 0) break;
    }
    // "HTTP/1.x 200"
    return resp.size() > 12 && resp.compare(0, 5, "HTTP/") == 0 && resp.compare(9, 3, "200") == 0;
}

int proxied_connect(int fd, const sockaddr *addr, socklen_t len) {
    Dest d;
    if (!parse_dest(addr, d)) return orig_connect(fd, addr, len);

    int flags = fcntl(fd, F_GETFL, 0);
    bool nonblock = flags >= 0 && (flags & O_NONBLOCK);
    if (nonblock) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    timeval oldSnd{}, oldRcv{};
    socklen_t tl = sizeof(timeval);
    getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &oldSnd, &tl);
    tl = sizeof(timeval);
    getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &oldRcv, &tl);
    timeval tmo{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));

    // socket family decides how we address the proxy
    int dom = 0; socklen_t dl = sizeof(dom);
    getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &dl);

    int rc;
    int saved_errno = 0;
    if (dom == AF_INET6) {
        sockaddr_in6 p6{};
        p6.sin6_family = AF_INET6;
        p6.sin6_port = g_cfg.proxy4.sin_port;
        p6.sin6_addr.s6_addr[10] = 0xff; p6.sin6_addr.s6_addr[11] = 0xff;
        memcpy(p6.sin6_addr.s6_addr + 12, &g_cfg.proxy4.sin_addr, 4);
        rc = orig_connect(fd, (sockaddr *) &p6, sizeof(p6));
    } else {
        rc = orig_connect(fd, (const sockaddr *) &g_cfg.proxy4, sizeof(g_cfg.proxy4));
    }
    bool ok = rc == 0;
    if (!ok) saved_errno = errno;
    if (ok) {
        ok = g_cfg.socks5 ? socks5_handshake(fd, d) : http_handshake(fd, d);
        if (!ok) { saved_errno = ECONNREFUSED; shutdown(fd, SHUT_RDWR); }
    }

    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &oldSnd, sizeof(oldSnd));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &oldRcv, sizeof(oldRcv));
    if (nonblock) fcntl(fd, F_SETFL, flags);

    if (ok) return 0;
    errno = saved_errno ? saved_errno : ECONNREFUSED;
    return -1;
}

int my_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!g_on.load(std::memory_order_relaxed) || addr == nullptr ||
        (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)) {
        return orig_connect(fd, addr, len);
    }
    int type = 0; socklen_t tl = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) {
        return orig_connect(fd, addr, len);
    }
    if (is_loopback(addr) || is_proxy_addr(addr)) return orig_connect(fd, addr, len);
    return proxied_connect(fd, addr, len);
}

typedef int (*gai_t)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef int (*gai_net_t)(const char *, const char *, const struct addrinfo *, unsigned, unsigned,
                         struct addrinfo **);
gai_t orig_gai = nullptr;
gai_net_t orig_gai_net = nullptr;
bool g_dns_hooked = false;

bool should_fake(const char *node, const struct addrinfo *hints) {
    if (!g_on.load() || !g_cfg.dns || !node || !*node) return false;
    if (hints && (hints->ai_flags & AI_NUMERICHOST)) return false;
    if (strcmp(node, "localhost") == 0 || strlen(node) > 250) return false;
    in_addr a4; in6_addr a6;
    if (inet_pton(AF_INET, node, &a4) == 1 || inet_pton(AF_INET6, node, &a6) == 1) return false;
    return true;
}

void fake_target(const char *node, const struct addrinfo *hints, char *ipstr, size_t n,
                 struct addrinfo &h) {
    in_addr a; a.s_addr = htonl(fake_ip_for(node));
    inet_ntop(AF_INET, &a, ipstr, n);
    memset(&h, 0, sizeof(h));
    if (hints) { h.ai_socktype = hints->ai_socktype; h.ai_protocol = hints->ai_protocol; }
    h.ai_family = AF_INET;
    h.ai_flags = AI_NUMERICHOST;
}

int my_gai(const char *node, const char *serv, const struct addrinfo *hints, struct addrinfo **res) {
    if (should_fake(node, hints)) {
        char ip[32]; struct addrinfo h;
        fake_target(node, hints, ip, sizeof(ip), h);
        return orig_gai(ip, serv, &h, res);
    }
    return orig_gai(node, serv, hints, res);
}

int my_gai_net(const char *node, const char *serv, const struct addrinfo *hints, unsigned netid,
               unsigned mark, struct addrinfo **res) {
    if (should_fake(node, hints)) {
        char ip[32]; struct addrinfo h;
        fake_target(node, hints, ip, sizeof(ip), h);
        return orig_gai_net(ip, serv, &h, netid, mark, res);
    }
    return orig_gai_net(node, serv, hints, netid, mark, res);
}

void install_dns_hooks() {
    if (g_dns_hooked) return;
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (!handle) return;
    void *t1 = xdl_dsym(handle, "getaddrinfo", nullptr);
    void *t2 = xdl_dsym(handle, "android_getaddrinfofornet", nullptr);
    xdl_close(handle);
    if (t1 && DobbyHook(t1, (void *) my_gai, (void **) &orig_gai) != 0) orig_gai = nullptr;
    if (t2 && DobbyHook(t2, (void *) my_gai_net, (void **) &orig_gai_net) != 0) orig_gai_net = nullptr;
    g_dns_hooked = true;
    ALOGD("NetProxyHook: dns hooks gai=%d net=%d", orig_gai != nullptr, orig_gai_net != nullptr);
}

bool install_hook() {
    if (g_hooked) return true;
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (!handle) return false;
    void *target = xdl_dsym(handle, "connect", nullptr);
    xdl_close(handle);
    if (!target) return false;
    if (DobbyHook(target, (void *) my_connect, (void **) &orig_connect) != 0) return false;
    g_hooked = true;
    ALOGD("NetProxyHook: connect() hooked");
    return true;
}

} // namespace

jboolean enableNetProxy(JNIEnv *env, jclass clazz, jstring ip, jint port, jboolean socks5,
                        jstring user, jstring pass, jboolean proxyDns) {
    const char *ipc = env->GetStringUTFChars(ip, nullptr);
    sockaddr_in p{};
    p.sin_family = AF_INET;
    p.sin_port = htons((uint16_t) port);
    bool valid = inet_pton(AF_INET, ipc, &p.sin_addr) == 1;
    env->ReleaseStringUTFChars(ip, ipc);
    if (!valid || port <= 0) return JNI_FALSE;

    const char *u = user ? env->GetStringUTFChars(user, nullptr) : nullptr;
    const char *w = pass ? env->GetStringUTFChars(pass, nullptr) : nullptr;

    std::lock_guard<std::mutex> lock(g_mu);
    g_cfg.proxy4 = p;
    g_cfg.socks5 = socks5;
    g_cfg.dns = proxyDns;
    g_cfg.user = u ? u : "";
    g_cfg.pass = w ? w : "";
    if (u) env->ReleaseStringUTFChars(user, u);
    if (w) env->ReleaseStringUTFChars(pass, w);

    if (!install_hook()) { ALOGE("NetProxyHook: install failed"); return JNI_FALSE; }
    if (proxyDns) install_dns_hooks();
    g_on = true;
    return JNI_TRUE;
}

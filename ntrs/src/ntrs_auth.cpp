#include <ntrs_auth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <utils/sysdef.h>

#if defined(OS_WINDOWS)
#include <process.h>
#else
#include <unistd.h>
#endif

#if defined(OS_WINDOWS)
#include <errno.h>
#else
#include <fcntl.h>
#endif
#include <sstream>

#if defined(__linux__)
#include <sys/random.h>
#endif

#if defined(OS_WINDOWS)
static bool FillRandomBytesWindows(void* buf, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i) {
        unsigned int value = 0;
        if (rand_s(&value) != 0) {
            return false;
        }
        out[i] = (uint8_t)(value & 0xFFu);
    }
    return true;
}
#endif

namespace eular {
namespace ntrs {

namespace {

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  buffer[64];
    size_t   buffer_len;
};

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t Rotr32(uint32_t value, uint32_t shift) { return (value >> shift) | (value << (32u - shift)); }

static uint32_t Load32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void Store32be(uint8_t* p, uint32_t value)
{
    p[0] = (uint8_t)((value >> 24) & 0xFFu);
    p[1] = (uint8_t)((value >> 16) & 0xFFu);
    p[2] = (uint8_t)((value >> 8) & 0xFFu);
    p[3] = (uint8_t)(value & 0xFFu);
}

static void Store64be(uint8_t* p, uint64_t value)
{
    for (int i = 7; i >= 0; --i) {
        p[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static void Sha256Transform(Sha256Ctx* ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;

    for (size_t i = 0; i < 16; ++i) {
        w[i] = Load32be(block + i * 4u);
    }
    for (size_t i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (size_t i = 0; i < 64; ++i) {
        uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void Sha256Init(Sha256Ctx* ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_len = 0;
    ctx->buffer_len = 0;
}

static void Sha256Update(Sha256Ctx* ctx, const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (len == 0) {
        return;
    }
    ctx->bit_len += (uint64_t)len * 8u;
    while (len > 0) {
        size_t copy_len = sizeof(ctx->buffer) - ctx->buffer_len;
        if (copy_len > len) {
            copy_len = len;
        }
        memcpy(ctx->buffer + ctx->buffer_len, p, copy_len);
        ctx->buffer_len += copy_len;
        p += copy_len;
        len -= copy_len;
        if (ctx->buffer_len == sizeof(ctx->buffer)) {
            Sha256Transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void Sha256Final(Sha256Ctx* ctx, uint8_t digest[32])
{
    ctx->buffer[ctx->buffer_len++] = 0x80u;
    if (ctx->buffer_len > 56u) {
        while (ctx->buffer_len < sizeof(ctx->buffer)) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        Sha256Transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }
    while (ctx->buffer_len < 56u) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }
    Store64be(ctx->buffer + 56, ctx->bit_len);
    Sha256Transform(ctx, ctx->buffer);
    for (size_t i = 0; i < 8; ++i) {
        Store32be(digest + i * 4u, ctx->state[i]);
    }
}

static std::string HexBytes(const uint8_t* bytes, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return out;
}

static std::string HmacSha256Hex(const std::string& key, const std::string& message)
{
    uint8_t   key_block[64];
    uint8_t   inner_pad[64];
    uint8_t   outer_pad[64];
    uint8_t   key_hash[32];
    uint8_t   inner_hash[32];
    uint8_t   digest[32];
    Sha256Ctx ctx;

    memset(key_block, 0, sizeof(key_block));
    if (key.size() > sizeof(key_block)) {
        Sha256Init(&ctx);
        Sha256Update(&ctx, key.data(), key.size());
        Sha256Final(&ctx, key_hash);
        memcpy(key_block, key_hash, sizeof(key_hash));
    } else if (!key.empty()) {
        memcpy(key_block, key.data(), key.size());
    }

    for (size_t i = 0; i < sizeof(key_block); ++i) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    Sha256Init(&ctx);
    Sha256Update(&ctx, inner_pad, sizeof(inner_pad));
    Sha256Update(&ctx, message.data(), message.size());
    Sha256Final(&ctx, inner_hash);

    Sha256Init(&ctx);
    Sha256Update(&ctx, outer_pad, sizeof(outer_pad));
    Sha256Update(&ctx, inner_hash, sizeof(inner_hash));
    Sha256Final(&ctx, digest);
    return HexBytes(digest, sizeof(digest));
}

static bool FillRandomBytes(void* buf, size_t len)
{
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t   offset = 0;

#if defined(OS_WINDOWS)
    return FillRandomBytesWindows(buf, len);
#endif

#if defined(__linux__)
    while (offset < len) {
        ssize_t n = getrandom(out + offset, len - offset, 0);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        break;
    }
    if (offset == len) {
        return true;
    }
    offset = 0;
#endif

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (offset < len) {
            ssize_t n = read(fd, out + offset, len - offset);
            if (n <= 0) {
                break;
            }
            offset += (size_t)n;
        }
        close(fd);
        if (offset == len) {
            return true;
        }
    }

    return false;
}

static uint64_t WeakRandom64()
{
    uint64_t a = (uint64_t)time(NULL);
    uint64_t b = (uint64_t)
#if defined(OS_WINDOWS)
        _getpid();
#else
        getpid();
#endif
    uint64_t c = (uint64_t)rand();
    uint64_t d = (uint64_t)rand();
    return (a << 32) ^ (b << 16) ^ (c << 8) ^ d;
}

static std::string RandomHexToken(const char* prefix)
{
    static const char  kHex[] = "0123456789abcdef";
    uint8_t            bytes[16];
    std::ostringstream oss;

    if (!FillRandomBytes(bytes, sizeof(bytes))) {
        uint64_t value1 = WeakRandom64();
        uint64_t value2 = WeakRandom64();
        for (size_t i = 0; i < 8; ++i) {
            bytes[i] = (uint8_t)((value1 >> ((7u - i) * 8u)) & 0xFFu);
            bytes[i + 8] = (uint8_t)((value2 >> ((7u - i) * 8u)) & 0xFFu);
        }
    }

    oss << prefix;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        oss << kHex[(bytes[i] >> 4) & 0x0Fu] << kHex[bytes[i] & 0x0Fu];
    }
    return oss.str();
}

}  // namespace

ControlAuthManager::ControlAuthManager(const std::string& shared_secret, uint32_t session_ttl_sec)
    : shared_secret_(shared_secret.empty() ? "ntrs-dev-secret" : shared_secret),
      session_ttl_sec_(session_ttl_sec == 0 ? 30 : session_ttl_sec)
{
}

bool ControlAuthManager::issueSession(const std::string& peer_id, const std::string& bootstrap_token, int fd,
                                      uint64_t now_sec, ControlSession* session, std::string* reason)
{
    if (peer_id.empty()) {
        if (reason != NULL) {
            *reason = "peer_id required";
        }
        return false;
    }
    if (bootstrap_token != shared_secret_) {
        if (reason != NULL) {
            *reason = "invalid bootstrap token";
        }
        return false;
    }

    ControlSession current;
    current.peer_id = peer_id;
    current.token = mintToken(peer_id, fd, now_sec);
    current.fd = fd;
    current.expire_at_sec = now_sec + session_ttl_sec_;
    sessions_[fd] = current;

    if (session != NULL) {
        *session = current;
    }
    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::validateSession(int fd, const std::string& peer_id, const std::string& session_token,
                                         uint64_t now_sec, std::string* reason)
{
    std::map<int, ControlSession>::iterator it = sessions_.find(fd);
    if (it == sessions_.end()) {
        if (reason != NULL) {
            *reason = "auth session missing";
        }
        return false;
    }
    if (!peer_id.empty() && it->second.peer_id != peer_id) {
        if (reason != NULL) {
            *reason = "peer_id mismatch";
        }
        return false;
    }
    if (session_token.empty() || it->second.token != session_token) {
        if (reason != NULL) {
            *reason = "session token invalid";
        }
        return false;
    }
    if (now_sec >= it->second.expire_at_sec) {
        if (reason != NULL) {
            *reason = "session token expired";
        }
        return false;
    }

    it->second.expire_at_sec = now_sec + session_ttl_sec_;

    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::issuePeerSession(const std::string& src_peer_id, const std::string& dst_peer_id,
                                          const std::string& session_id, uint64_t now_sec, uint32_t ttl_sec,
                                          PeerSessionLease* session, std::string* reason)
{
    PeerSessionLease current;

    if (src_peer_id.empty() || dst_peer_id.empty() || session_id.empty()) {
        if (reason != NULL) {
            *reason = "session scope required";
        }
        return false;
    }

    current.session_id = session_id;
    current.token = MintPeerSessionToken(src_peer_id, dst_peer_id, now_sec);
    current.src_peer_id = src_peer_id;
    current.dst_peer_id = dst_peer_id;
    current.expire_at_sec = now_sec + (ttl_sec == 0 ? session_ttl_sec_ : ttl_sec);
    peer_sessions_[session_id] = current;

    if (session != NULL) {
        *session = current;
    }
    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::validatePeerSession(const std::string& session_id, const std::string& src_peer_id,
                                             const std::string& dst_peer_id, const std::string& token, uint64_t now_sec,
                                             std::string* reason) const
{
    std::map<std::string, PeerSessionLease>::const_iterator it = peer_sessions_.find(session_id);
    if (it == peer_sessions_.end()) {
        if (reason != NULL) {
            *reason = "peer session missing";
        }
        return false;
    }
    if (!src_peer_id.empty() && it->second.src_peer_id != src_peer_id) {
        if (reason != NULL) {
            *reason = "peer session src mismatch";
        }
        return false;
    }
    if (!dst_peer_id.empty() && it->second.dst_peer_id != dst_peer_id) {
        if (reason != NULL) {
            *reason = "peer session dst mismatch";
        }
        return false;
    }
    if (token.empty() || it->second.token != token) {
        if (reason != NULL) {
            *reason = "peer session token invalid";
        }
        return false;
    }
    if (now_sec >= it->second.expire_at_sec) {
        if (reason != NULL) {
            *reason = "peer session token expired";
        }
        return false;
    }

    if (reason != NULL) {
        reason->clear();
    }
    return true;
}

bool ControlAuthManager::sessionPeerId(int fd, std::string* peer_id) const
{
    std::map<int, ControlSession>::const_iterator it = sessions_.find(fd);
    if (it == sessions_.end() || peer_id == NULL) {
        return false;
    }
    *peer_id = it->second.peer_id;
    return !peer_id->empty();
}

void ControlAuthManager::revokeFd(int fd) { sessions_.erase(fd); }

void ControlAuthManager::revokePeerSession(const std::string& session_id) { peer_sessions_.erase(session_id); }

void ControlAuthManager::sweepExpired(uint64_t now_sec)
{
    std::map<int, ControlSession>::iterator it = sessions_.begin();
    while (it != sessions_.end()) {
        if (now_sec >= it->second.expire_at_sec) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    std::map<std::string, PeerSessionLease>::iterator pit = peer_sessions_.begin();
    while (pit != peer_sessions_.end()) {
        if (now_sec >= pit->second.expire_at_sec) {
            pit = peer_sessions_.erase(pit);
        } else {
            ++pit;
        }
    }
}

uint32_t ControlAuthManager::sessionTtlSec() const { return session_ttl_sec_; }

std::string ControlAuthManager::mintToken(const std::string& peer_id, int fd, uint64_t now_sec) const
{
    (void)peer_id;
    (void)fd;
    (void)now_sec;
    return RandomHexToken("ctl_");
}

std::string MintPeerSessionId(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return RandomHexToken("sid_");
}

std::string MintPeerSessionToken(const std::string& src_peer_id, const std::string& dst_peer_id, uint64_t now_sec)
{
    (void)src_peer_id;
    (void)dst_peer_id;
    (void)now_sec;
    return RandomHexToken("peer_");
}

std::string MintProbeAuthorization(const std::string& shared_secret, const std::string& owner_peer_id,
                                   const std::string& target_ip, uint16_t target_port, const std::string& probe_token,
                                   uint64_t expire_at_sec)
{
    std::ostringstream oss;
    if (shared_secret.empty()) {
        return "";
    }
    oss << "probe|" << owner_peer_id << "|" << target_ip << "|" << target_port << "|" << probe_token << "|"
        << expire_at_sec;
    return HmacSha256Hex(shared_secret, oss.str());
}

bool ValidateProbeAuthorization(const std::string& shared_secret, const std::string& owner_peer_id,
                                const std::string& target_ip, uint16_t target_port, const std::string& probe_token,
                                uint64_t expire_at_sec, const std::string& authorization)
{
    if (owner_peer_id.empty() || target_ip.empty() || target_port == 0 || probe_token.empty() ||
        authorization.empty()) {
        return false;
    }
    return authorization ==
           MintProbeAuthorization(shared_secret, owner_peer_id, target_ip, target_port, probe_token, expire_at_sec);
}

}  // namespace ntrs
}  // namespace eular

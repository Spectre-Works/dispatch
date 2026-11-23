// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "shelly_http.h"

#include "esp_random.h"
#include "http_parser.h"
#include "local_tcp.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    char *body, *challenge;
    size_t capacity, challenge_capacity, body_used, challenge_used, field_used;
    char field[32];
    bool header_value, wanted, complete;
} http_response;

static int on_field(http_parser *p, const char *fragment, size_t n)
{
    http_response *v = p->data;
    if (v->header_value) {
        v->field_used = 0;
        v->header_value = false;
    }
    if (n < sizeof(v->field) - v->field_used) {
        memcpy(v->field + v->field_used, fragment, n);
        v->field_used += n;
        v->field[v->field_used] = 0;
    } else {
        v->field_used = sizeof(v->field);
    }
    return 0;
}

static int on_value(http_parser *p, const char *fragment, size_t n)
{
    http_response *v = p->data;
    if (!v->header_value) {
        v->wanted = v->challenge && v->field_used < sizeof(v->field) && !strcasecmp(v->field, "WWW-Authenticate");
        if (v->wanted && v->challenge_used) {
            return -1;
        }
        v->header_value = true;
    }
    if (v->wanted) {
        if (n >= v->challenge_capacity - v->challenge_used || memchr(fragment, 0, n)) {
            return -1;
        }
        memcpy(v->challenge + v->challenge_used, fragment, n);
        v->challenge_used += n;
        v->challenge[v->challenge_used] = 0;
    }
    return 0;
}

static int on_body(http_parser *p, const char *fragment, size_t n)
{
    http_response *v = p->data;
    if (n >= v->capacity - v->body_used || memchr(fragment, 0, n)) {
        return -1;
    }
    memcpy(v->body + v->body_used, fragment, n);
    v->body_used += n;
    v->body[v->body_used] = 0;
    return 0;
}

static int on_complete(http_parser *p)
{
    http_response *v = p->data;
    v->complete = true;
    http_parser_pause(p, 1);
    return 0;
}

static int exchange(const char *ip, const char *path, const char *body, const char *authorization,
                    char *answer, size_t capacity, char *challenge, size_t challenge_capacity, int64_t deadline)
{
    char req[768];
    const bool auth = authorization[0];
    const char *auth_label = auth ? "Authorization: " : "", *auth_end = auth ? "\r\n" : "";
    const int size = body
        ? snprintf(req, sizeof(req), "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "Content-Type: application/json\r\nContent-Length: %u\r\n%s%s%s\r\n%s",
            path, ip, (unsigned)strlen(body), auth_label, authorization, auth_end, body)
        : snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s%s%s\r\n",
            path, ip, auth_label, authorization, auth_end);

    int fd = size > 0 && (size_t)size < sizeof(req) ? local_tcp_open(ip, 80, deadline) : -1;
    bool sent = fd >= 0 && local_tcp_send(fd, req, size, deadline);
    explicit_bzero(req, sizeof(req));
    if (!sent) {
        if (fd >= 0) {
            close(fd);
        }
        return 0;
    }

    answer[0] = 0;
    if (challenge) challenge[0] = 0;
    http_response v = {.body = answer, .challenge = challenge, .capacity = capacity,
                       .challenge_capacity = challenge_capacity};
    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = &v;
    const http_parser_settings callbacks = {
        .on_header_field = on_field, .on_header_value = on_value,
        .on_body = on_body, .on_message_complete = on_complete,
    };
    size_t total = 0;
    while (!v.complete && total < 6144) {
        char buf[512];
        int n = local_tcp_read(fd, buf, sizeof(buf), deadline);
        if (n < 0) {
            break;
        }
        total += n;
        size_t parsed = http_parser_execute(&parser, &callbacks, buf, n);
        if (!n || parsed != (size_t)n || HTTP_PARSER_ERRNO(&parser) != HPE_OK) {
            break;
        }
    }
    close(fd);
    return v.complete && total <= 6144 && HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED ? parser.status_code : 0;
}

int shelly_http_until(const char *ip, const char *body, const char *authorization,
                      char *answer, size_t capacity, char *challenge, size_t challenge_capacity, int64_t deadline)
{
    return exchange(ip, "/rpc", body, authorization, answer, capacity, challenge, challenge_capacity, deadline);
}

int shelly_get_until(const char *ip, const char *path, const char *credentials,
                     char *answer, size_t capacity, int64_t deadline)
{
    char authorization[6 + 4 * ((sizeof(((shelly_target *)0)->password) + 1) / 3) + 1] = "";
    if (credentials[0]) {
        size_t written;
        if (mbedtls_base64_encode((unsigned char *)authorization + 6, sizeof(authorization) - 6, &written,
                                  (const unsigned char *)credentials, strlen(credentials))) {
            explicit_bzero(authorization, sizeof(authorization));
            return 0;
        }
        memcpy(authorization, "Basic ", 6);
    }
    const int code = exchange(ip, path, NULL, authorization, answer, capacity, NULL, 0, deadline);
    explicit_bzero(authorization, sizeof(authorization));
    return code;
}

static bool attribute(const char *challenge, const char *key, char *out, size_t capacity)
{
    if (strncasecmp(challenge, "Digest ", 7)) {
        return false;
    }
    const char *p = challenge + 7;
    size_t key_len = strlen(key);
    bool found = false;
    while (*p) {
        p += strspn(p, " ,\t");
        const char *name = p;
        size_t name_len = strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
        p += name_len;
        p += strspn(p, " \t");
        if (!name_len || *p != '=') {
            return false;
        }
        ++p;
        p += strspn(p, " \t");
        bool quoted = *p == '"';
        if (quoted) {
            ++p;
        }
        size_t len = strcspn(p, quoted ? "\"\r\n" : ", \t\r\n");
        if (quoted && p[len] != '"') {
            return false;
        }
        if (name_len == key_len && !strncasecmp(name, key, key_len)) {
            if (found || !len || len >= capacity) {
                return false;
            }
            memcpy(out, p, len);
            out[len] = 0;
            found = true;
        }
        p += len + quoted;
        p += strspn(p, " \t");
        if (*p && *p != ',') {
            return false;
        }
    }
    return found;
}

static bool hash_hex(const char *text, char out[65])
{
    uint8_t hash[32];
    size_t used;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)text, strlen(text),
                         hash, sizeof(hash), &used) != PSA_SUCCESS || used != sizeof(hash)) {
        return false;
    }
    const char *hex = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(hash); ++i) {
        out[2 * i] = hex[hash[i] >> 4];
        out[2 * i + 1] = hex[hash[i] & 15];
    }
    out[64] = 0;
    explicit_bzero(hash, sizeof(hash));
    return true;
}

bool shelly_digest(const shelly_target *target, const char *challenge,
                   char authorization[SHELLY_AUTH_BYTES])
{
    char realm[40], nonce[128], algorithm[12], qop[8];
    if (!target->password[0] || !attribute(challenge, "realm", realm, sizeof(realm)) ||
        strcmp(realm, target->identity) || !attribute(challenge, "nonce", nonce, sizeof(nonce)) ||
        strspn(nonce, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-") != strlen(nonce) ||
        !attribute(challenge, "algorithm", algorithm, sizeof(algorithm)) || strcasecmp(algorithm, "SHA-256") ||
        !attribute(challenge, "qop", qop, sizeof(qop)) || strcmp(qop, "auth") ||
        psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    char input[300], ha1[65], ha2[65], result[65], cnonce[17];
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(input, sizeof(input), "admin:%s:%s", realm, target->password);
    bool ok = hash_hex(input, ha1) && hash_hex("POST:/rpc", ha2);
    if (ok) {
        snprintf(input, sizeof(input), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
        ok = hash_hex(input, result);
    }
    if (ok) {
        int length = snprintf(authorization, SHELLY_AUTH_BYTES, "Digest username=\"admin\", realm=\"%s\", nonce=\"%s\", "
            "uri=\"/rpc\", algorithm=SHA-256, response=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\"",
            realm, nonce, result, cnonce);
        ok = length > 0 && length < SHELLY_AUTH_BYTES;
    }
    explicit_bzero(input, sizeof(input));
    explicit_bzero(ha1, sizeof(ha1));
    explicit_bzero(result, sizeof(result));
    return ok;
}

// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "aes/esp_aes.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "local_tcp.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <unistd.h>

int mbedtls_base64_encode(unsigned char *dst, size_t capacity, size_t *written,
                          const unsigned char *source, size_t bytes)
{
    if (4 * ((bytes + 2) / 3) >= capacity) return -1;
    *written = EVP_EncodeBlock(dst, source, bytes);
    return 0;
}

int mbedtls_base64_decode(unsigned char *dst, size_t capacity, size_t *written,
                          const unsigned char *source, size_t bytes)
{
    if (!bytes || bytes % 4 || 3 * (bytes / 4) > capacity) return -1;
    const int decoded = EVP_DecodeBlock(dst, source, bytes);
    if (decoded < 0) return -1;
    *written = decoded - (source[bytes - 1] == '=') - (source[bytes - 2] == '=');
    return 0;
}

int psa_hash_compute(int alg, const uint8_t *data, size_t bytes, uint8_t *digest, size_t capacity, size_t *used)
{
    unsigned written;
    if (alg != PSA_ALG_SHA_256 || capacity < 32 || EVP_Digest(data, bytes, digest, &written, EVP_sha256(), NULL) != 1)
        return -1;
    *used = written;
    return PSA_SUCCESS;
}

void esp_aes_init(esp_aes_context *aes)
{
    aes->cipher = EVP_CIPHER_CTX_new();
}

int esp_aes_setkey(esp_aes_context *aes, const unsigned char *key, unsigned bits)
{
    return aes->cipher && bits == 128 &&
        EVP_EncryptInit_ex(aes->cipher, EVP_aes_128_cbc(), NULL, key, NULL) == 1 &&
        EVP_CIPHER_CTX_set_padding(aes->cipher, 0) == 1 ? 0 : -1;
}

int esp_aes_crypt_cbc(esp_aes_context *aes, int mode, size_t bytes, unsigned char *iv,
                      const unsigned char *input, unsigned char *output)
{
    int written, final;
    return mode == ESP_AES_ENCRYPT && EVP_EncryptInit_ex(aes->cipher, NULL, NULL, NULL, iv) == 1 &&
        EVP_EncryptUpdate(aes->cipher, output, &written, input, bytes) == 1 && (size_t)written == bytes &&
        EVP_EncryptFinal_ex(aes->cipher, output + written, &final) == 1 && final == 0 ? 0 : -1;
}

void esp_aes_free(esp_aes_context *aes)
{
    EVP_CIPHER_CTX_free(aes->cipher);
}

struct esp_tls {
    SSL *connection;
    int fd;
};

esp_tls_t *esp_tls_init(void)
{
    esp_tls_t *tls = calloc(1, sizeof(*tls));
    if (tls) tls->fd = -1;
    return tls;
}

static bool broker_context(esp_tls_t *tls, const esp_tls_cfg_t *config)
{
    SSL_CTX *context = SSL_CTX_new(TLS_client_method());
    if (!context) return false;
    const unsigned char *next = config->cacert_buf;
    X509 *certificate = d2i_X509(NULL, &next, config->cacert_bytes);
    if (certificate && next == config->cacert_buf + config->cacert_bytes &&
        SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 1 &&
        X509_STORE_add_cert(SSL_CTX_get_cert_store(context), certificate) == 1) {
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
        tls->connection = SSL_new(context);
    }
    X509_free(certificate);
    SSL_CTX_free(context);
    if (!tls->connection) return false;

    SSL_set_hostflags(tls->connection, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return SSL_set1_host(tls->connection, config->common_name) == 1 &&
        SSL_set_tlsext_host_name(tls->connection, config->common_name) == 1 &&
        SSL_set_fd(tls->connection, tls->fd) == 1;
}

int esp_tls_conn_new_async(const char *ip, int length, int port, const esp_tls_cfg_t *config, esp_tls_t *tls)
{
    if (tls->fd < 0) {
        tls->fd = local_tcp_open(ip, port, esp_timer_get_time() + config->timeout_ms * 1000);
        if (tls->fd < 0 || !broker_context(tls, config)) return -1;
    }
    const int result = SSL_connect(tls->connection);
    if (result == 1) return SSL_get_verify_result(tls->connection) == X509_V_OK ? 1 : -1;
    const int error = SSL_get_error(tls->connection, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd)
{
    *fd = tls->fd;
    return tls->fd < 0 ? ESP_FAIL : ESP_OK;
}

static int io_result(esp_tls_t *tls, int result, size_t bytes)
{
    if (result == 1) return bytes;
    switch (SSL_get_error(tls->connection, result)) {
        case SSL_ERROR_WANT_READ: return ESP_TLS_ERR_SSL_WANT_READ;
        case SSL_ERROR_WANT_WRITE: return ESP_TLS_ERR_SSL_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return 0;
        default: return -1;
    }
}

int esp_tls_conn_read(esp_tls_t *tls, void *data, size_t size)
{
    size_t bytes = 0;
    const int result = SSL_read_ex(tls->connection, data, size, &bytes);
    return io_result(tls, result, bytes);
}

int esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t size)
{
    size_t bytes = 0;
    const int result = SSL_write_ex(tls->connection, data, size, &bytes);
    return io_result(tls, result, bytes);
}

void esp_tls_conn_destroy(esp_tls_t *tls)
{
    SSL_free(tls->connection);
    if (tls->fd >= 0) close(tls->fd);
    free(tls);
}

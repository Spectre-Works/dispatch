// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "network_update.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include <stdio.h>

static const char *const tag = "network_update";

void network_update_confirm_boot(bool healthy)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) return;
#ifdef DISPATCH_OTA_TRIAL_FAIL
    ESP_LOGW(tag, "Resetting unconfirmed trial in %s", running->label);
    esp_restart();
#endif
    if (healthy && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(tag, "Confirmed %s in %s", esp_app_get_description()->version, running->label);
        return;
    }
    ESP_LOGE(tag, "Trial boot failed in %s", running->label);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}

#ifdef DISPATCH_OTA_RELEASE_BASE
#include "network.h"

#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "http_parser.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const unsigned char ota_ca[] asm("_binary_dispatch_ota_ca_start");
extern const unsigned char ota_ca_end[] asm("_binary_dispatch_ota_ca_end");
extern const unsigned char ota_signer[] asm("_binary_dispatch_ota_signer_start");
extern const unsigned char ota_signer_end[] asm("_binary_dispatch_ota_signer_end");

enum {
    OTA_MAX_IMAGE = 0x340000, OTA_TASK_STACK = 8192, OTA_BUFFER = 4096,
    OTA_MANIFEST_MAX = 320, OTA_REDIRECT_MAX = 2048, OTA_REDIRECTS = 3,
    OTA_HEADER_MAX = 8192,
    CHECK_INTERVAL = 86400, RETRY_INTERVAL = 3600, BUTTON_INTERVAL = 15,
    SOCKET_TIMEOUT_MS = 2500, CHECK_TIMEOUT = 15, INSTALL_TIMEOUT = 120,
};
static const char release_base[] = DISPATCH_OTA_RELEASE_BASE;
static const char manifest_prefix[] = "DISPATCH/01 OTA v1\n";
static const unsigned heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

typedef struct {
    char version[32];
    unsigned size;
    unsigned char digest[32];
} release;

typedef struct {
    char redirect[OTA_REDIRECT_MAX];
    unsigned char buffer[OTA_BUFFER];
    size_t smallest_block;
    int64_t deadline;
    uint32_t epoch;
} transfer;

static SemaphoreHandle_t mutex;
static nvs_handle_t nvs;
static network_update_view status;
static release offer;
static char seen[32], rejected[32];
static uint32_t last_check, next_check;
static const esp_partition_t *staged_slot;

static bool version_parts(const char *version, unsigned parts[3])
{
    if (*version++ != 'v') return false;
    for (unsigned i = 0; i < 3; ++i) {
        const char *start = version;
        unsigned value = 0;
        while (*version >= '0' && *version <= '9') {
            if (version - start >= 6) return false;
            value = value * 10 + (*version++ - '0');
        }
        if (version == start || (version - start > 1 && *start == '0') ||
            *version++ != (i == 2 ? '\0' : '.')) return false;
        parts[i] = value;
    }
    return true;
}

static bool newer(const char *candidate)
{
    unsigned offered[3], installed[3];
    if (!version_parts(candidate, offered) || !version_parts(status.installed, installed)) return false;
    for (unsigned i = 0; i < 3; ++i) {
        if (offered[i] != installed[i]) return offered[i] > installed[i];
    }
    return false;
}

static bool compatible_layout(void)
{
    const esp_partition_t *first = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *second = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    const esp_partition_t *archive = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "archive");
    const esp_partition_t *operations = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "operations");
    return first && first->address == 0x20000 && first->size == 0x3f0000 &&
        second && second->address == 0x410000 && second->size == OTA_MAX_IMAGE &&
        data && data->address == 0xf000 && data->size == 0x2000 &&
        archive && archive->address == 0x750000 && archive->size == 0x88000 &&
        operations && operations->address == 0x7d8000 && operations->size == 0x28000;
}

static void lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mutex); }

void network_update_start(void)
{
    snprintf(status.installed, sizeof(status.installed), "%s", esp_app_get_description()->version);
    mutex = xSemaphoreCreateMutex();
    unsigned parts[3];
    if (!mutex || !version_parts(status.installed, parts) || !compatible_layout() ||
        nvs_open("dispatch_ota", NVS_READWRITE, &nvs) != ESP_OK) return;
    status.supported = true;
    status.github = !strcmp(release_base, "https://github.com/Spectre-Works/dispatch/releases");
    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, "enabled", &enabled) == ESP_OK) status.enabled = enabled == 1;
    size_t size = sizeof(seen);
    if (nvs_get_str(nvs, "seen", seen, &size) != ESP_OK) seen[0] = 0;
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid) {
        status.result = UPDATE_RESULT_ROLLBACK;
        esp_app_desc_t description;
        if (esp_ota_get_partition_description(invalid, &description) == ESP_OK) {
            memcpy(rejected, description.version, sizeof(rejected) - 1);
        }
        ESP_LOGW(tag, "Previous trial failed in %s", invalid->label);
    }
    ESP_LOGI(tag, "Running %s in %s", status.installed, esp_ota_get_running_partition()->label);
}

void network_update_read(network_update_view *view)
{
    if (mutex) lock();
    *view = status;
    if (mutex) unlock();
}

static void clear_offer(void)
{
    offer = (release){0};
    status.offered[0] = 0;
    status.unread = false;
    status.received = status.total = 0;
}

esp_err_t network_update_enable(bool enabled)
{
    if (!status.supported) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (status.phase == UPDATE_IDLE || status.phase == UPDATE_CHECK_QUEUED) {
        err = nvs_set_u8(nvs, "enabled", enabled);
        if (err == ESP_OK) err = nvs_commit(nvs);
        if (err == ESP_OK && enabled != status.enabled) {
            status.enabled = enabled;
            status.phase = UPDATE_IDLE;
            status.result = UPDATE_RESULT_NONE;
            next_check = network_uptime();
            clear_offer();
        }
    }
    unlock();
    return err;
}

static bool connected(void)
{
    return network_online() && !network_setup_active() && network_clock();
}

esp_err_t network_update_check(void)
{
    if (!status.supported) return ESP_ERR_INVALID_STATE;
    const uint32_t now = network_uptime();
    lock();
    const bool allowed = status.enabled && status.phase == UPDATE_IDLE && connected() &&
        (!last_check || now - last_check >= BUTTON_INTERVAL);
    if (allowed) status.phase = UPDATE_CHECK_QUEUED;
    unlock();
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t network_update_seen(const char *version)
{
    if (!status.supported) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (status.enabled && *offer.version && !strcmp(offer.version, version)) {
        err = ESP_OK;
        if (status.unread) {
            err = nvs_set_str(nvs, "seen", version);
            if (err == ESP_OK) err = nvs_commit(nvs);
            if (err == ESP_OK) {
                strcpy(seen, version);
                status.unread = false;
            }
        }
    }
    unlock();
    return err;
}

esp_err_t network_update_install(const char *version)
{
    if (!status.supported) return ESP_ERR_INVALID_STATE;
    lock();
    const bool allowed = status.enabled && status.phase == UPDATE_IDLE && connected() &&
        *offer.version && !strcmp(offer.version, version) && newer(version);
    if (allowed) {
        status.phase = UPDATE_INSTALL_QUEUED;
        status.result = UPDATE_RESULT_AVAILABLE;
        status.received = 0;
        status.total = offer.size;
    }
    unlock();
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool transfer_ready(const transfer *io)
{
    return network_online() && network_epoch() == io->epoch && esp_timer_get_time() < io->deadline;
}

static void sample_block(transfer *io)
{
    const size_t largest = heap_caps_get_largest_free_block(heap_caps);
    if (largest < io->smallest_block) io->smallest_block = largest;
}

typedef bool (*asset_sink)(void *context, const unsigned char *bytes, size_t count);

typedef struct {
    transfer *io;
    asset_sink sink;
    void *context;
    size_t limit, field_size, location_size;
    char field[sizeof("Location")];
    bool in_value, location, headers, complete;
} asset_response;

static int on_field(http_parser *parser, const char *bytes, size_t count)
{
    asset_response *response = parser->data;
    if (response->in_value) response->field_size = 0;
    response->in_value = false;
    if (response->field_size + count >= sizeof(response->field)) {
        response->field_size = sizeof(response->field);
    } else {
        memcpy(response->field + response->field_size, bytes, count);
        response->field_size += count;
        response->field[response->field_size] = 0;
    }
    return 0;
}

static int on_value(http_parser *parser, const char *bytes, size_t count)
{
    asset_response *response = parser->data;
    if (!response->in_value) {
        response->location = !response->headers && response->field_size == sizeof("Location") - 1 &&
            !strcasecmp(response->field, "Location");
        if (response->location && response->location_size) return -1;
    }
    response->in_value = true;
    if (response->location) {
        if (count >= sizeof(response->io->redirect) - response->location_size) return -1;
        memcpy(response->io->redirect + response->location_size, bytes, count);
        response->location_size += count;
        response->io->redirect[response->location_size] = 0;
    }
    return 0;
}

static int on_headers(http_parser *parser)
{
    asset_response *response = parser->data;
    response->headers = true;
    if (parser->nread > OTA_HEADER_MAX) return -1;
    if (parser->status_code != 200) http_parser_pause(parser, 1);
    else if (parser->content_length != UINT64_MAX && parser->content_length > response->limit) return -1;
    return 0;
}

static int on_body(http_parser *parser, const char *bytes, size_t count)
{
    asset_response *response = parser->data;
    if (count > response->limit) return -1;
    response->limit -= count;
    return response->sink(response->context, (const unsigned char *)bytes, count) ? 0 : -1;
}

static int on_done(http_parser *parser)
{
    ((asset_response *)parser->data)->complete = true;
    http_parser_pause(parser, 1);
    return 0;
}

static bool permitted_redirect(const char *url)
{
    const char *authority_end = strchr(release_base + sizeof("https://") - 1, '/');
    const size_t prefix = authority_end - release_base + 1;
    if (!strncmp(url, release_base, prefix - 1) && url[prefix - 1] == '/') return true;
    return !strncmp(release_base, "https://github.com/", sizeof("https://github.com/") - 1) &&
        (!strncmp(url, "https://release-assets.githubusercontent.com/", sizeof("https://release-assets.githubusercontent.com/") - 1) ||
         !strncmp(url, "https://objects.githubusercontent.com/", sizeof("https://objects.githubusercontent.com/") - 1));
}

static int tls_bytes(transfer *io, esp_tls_t *tls, int fd, unsigned char *bytes, size_t count, bool write)
{
    const int64_t limit = esp_timer_get_time() + SOCKET_TIMEOUT_MS * INT64_C(1000);
    while (transfer_ready(io)) {
        const int result = write ? esp_tls_conn_write(tls, bytes, count) : esp_tls_conn_read(tls, bytes, count);
        if (result >= 0) return result;
        if (result != ESP_TLS_ERR_SSL_WANT_READ && result != ESP_TLS_ERR_SSL_WANT_WRITE) return -1;
        const int64_t end = limit < io->deadline ? limit : io->deadline;
        const int64_t remaining = end - esp_timer_get_time();
        if (remaining <= 0) break;
        struct timeval timeout = {remaining / 1000000, remaining % 1000000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        const bool writable = result == ESP_TLS_ERR_SSL_WANT_WRITE;
        if (select(fd + 1, writable ? NULL : &fds, writable ? &fds : NULL, NULL, &timeout) != 1) break;
    }
    return -1;
}

static unsigned request_asset(transfer *io, const char *url, asset_response *response)
{
    static const http_parser_settings callbacks = {
        .on_header_field = on_field, .on_header_value = on_value, .on_headers_complete = on_headers,
        .on_body = on_body, .on_message_complete = on_done,
    };
    struct http_parser_url parts;
    http_parser_url_init(&parts);
    if (http_parser_parse_url(url, strlen(url), 0, &parts) ||
        (parts.field_set & ((1 << UF_USERINFO) | (1 << UF_FRAGMENT))) ||
        !(parts.field_set & (1 << UF_HOST)) || !(parts.field_set & (1 << UF_PATH))) return 0;
    const char *host = url + parts.field_data[UF_HOST].off;
    const char *path = url + parts.field_data[UF_PATH].off;
    const int bytes = snprintf((char *)io->buffer, sizeof(io->buffer),
        "GET %s HTTP/1.1\r\nHost: %.*s\r\nUser-Agent: DISPATCH/01\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
        path, (int)(path - host), host);
    if (bytes <= 0 || bytes >= sizeof(io->buffer)) return 0;

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return 0;
    const esp_tls_cfg_t config = {
        .cacert_buf = ota_ca, .cacert_bytes = ota_ca_end - ota_ca,
        .non_block = true, .timeout_ms = 100,
    };
    const int64_t connect_by = esp_timer_get_time() + SOCKET_TIMEOUT_MS * INT64_C(1000);
    int connected = 0, fd = -1;
    while (transfer_ready(io) && esp_timer_get_time() < connect_by) {
        connected = esp_tls_conn_new_async(host, parts.field_data[UF_HOST].len,
            (parts.field_set & (1 << UF_PORT)) ? parts.port : 443, &config, tls);
        sample_block(io);
        if (connected) break;
        vTaskDelay(1);
    }
    unsigned code = 0;
    if (connected != 1 || esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK) goto done;
    for (int sent = 0; sent < bytes;) {
        const int n = tls_bytes(io, tls, fd, io->buffer + sent, bytes - sent, true);
        if (n <= 0) goto done;
        sent += n;
    }
    io->redirect[0] = 0;
    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = response;
    size_t header_wire = 0;
    while (transfer_ready(io)) {
        const int n = tls_bytes(io, tls, fd, io->buffer, sizeof(io->buffer), false);
        sample_block(io);
        if (n < 0) break;
        http_parser_execute(&parser, &callbacks, (const char *)io->buffer, n);
        if (!response->headers && (header_wire += n) > OTA_HEADER_MAX) break;
        if (HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED) {
            code = parser.status_code;
            break;
        }
        if (!n || HTTP_PARSER_ERRNO(&parser) != HPE_OK) break;
        if (response->headers) vTaskDelay(1);
    }
done:
    esp_tls_conn_destroy(tls);
    return code;
}

static bool get_asset(transfer *io, const char *url, size_t limit, asset_sink sink, void *context)
{
    for (unsigned hop = 0; hop <= OTA_REDIRECTS && transfer_ready(io); ++hop) {
        asset_response response = {.io = io, .sink = sink, .context = context, .limit = limit};
        const unsigned code = request_asset(io, url, &response);
        if (code == 200) return response.complete && transfer_ready(io);
        if ((code != 301 && code != 302 && code != 303 && code != 307 && code != 308) ||
            !permitted_redirect(io->redirect)) return false;
        url = io->redirect;
    }
    return false;
}

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

static bool decode_hex(const unsigned char *hex, unsigned char *bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const int a = hex_value(hex[2 * i]), b = hex_value(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        bytes[i] = (a << 4) | b;
    }
    return true;
}

static bool parse_manifest(unsigned char *body, size_t length, release *candidate)
{
    if (length <= sizeof(manifest_prefix) ||
        memcmp(body, manifest_prefix, sizeof(manifest_prefix) - 1) ||
        memchr(body, 0, length)) return false;
    const unsigned char *end = body + length;
    const unsigned char *version = body + sizeof(manifest_prefix) - 1;
    const unsigned char *line = memchr(version, '\n', end - version);
    if (!line || line - version >= sizeof(candidate->version)) return false;
    memcpy(candidate->version, version, line - version);
    unsigned parts[3];
    if (!version_parts(candidate->version, parts)) return false;

    const unsigned char *size = line + 1;
    line = memchr(size, '\n', end - size);
    if (!line || size == line || line - size > 7 || *size == '0') return false;
    for (const unsigned char *p = size; p < line; ++p) {
        if (*p < '0' || *p > '9') return false;
        candidate->size = candidate->size * 10 + (*p - '0');
    }
    if (!candidate->size || candidate->size > OTA_MAX_IMAGE) return false;

    const unsigned char *digest = line + 1;
    const unsigned char *signature = digest + 2 * sizeof(candidate->digest) + 1;
    if (end - digest <= 2 * sizeof(candidate->digest) + 1 || signature[-1] != '\n' ||
        !decode_hex(digest, candidate->digest, sizeof(candidate->digest))) return false;
    const size_t hex_length = end - signature - 1;
    unsigned char der[72], hash[32];
    size_t hash_length = 0;
    if (end[-1] != '\n' || !hex_length || hex_length % 2 || hex_length > 2 * sizeof(der) ||
        !decode_hex(signature, der, hex_length / 2) ||
        psa_hash_compute(PSA_ALG_SHA_256, body, signature - body, hash, sizeof(hash), &hash_length) != PSA_SUCCESS)
        return false;
    mbedtls_pk_context signer;
    mbedtls_pk_init(&signer);
    const bool valid = mbedtls_pk_parse_public_key(&signer, ota_signer, ota_signer_end - ota_signer) == 0 &&
        mbedtls_pk_get_bitlen(&signer) == 256 &&
        mbedtls_pk_verify_ext(MBEDTLS_PK_SIGALG_ECDSA, &signer, MBEDTLS_MD_SHA256,
            hash, hash_length, der, hex_length / 2) == 0;
    mbedtls_pk_free(&signer);
    return valid;
}

typedef struct {
    unsigned char bytes[OTA_MANIFEST_MAX];
    size_t used;
} manifest_body;

static bool collect_manifest(void *context, const unsigned char *bytes, size_t count)
{
    manifest_body *manifest = context;
    memcpy(manifest->bytes + manifest->used, bytes, count);
    manifest->used += count;
    return true;
}

static update_result check_release(transfer *io, release *candidate)
{
    const char url[] = DISPATCH_OTA_RELEASE_BASE "/latest/download/dispatch01-ota-v1.txt";
    manifest_body manifest = {0};
    if (!get_asset(io, url, sizeof(manifest.bytes), collect_manifest, &manifest)) return UPDATE_RESULT_CONNECTION;
    if (!parse_manifest(manifest.bytes, manifest.used, candidate)) return UPDATE_RESULT_MANIFEST;
    if (!strcmp(candidate->version, rejected)) return UPDATE_RESULT_ROLLBACK;
    return newer(candidate->version) ? UPDATE_RESULT_AVAILABLE : UPDATE_RESULT_CURRENT;
}

enum {
    IMAGE_DESC_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
    IMAGE_PREFIX_SIZE = IMAGE_DESC_OFFSET + sizeof(esp_app_desc_t),
};

typedef struct {
    unsigned char prefix[IMAGE_PREFIX_SIZE];
    size_t prefix_used, written;
    const char *version;
    const esp_partition_t *slot;
    esp_ota_handle_t handle;
    psa_hash_operation_t hash;
    update_result failure;
} image_body;

static bool write_image(image_body *image, const unsigned char *bytes, size_t count)
{
    if (psa_hash_update(&image->hash, bytes, count) != PSA_SUCCESS) return false;
    if (esp_ota_write(image->handle, bytes, count) != ESP_OK) {
        image->failure = UPDATE_RESULT_STORAGE;
        return false;
    }
    image->written += count;
    lock();
    status.received = image->written;
    unlock();
    return true;
}

static bool receive_image(void *context, const unsigned char *bytes, size_t count)
{
    image_body *image = context;
    if (!image->handle) {
        const size_t missing = sizeof(image->prefix) - image->prefix_used;
        const size_t take = count < missing ? count : missing;
        memcpy(image->prefix + image->prefix_used, bytes, take);
        image->prefix_used += take;
        if (take < missing) return true;
        esp_app_desc_t desc;
        memcpy(&desc, image->prefix + IMAGE_DESC_OFFSET, sizeof(desc));
        if (image->prefix[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strncmp(desc.project_name, "dispatch01", sizeof(desc.project_name)) ||
            strncmp(desc.version, image->version, sizeof(desc.version))) {
            image->failure = UPDATE_RESULT_IMAGE;
            return false;
        }
        if (esp_ota_begin(image->slot, OTA_WITH_SEQUENTIAL_WRITES, &image->handle) != ESP_OK) {
            image->failure = UPDATE_RESULT_STORAGE;
            return false;
        }
        if (!write_image(image, image->prefix, sizeof(image->prefix))) return false;
        bytes += take;
        count -= take;
    }
    return !count || write_image(image, bytes, count);
}

static update_result download(transfer *io, const release *candidate, const esp_partition_t **slot_out)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || candidate->size > slot->size) return UPDATE_RESULT_IMAGE;
    image_body image = {
        .slot = slot, .version = candidate->version, .hash = PSA_HASH_OPERATION_INIT,
        .failure = UPDATE_RESULT_CONNECTION,
    };
    if (psa_hash_setup(&image.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) return UPDATE_RESULT_IMAGE;
    char url[sizeof(release_base) + 64];
    snprintf(url, sizeof(url), "%s/download/%s/dispatch01.bin", release_base, candidate->version);
    update_result result = image.failure;
    if (get_asset(io, url, candidate->size, receive_image, &image)) {
        unsigned char digest[32];
        size_t hash_size = 0;
        result = UPDATE_RESULT_IMAGE;
        if (image.written == candidate->size &&
            psa_hash_finish(&image.hash, digest, sizeof(digest), &hash_size) == PSA_SUCCESS &&
            hash_size == sizeof(digest) && !memcmp(digest, candidate->digest, sizeof(digest))) {
            const esp_err_t verified = esp_ota_end(image.handle);
            image.handle = 0;
            if (verified == ESP_OK) {
                *slot_out = slot;
                result = UPDATE_RESULT_AVAILABLE;
            }
        }
    } else {
        result = image.failure;
    }
    if (image.handle) esp_ota_abort(image.handle);
    psa_hash_abort(&image.hash);
    return result;
}

static void update_task(void *arg)
{
    const bool install = arg != NULL;
    const uint32_t started = network_uptime();
    const esp_partition_t *slot = NULL;
    release candidate = {0};
    update_result result = UPDATE_RESULT_MEMORY;
    transfer *io = calloc(1, sizeof(*io));
    if (io) {
        io->epoch = network_epoch();
        io->deadline = esp_timer_get_time() + (install ? INSTALL_TIMEOUT : CHECK_TIMEOUT) * INT64_C(1000000);
        io->smallest_block = SIZE_MAX;
        sample_block(io);
        if (!connected()) result = UPDATE_RESULT_CONNECTION;
        else if (install) result = download(io, &offer, &slot);
        else result = check_release(io, &candidate);
    }
    ESP_LOGI(tag, "%s result=%u elapsed=%lus heap_minimum=%u largest_sample_min=%u stack=%u",
        install ? "Download" : "Check", (unsigned)result, (unsigned long)(network_uptime() - started),
        (unsigned)heap_caps_get_minimum_free_size(heap_caps), io ? (unsigned)io->smallest_block : 0,
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(io);

    lock();
    if (!install) {
        const bool checked = result == UPDATE_RESULT_CURRENT || result == UPDATE_RESULT_AVAILABLE ||
            result == UPDATE_RESULT_ROLLBACK;
        next_check = network_uptime() + (checked ? CHECK_INTERVAL : RETRY_INTERVAL);
        clear_offer();
        if (result == UPDATE_RESULT_AVAILABLE) {
            offer = candidate;
            strcpy(status.offered, offer.version);
            status.unread = strcmp(seen, offer.version) != 0;
            status.total = offer.size;
        }
    }
    staged_slot = slot;
    status.result = result;
    status.phase = slot ? UPDATE_STAGED : UPDATE_IDLE;
    unlock();
    vTaskDelete(NULL);
}

void network_update_tick(void)
{
    if (!status.supported) return;
    lock();
    const uint32_t now = network_uptime();
    const bool install = status.phase == UPDATE_INSTALL_QUEUED;
    const bool check = status.phase == UPDATE_CHECK_QUEUED ||
        (status.phase == UPDATE_IDLE && now >= next_check);
    const bool online = connected();
    if (!online && (install || status.phase == UPDATE_CHECK_QUEUED)) {
        status.phase = UPDATE_IDLE;
        status.result = UPDATE_RESULT_CONNECTION;
        if (!install) next_check = now + RETRY_INTERVAL;
    } else if (status.enabled && online && (install || check)) {
        if (!install) last_check = now;
        status.phase = install ? UPDATE_DOWNLOADING : UPDATE_CHECKING;
        if (xTaskCreate(update_task, "dispatch_ota", OTA_TASK_STACK, install ? &offer : NULL, 3, NULL) != pdPASS) {
            status.phase = UPDATE_IDLE;
            status.result = UPDATE_RESULT_MEMORY;
            next_check = now + RETRY_INTERVAL;
        }
    }
    unlock();
}

bool network_update_staged(void)
{
    if (!status.supported) return false;
    lock();
    const bool staged = status.phase == UPDATE_STAGED || status.phase == UPDATE_RESTARTING;
    unlock();
    return staged;
}

bool network_update_active(void)
{
    if (!status.supported) return false;
    lock();
    const bool active = status.phase != UPDATE_IDLE;
    unlock();
    return active;
}

void network_update_restart(bool saved)
{
    lock();
    status.phase = UPDATE_RESTARTING;
    const esp_partition_t *slot = staged_slot;
    unlock();

    if (saved && esp_ota_set_boot_partition(slot) == ESP_OK) {
        ESP_LOGI(tag, "Restarting into %s", slot->label);
        esp_restart();
    }
    lock();
    staged_slot = NULL;
    status.phase = UPDATE_IDLE;
    status.result = UPDATE_RESULT_STORAGE;
    unlock();
}
#else
void network_update_start(void) {}
void network_update_read(network_update_view *view)
{
    *view = (network_update_view){0};
    snprintf(view->installed, sizeof(view->installed), "%s", esp_app_get_description()->version);
}
esp_err_t network_update_enable(bool enabled) { return ESP_ERR_INVALID_STATE; }
esp_err_t network_update_check(void) { return ESP_ERR_INVALID_STATE; }
esp_err_t network_update_seen(const char *version) { return ESP_ERR_INVALID_STATE; }
esp_err_t network_update_install(const char *version) { return ESP_ERR_INVALID_STATE; }
void network_update_tick(void) {}
bool network_update_staged(void) { return false; }
bool network_update_active(void) { return false; }
void network_update_restart(bool saved) {}
#endif

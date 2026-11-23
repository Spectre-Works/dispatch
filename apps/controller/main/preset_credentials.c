// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "preset_credentials.h"

#include "dispatch_policy.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <string.h>

typedef struct {
    uint8_t nonce[12];
    uint8_t encrypted[GOLDSHELL_PASSWORD_BYTES + 16];
} sealed_password;

static nvs_handle_t nvs;
static hmac_key_id_t key_slot;
static bool persistent;
static uint8_t available;
static char session_password[DISPATCH_LIMIT][GOLDSHELL_PASSWORD_BYTES];

static void storage_name(unsigned slot, char name[8])
{
    memcpy(name, "preset0", 8);
    name[6] += slot;
}

static bool open_key(psa_key_id_t *id, psa_key_usage_t usage)
{
    static const char purpose[] = "DISPATCH miner preset authorization v1";
    unsigned char derived[32];
    if (esp_hmac_calculate(key_slot, purpose, sizeof(purpose), derived) != ESP_OK) {
        explicit_bzero(derived, sizeof(derived));
        return false;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, sizeof(derived) * 8);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    const psa_status_t result = psa_import_key(&attributes, derived, sizeof(derived), id);
    explicit_bzero(derived, sizeof(derived));
    psa_reset_key_attributes(&attributes);
    return result == PSA_SUCCESS;
}

void preset_credentials_start(nvs_handle_t store)
{
    nvs = store;
    esp_efuse_block_t block;
    if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block) ||
        block < EFUSE_BLK_KEY0 || block > EFUSE_BLK_KEY5 ||
        !esp_efuse_get_key_dis_read(block) || psa_crypto_init() != PSA_SUCCESS) return;
    key_slot = block - EFUSE_BLK_KEY0;
    psa_key_id_t key;
    if (!open_key(&key, PSA_KEY_USAGE_DECRYPT)) return;
    psa_destroy_key(key);
    persistent = true;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        char name[8];
        storage_name(i, name);
        size_t length = 0;
        if (nvs_get_blob(nvs, name, NULL, &length) == ESP_OK && length == sizeof(sealed_password)) available |= 1u << i;
    }
}

bool preset_credentials_persistent(void)
{
    return persistent;
}

bool preset_credentials_available(unsigned slot)
{
    return (available & (1u << slot)) != 0;
}

esp_err_t preset_credentials_store(unsigned slot, const char *binding, const char *password)
{
    char plain[GOLDSHELL_PASSWORD_BYTES] = {0};
    if (!password[0] || strlen(password) >= sizeof(plain)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = ESP_OK;
    if (persistent) {
        psa_key_id_t key;
        if (!open_key(&key, PSA_KEY_USAGE_ENCRYPT)) return ESP_FAIL;
        strcpy(plain, password);
        sealed_password sealed;
        esp_fill_random(sealed.nonce, sizeof(sealed.nonce));
        size_t written;
        const psa_status_t encrypted = psa_aead_encrypt(key, PSA_ALG_GCM, sealed.nonce, sizeof(sealed.nonce),
            (const uint8_t *)binding, strlen(binding), (const uint8_t *)plain, sizeof(plain),
            sealed.encrypted, sizeof(sealed.encrypted), &written);
        explicit_bzero(plain, sizeof(plain));
        psa_destroy_key(key);
        char name[8];
        storage_name(slot, name);
        result = encrypted == PSA_SUCCESS && written == sizeof(sealed.encrypted) ?
            nvs_set_blob(nvs, name, &sealed, sizeof(sealed)) : ESP_FAIL;
        if (result == ESP_OK) result = nvs_commit(nvs);
    } else {
        explicit_bzero(session_password[slot], sizeof(session_password[slot]));
        strcpy(session_password[slot], password);
    }
    if (result == ESP_OK) available |= 1u << slot;
    return result;
}

bool preset_credentials_read(unsigned slot, const char *binding, char password[GOLDSHELL_PASSWORD_BYTES])
{
    if (!preset_credentials_available(slot)) return false;
    if (!persistent) {
        memcpy(password, session_password[slot], GOLDSHELL_PASSWORD_BYTES);
        return true;
    }
    char name[8];
    storage_name(slot, name);
    sealed_password sealed;
    size_t size = sizeof(sealed);
    psa_key_id_t key;
    if (nvs_get_blob(nvs, name, &sealed, &size) != ESP_OK || size != sizeof(sealed) ||
        !open_key(&key, PSA_KEY_USAGE_DECRYPT)) return false;
    size_t written;
    const psa_status_t decrypted = psa_aead_decrypt(key, PSA_ALG_GCM, sealed.nonce, sizeof(sealed.nonce),
        (const uint8_t *)binding, strlen(binding), sealed.encrypted, sizeof(sealed.encrypted),
        (uint8_t *)password, GOLDSHELL_PASSWORD_BYTES, &written);
    psa_destroy_key(key);
    if (decrypted == PSA_SUCCESS && written == GOLDSHELL_PASSWORD_BYTES && password[0] &&
        !password[GOLDSHELL_PASSWORD_BYTES - 1]) return true;
    explicit_bzero(password, GOLDSHELL_PASSWORD_BYTES);
    return false;
}

esp_err_t preset_credentials_forget(unsigned slot)
{
    available &= ~(1u << slot);
    explicit_bzero(session_password[slot], sizeof(session_password[slot]));
    if (!persistent) return ESP_OK;
    char name[8];
    storage_name(slot, name);
    const esp_err_t result = nvs_erase_key(nvs, name);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    return result == ESP_OK ? nvs_commit(nvs) : result;
}

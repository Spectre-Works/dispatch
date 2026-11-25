// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "platform.h"
#include "esp_crc.h"
#include "esp_partition.h"
#include "nvs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static int volume = -1;

bool preview_storage_open(const char *directory)
{
    volume = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (volume < 0 && errno == ENOENT && mkdir(directory, 0700) == 0)
        volume = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return volume >= 0 && flock(volume, LOCK_EX | LOCK_NB) == 0;
}

static bool transfer(int fd, size_t offset, void *data, size_t bytes, bool write)
{
    unsigned char *cursor = data;
    while (bytes) {
        ssize_t done = write ? pwrite(fd, cursor, bytes, offset) : pread(fd, cursor, bytes, offset);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return false;
        cursor += done;
        offset += done;
        bytes -= done;
    }
    return true;
}

esp_err_t preview_file_read(const char *name, void *data, size_t *bytes)
{
    const int fd = openat(volume, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    struct stat state;
    esp_err_t result = ESP_FAIL;
    if (fstat(fd, &state) == 0 && S_ISREG(state.st_mode)) {
        const size_t actual = state.st_size;
        result = data && *bytes < actual ? ESP_ERR_INVALID_SIZE :
            (!data || transfer(fd, 0, data, actual, false)) ? ESP_OK : ESP_FAIL;
        *bytes = actual;
    }
    close(fd);
    return result;
}

esp_err_t preview_file_write(const char *name, const void *data, size_t bytes)
{
    char temporary[96];
    const int length = snprintf(temporary, sizeof(temporary), ".%s.tmp", name);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return ESP_ERR_INVALID_ARG;
    // One owner thread per record. A leftover staging file can only be from an interrupted write.
    unlinkat(volume, temporary, 0);
    const int fd = openat(volume, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return ESP_FAIL;
    const bool written = transfer(fd, 0, (void *)data, bytes, true) && fdatasync(fd) == 0;
    const int closed = close(fd);
    if (written && closed == 0 && renameat(volume, temporary, volume, name) == 0)
        return fsync(volume) == 0 ? ESP_OK : ESP_FAIL;
    unlinkat(volume, temporary, 0);
    return ESP_FAIL;
}

static void setting_name(char name[80], nvs_handle_t handle, const char *key)
{
    // Both components come from fixed source literals or the integration family table.
    if (snprintf(name, 80, "%s-%s", handle, key) >= 80) abort();
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    *handle = name;
    return volume < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *bytes)
{
    char name[80];
    setting_name(name, handle, key);
    return preview_file_read(name, data, bytes);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t bytes)
{
    char name[80];
    setting_name(name, handle, key);
    return preview_file_write(name, data, bytes);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *data, size_t *bytes)
{
    const esp_err_t result = nvs_get_blob(handle, key, data, bytes);
    if (result == ESP_OK && data && (!*bytes || data[*bytes - 1] || strnlen(data, *bytes) != *bytes - 1))
        return ESP_ERR_INVALID_SIZE;
    return result;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *data)
{
    return nvs_set_blob(handle, key, data, strlen(data) + 1);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    char name[80];
    setting_name(name, handle, key);
    if (unlinkat(volume, name, 0)) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    return fsync(volume) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    return ESP_OK; // Each named setting is already committed atomically.
}

static esp_partition_t partitions[] = {
    {.name = "archive", .size = 0x88000, .fd = -1, .erase_size = 4096},
    {.name = "operations", .size = 0x28000, .fd = -1, .erase_size = 4096},
};

static esp_partition_t *open_partition(esp_partition_t *part)
{
    if (part->fd >= 0) return part;
    char name[80];
    snprintf(name, sizeof(name), "%s.bin", part->name);
    int fd = openat(volume, name, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        unsigned char *blank = malloc(part->size);
        if (!blank) return NULL;
        memset(blank, 0xff, part->size);
        const esp_err_t created = preview_file_write(name, blank, part->size);
        free(blank);
        if (created != ESP_OK) return NULL;
        fd = openat(volume, name, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) return NULL;
    struct stat status;
    if (fstat(fd, &status) || !S_ISREG(status.st_mode) || (size_t)status.st_size != part->size) {
        close(fd);
        fprintf(stderr, "Invalid persistent partition %s\n", name);
        abort();
    }
    part->fd = fd;
    return part;
}

const esp_partition_t *esp_partition_find_first(unsigned type, unsigned subtype, const char *name)
{
    if (type != ESP_PARTITION_TYPE_DATA || subtype < 0x41 || subtype > 0x42) return NULL;
    esp_partition_t *part = &partitions[subtype - 0x41];
    return strcmp(part->name, name) ? NULL : open_partition(part);
}

void esp_partition_register_external(void *chip, size_t offset, size_t size, const char *name,
                                     unsigned type, unsigned subtype, const esp_partition_t **part)
{
    *part = NULL; // ESP32 legacy addresses do not exist in a service volume.
}

esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *data, size_t size)
{
    return offset <= part->size && size <= part->size - offset &&
        transfer(part->fd, offset, data, size, false) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *data, size_t size)
{
    return offset <= part->size && size <= part->size - offset &&
        transfer(part->fd, offset, (void *)data, size, true) && fdatasync(part->fd) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t size)
{
    unsigned char erased[4096];
    memset(erased, 0xff, sizeof(erased));
    if (offset > part->size || size > part->size - offset || size % sizeof(erased)) return ESP_ERR_INVALID_SIZE;
    for (size_t pos = 0; pos < size; pos += sizeof(erased))
        if (!transfer(part->fd, offset + pos, erased, sizeof(erased), true)) return ESP_FAIL;
    return fdatasync(part->fd) == 0 ? ESP_OK : ESP_FAIL;
}

uint16_t esp_crc16_le(uint16_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
    }
    return ~crc;
}

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, size_t size)
{
    return crc32(crc, data, size);
}

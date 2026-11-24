// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "operations_store.h"

#include "esp_crc.h"
#include "esp_partition.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Each group starts with a complete snapshot. Changed blocks are appended
// atomically until the next group is needed. Erase only that next group.
enum { GROUP_BYTES = 16384, BLOCK_BYTES = 64, SNAPSHOT = 0x44534f01, DELTA = 0x44534f02 };
typedef struct {
    uint32_t type, sequence, bytes, crc;
} record_header;

static const esp_partition_t *partition;
static uint8_t *previous, *payload;
static size_t state_bytes, mask_bytes, groups, group, cursor = GROUP_BYTES;
static uint32_t sequence;

static size_t align_word(size_t bytes)
{
    return (bytes + 3) & ~(size_t)3;
}

static uint32_t checksum(record_header header)
{
    return esp_crc32_le(esp_crc32_le(0, (const uint8_t *)&header, offsetof(record_header, crc)),
                        payload, header.bytes);
}

static bool read_record(size_t address, size_t remaining, record_header *header)
{
    return remaining >= sizeof(*header) &&
        esp_partition_read(partition, address, header, sizeof(*header)) == ESP_OK &&
        (header->type == SNAPSHOT || header->type == DELTA) && header->sequence &&
        header->bytes <= state_bytes && align_word(header->bytes) <= remaining - sizeof(*header) &&
        esp_partition_read(partition, address + sizeof(*header), payload, header->bytes) == ESP_OK &&
        checksum(*header) == header->crc;
}

static bool delta(size_t bytes, bool apply)
{
    if (bytes <= mask_bytes) return false;
    size_t offset = mask_bytes;
    for (size_t pos = 0, block = 0; pos < state_bytes; pos += BLOCK_BYTES, ++block) {
        if (!(payload[block / 8] & (1u << (block % 8)))) continue;
        const size_t size = state_bytes - pos < BLOCK_BYTES ? state_bytes - pos : BLOCK_BYTES;
        if (size > bytes - offset) return false;
        if (apply) memcpy(previous + pos, payload + offset, size);
        offset += size;
    }
    return offset == bytes;
}

bool operations_load(void *state, size_t size)
{
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "operations");
    if (!partition || partition->encrypted || partition->readonly ||
        partition->size < 2 * GROUP_BYTES || partition->size % GROUP_BYTES ||
        size + sizeof(record_header) > GROUP_BYTES) return false;
    state_bytes = size;
    mask_bytes = (size + BLOCK_BYTES * 8 - 1) / (BLOCK_BYTES * 8);
    groups = partition->size / GROUP_BYTES;
    previous = malloc(size);
    payload = malloc(align_word(size));
    if (!previous || !payload) {
        free(previous);
        free(payload);
        previous = payload = NULL;
        return false;
    }

    record_header header;
    for (size_t i = 0; i < groups; ++i) {
        if (!read_record(i * GROUP_BYTES, GROUP_BYTES, &header) ||
            header.type != SNAPSHOT || header.bytes != size || header.sequence <= sequence) continue;
        group = i;
        sequence = header.sequence;
        memcpy(previous, payload, size);
    }
    if (!sequence) return false;

    cursor = sizeof(header) + align_word(size);
    while (cursor + sizeof(header) <= GROUP_BYTES) {
        const size_t address = group * GROUP_BYTES + cursor;
        if (!read_record(address, GROUP_BYTES - cursor, &header)) {
            // A torn record closes this group. An erased header is safe to append to.
            if (esp_partition_read(partition, address, &header, sizeof(header)) != ESP_OK ||
                header.type != UINT32_MAX || header.sequence != UINT32_MAX ||
                header.bytes != UINT32_MAX || header.crc != UINT32_MAX) cursor = GROUP_BYTES;
            break;
        }
        if (header.type != DELTA || header.sequence != sequence + 1 || !delta(header.bytes, false)) {
            cursor = GROUP_BYTES;
            break;
        }
        delta(header.bytes, true);
        sequence = header.sequence;
        cursor += sizeof(header) + align_word(header.bytes);
    }
    memcpy(state, previous, state_bytes);
    return true;
}

esp_err_t operations_save(const void *state)
{
    if (!previous || !payload) return ESP_ERR_INVALID_STATE;
    const uint8_t *current = state;
    size_t bytes = mask_bytes;
    uint32_t type = DELTA;
    memset(payload, 0, mask_bytes);
    if (sequence) {
        for (size_t pos = 0, block = 0; pos < state_bytes; pos += BLOCK_BYTES, ++block) {
            const size_t size = state_bytes - pos < BLOCK_BYTES ? state_bytes - pos : BLOCK_BYTES;
            if (!memcmp(current + pos, previous + pos, size)) continue;
            if (bytes + size >= state_bytes) {
                bytes = state_bytes;
                break;
            }
            payload[block / 8] |= 1u << (block % 8);
            memcpy(payload + bytes, current + pos, size);
            bytes += size;
        }
        if (bytes == mask_bytes) return ESP_OK;
    }
    const size_t next_group = !sequence ? 0 : (group + 1) % groups;
    size_t destination = group;
    size_t offset = cursor;
    if (!sequence || bytes == state_bytes || cursor + sizeof(record_header) + align_word(bytes) > GROUP_BYTES) {
        esp_err_t result = esp_partition_erase_range(partition, next_group * GROUP_BYTES, GROUP_BYTES);
        if (result != ESP_OK) return result;
        type = SNAPSHOT;
        bytes = state_bytes;
        destination = next_group;
        offset = 0;
        memcpy(payload, state, state_bytes);
    }
    memset(payload + bytes, 0, align_word(bytes) - bytes);
    record_header header = {.type = type, .sequence = sequence + 1, .bytes = bytes};
    header.crc = checksum(header);
    const size_t address = destination * GROUP_BYTES + offset;
    esp_err_t result = esp_partition_write(partition, address, &header, sizeof(header));
    if (result == ESP_OK) result = esp_partition_write(partition, address + sizeof(header), payload, align_word(bytes));
    if (result != ESP_OK) {
        cursor = GROUP_BYTES;
        return result;
    }
    memcpy(previous, state, state_bytes);
    group = destination;
    sequence = header.sequence;
    cursor = offset + sizeof(header) + align_word(bytes);
    return ESP_OK;
}

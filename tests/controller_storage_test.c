// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "timeline.h"
#include "operations_store.h"
#include "esp_partition.h"
#include "esp_crc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

enum { SECTOR = 4096, ARCHIVE_SECTORS = 136, OPERATIONS_SECTORS = 40, LEGACY_SECTORS = 14 };
static const esp_partition_t parts[] = {
    {.size = SECTOR * ARCHIVE_SECTORS, .erase_size = SECTOR, .index = 0},
    {.size = SECTOR * OPERATIONS_SECTORS, .erase_size = SECTOR, .index = 1},
    {.size = SECTOR * LEGACY_SECTORS, .erase_size = SECTOR, .index = 2},
};
static uint8_t *flash[sizeof(parts) / sizeof(*parts)];
static unsigned *erases[sizeof(parts) / sizeof(*parts)];
static size_t write_limit = SIZE_MAX;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && label);
    return subtype == 0x41 ? &parts[0] : subtype == 0x42 ? &parts[1] : NULL;
}

esp_err_t esp_partition_register_external(void *chip, size_t address, size_t size, const char *label,
    int type, int subtype, const esp_partition_t **out)
{
    assert(!chip && address == 0x12000 && size == 0xe000 && label && type == ESP_PARTITION_TYPE_DATA &&
           subtype == 0x40 && out);
    *out = &parts[2];
    return ESP_OK;
}

esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset, void *data, size_t size)
{
    assert(offset + size <= part->size);
    memcpy(data, flash[part->index] + offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset, const void *data, size_t size)
{
    assert(offset + size <= part->size);
    const unsigned written = size < write_limit ? size : write_limit;
    if (write_limit != SIZE_MAX) write_limit -= written;
    for (unsigned i = 0; i < written; ++i) {
        uint8_t *cell = flash[part->index] + offset + i;
        const uint8_t value = ((const uint8_t *)data)[i];
        assert((*cell & value) == value);
        *cell = value;
    }
    return written == size ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t offset, size_t size)
{
    assert(offset + size <= part->size && !(offset % SECTOR) && !(size % SECTOR));
    memset(flash[part->index] + offset, 0xff, size);
    for (unsigned i = offset / SECTOR; i < (offset + size) / SECTOR; ++i) ++erases[part->index][i];
    return ESP_OK;
}

uint16_t esp_crc16_le(uint16_t crc, const uint8_t *data, uint32_t len)
{
    while (len--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (crc & 1 ? 0xa001 : 0);
    }
    return crc;
}

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *data, uint32_t len)
{
    while (len--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return crc;
}

static void balanced(const unsigned *counts, unsigned sectors)
{
    unsigned minimum = counts[0], maximum = counts[0];
    for (unsigned i = 1; i < sectors; ++i) {
        if (counts[i] < minimum) minimum = counts[i];
        if (counts[i] > maximum) maximum = counts[i];
    }
    assert(minimum && maximum - minimum <= 1);
}

static void archive_test(void)
{
    enum { START = 6000000, DAYS = 40, END = START + DAYS * 288 };
    timeline_start(0);
    for (uint32_t bucket = START; bucket < END; ++bucket) {
        timeline_sample sample = {.productive = 128, .grid_decawatts = -4, .grid_min = -5,
            .grid_max = 2, .grid_count = 30};
        for (unsigned slot = 0; slot < DISPATCH_LIMIT; ++slot) {
            sample.miner[slot] = (timeline_miner){.identity = slot + 1, .watts = 100 + slot,
                .minimum = 80, .maximum = 130, .observations = 60, .paid_mwh = 300,
                .cost_millicents = 4, .status = TIMELINE_METERED};
        }
        timeline_sample_write(bucket, &sample);
        timeline_complete(bucket + 1);
        for (unsigned i = 0; i < 2; ++i) {
            timeline_queue_event((timeline_event){.epoch = bucket * 300 + i, .slot = 7});
        }
        timeline_flush_events();
    }
    timeline_start(0);
    for (unsigned tier = 0; tier < TIMELINE_TIERS; ++tier) {
        const unsigned ratio = timeline_period(tier) / TIMELINE_SECONDS;
        const uint32_t last = END / ratio;
        for (unsigned i = 1; i <= timeline_retention(tier); ++i) {
            timeline_sample sample;
            assert(timeline_sample_read(tier, last - i, &sample));
            assert(sample.productive == 128 && sample.grid_min == -5 && sample.grid_max == 2);
            const timeline_miner *slot = &sample.miner[7];
            assert(slot->identity == 8 && slot->watts == 107 && slot->minimum == 80 && slot->maximum == 130);
            assert(slot->observations == 60 * ratio && slot->paid_mwh == 300 * ratio && slot->cost_millicents == 4 * ratio);
        }
    }
    const uint32_t cursor = timeline_event_cursor();
    assert(cursor == DAYS * 288 * 2);
    for (unsigned i = 1; i <= 5865; ++i) {
        timeline_event event;
        assert(timeline_event_read(cursor - i, &event) && event.slot == 7);
    }
    const unsigned *counts = erases[0];
    balanced(counts, 31);
    balanced(counts + 31, 40);
    balanced(counts + 71, 41);
    balanced(counts + 112, 24);

    // Format a four-slot archive bucket that is only present in legacy flash.
    const uint32_t legacy_bucket = START - 1, legacy_group = legacy_bucket / 63;
    const size_t page = legacy_group % 12 * SECTOR;
    const uint32_t header[] = {0x44535031, legacy_group, ~legacy_group, 0};
    uint16_t old_sample[32] = {[6] = 0x5400};
    old_sample[7] = esp_crc16_le(0, (const uint8_t *)old_sample, sizeof(old_sample));
    assert(esp_partition_write(&parts[2], page, header, sizeof(header)) == ESP_OK);
    assert(esp_partition_write(&parts[2], page + (1 + legacy_bucket % 63) * sizeof(old_sample),
                               old_sample, sizeof(old_sample)) == ESP_OK);
    timeline_sample sample;
    assert(timeline_sample_read(0, legacy_bucket, &sample));

    timeline_queue_event((timeline_event){.epoch = END * 300, .slot = 7});
    timeline_clear(1);
    timeline_flush_events();
    assert(timeline_event_cursor() == 0);
    timeline_event event;
    assert(!timeline_event_read(cursor - 1, &event));
    assert(!timeline_sample_read(0, legacy_bucket, &sample));
    for (unsigned tier = 0; tier < TIMELINE_TIERS; ++tier) {
        const unsigned ratio = timeline_period(tier) / TIMELINE_SECONDS;
        assert(!timeline_sample_read(tier, END / ratio - 1, &sample));
    }
    // A clear near a bucket boundary can write a new sample at the same index.
    sample = (timeline_sample){.productive = 4, .spent_millicents = 4};
    timeline_sample_write(END - 1, &sample);
    timeline_complete(END);
    timeline_queue_event((timeline_event){.epoch = END * 300, .slot = 2});
    timeline_flush_events();
    timeline_start(1);
    assert(!timeline_sample_read(0, legacy_bucket, &sample));
    assert(timeline_event_cursor() == 1 && timeline_event_read(0, &event) && event.slot == 2);
    for (unsigned tier = 0; tier < TIMELINE_TIERS; ++tier) {
        const unsigned ratio = timeline_period(tier) / TIMELINE_SECONDS;
        assert(timeline_sample_read(tier, END / ratio - 1, &sample) && sample.spent_millicents == 4);
        assert(!timeline_sample_read(tier, END / ratio - 2, &sample));
    }
    timeline_clear(2);
    timeline_start(2);
    assert(timeline_event_cursor() == 0 && !timeline_sample_read(0, END - 1, &sample));
}

static void commit_attempt(bool first, bool torn)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        static uint8_t state[9952];
        assert(operations_load(state, sizeof(state)) == !first);
        if (first) {
            memset(state, 0xa5, sizeof(state));
            if (torn) write_limit = 24;
            assert(operations_save(state) == (torn ? ESP_FAIL : ESP_OK));
            if (torn) _exit(0);
            for (unsigned i = 0; i < 10000; ++i) {
                state[9944] ^= 1;
                assert(operations_save(state) == ESP_OK);
            }
        } else {
            assert(state[0] == 0xa5 && state[9944] == 0xa5);
            state[9944] ^= 1;
            if (torn) write_limit = 24;
            assert(operations_save(state) == (torn ? ESP_FAIL : ESP_OK));
        }
        _exit(0);
    }
    int result;
    assert(waitpid(pid, &result, 0) == pid && WIFEXITED(result) && WEXITSTATUS(result) == 0);
}

int main(void)
{
    for (unsigned i = 0; i < sizeof(parts) / sizeof(*parts); ++i) {
        flash[i] = mmap(NULL, parts[i].size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        erases[i] = mmap(NULL, parts[i].size / SECTOR * sizeof(unsigned), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        assert(flash[i] != MAP_FAILED && erases[i] != MAP_FAILED);
        memset(flash[i], 0xff, parts[i].size);
    }
    archive_test();
    commit_attempt(true, true);
    // An interrupted first snapshot still leaves the previous format available.
    memset(erases[1], 0, OPERATIONS_SECTORS * sizeof(*erases[1]));
    commit_attempt(true, false);
    balanced(erases[1], OPERATIONS_SECTORS);
    commit_attempt(false, true);
    commit_attempt(false, false);
    static uint8_t state[9952];
    assert(operations_load(state, sizeof(state)) && state[9944] == (0xa5 ^ 1));
    puts("Eight-slot archive retention, clearing, extrema, cost, wear rotation, and torn journal recovery passed");
}

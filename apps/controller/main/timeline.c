// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "timeline.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <stdatomic.h>

// Reserved gap on the DISPATCH/01 flash map. Existing controllers acquire this
// partition at runtime after the partition layer verifies size and non-overlap.
enum { OFFSET = 0x12000, SECTOR = 4096, SAMPLE_SECTORS = 12, EVENT_SECTORS = 2,
       BYTES = (SAMPLE_SECTORS + EVENT_SECTORS) * SECTOR, SUBTYPE = 0x40,
       EVENT_QUEUE = 24, EVENT_FLUSH = 4 };
enum { SAMPLE_MAGIC = 0x44535031, EVENT_MAGIC = 0x44534531 };
enum { SAMPLES_PER_SECTOR = SECTOR / sizeof(timeline_sample) - 1,
       EVENTS_PER_SECTOR = SECTOR / sizeof(timeline_event) - 1 };
_Static_assert(sizeof(timeline_sample) == 64, "Five-minute flash record");
_Static_assert(sizeof(timeline_event) == 16, "Event flash record");
_Static_assert(SAMPLES_PER_SECTOR * (SAMPLE_SECTORS - 1) >= TIMELINE_BUCKETS, "48 hour window");

typedef struct { uint32_t magic, group, inverse, reserved; } page_header;
static const esp_partition_t *history_partition;
static atomic_uint_least32_t event_cursor;
static timeline_event pending[EVENT_QUEUE];
static unsigned queue_head, queue_count;
static bool failure_logged;

static uint32_t page_address(bool event, uint32_t group)
{
    return (event ? SAMPLE_SECTORS + group % EVENT_SECTORS : group % SAMPLE_SECTORS) * SECTOR;
}

static bool read_header(uint32_t address, page_header *header)
{
    return esp_partition_read(history_partition, address, header, sizeof(*header)) == ESP_OK;
}

static bool valid_header(page_header header, bool event, uint32_t group)
{
    return header.magic == (event ? EVENT_MAGIC : SAMPLE_MAGIC) &&
        header.group == group && header.inverse == ~group && header.reserved == 0;
}

static bool prepare_page(uint32_t address, bool event, uint32_t group)
{
    page_header header;
    if (!read_header(address, &header)) return false;
    if (valid_header(header, event, group)) return true;
    if (valid_header(header, event, header.group) && header.group > group) return false;
    header = (page_header){event ? EVENT_MAGIC : SAMPLE_MAGIC, group, ~group, 0};
    return esp_partition_erase_range(history_partition, address, SECTOR) == ESP_OK &&
        esp_partition_write(history_partition, address, &header, sizeof(header)) == ESP_OK;
}

// Never rewrite a slot, including a write interrupted by loss of power.
// A read and CRC check on the reader side discard any torn record.
static bool write_record(uint32_t address, const void *data, size_t size)
{
    uint32_t existing[sizeof(timeline_sample) / sizeof(uint32_t)];
    if (esp_partition_read(history_partition, address, existing, size) != ESP_OK) return false;
    for (unsigned i = 0; i < size / sizeof(*existing); ++i) {
        if (existing[i] != UINT32_MAX) return false;
    }
    return esp_partition_write(history_partition, address, data, size) == ESP_OK;
}

static void report_failure(void)
{
    if (!failure_logged) ESP_LOGW("timeline", "A history record could not be saved");
    failure_logged = true;
}

static uint16_t sample_crc(timeline_sample sample)
{
    sample.crc = 0;
    return esp_crc16_le(0, (const uint8_t *)&sample, sizeof(sample));
}

static uint16_t event_crc(timeline_event event)
{
    event.crc = 0;
    return esp_crc16_le(0, (const uint8_t *)&event, sizeof(event));
}

void timeline_start(void)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, SUBTYPE, "history");
    if (!partition &&
        esp_partition_register_external(NULL, OFFSET, BYTES, "history", ESP_PARTITION_TYPE_DATA, SUBTYPE,
                                        &partition) != ESP_OK) return;
    if (!partition || partition->size != BYTES || partition->erase_size != SECTOR ||
        partition->encrypted || partition->readonly) return;
    history_partition = partition;
    uint32_t newest = 0;
    for (unsigned i = 0; i < EVENT_SECTORS; ++i) {
        uint32_t address = (SAMPLE_SECTORS + i) * SECTOR;
        page_header header;
        if (!read_header(address, &header) || !valid_header(header, true, header.group) ||
            header.group % EVENT_SECTORS != i || header.group >= UINT32_MAX / EVENTS_PER_SECTOR) continue;
        // Include any touched slot after a torn write so the next sequence never rewrites it.
        for (unsigned j = 0; j < EVENTS_PER_SECTOR; ++j) {
            uint32_t words[sizeof(timeline_event) / sizeof(uint32_t)];
            if (esp_partition_read(partition, address + (j + 1) * sizeof(timeline_event), words, sizeof(words)) != ESP_OK) break;
            if (words[0] == UINT32_MAX && words[1] == UINT32_MAX && words[2] == UINT32_MAX && words[3] == UINT32_MAX) continue;
            uint32_t next = header.group * EVENTS_PER_SECTOR + j + 1;
            if (next > newest) newest = next;
        }
    }
    atomic_store(&event_cursor, newest);
}

bool timeline_available(void)
{
    return history_partition != NULL;
}

bool timeline_sample_read(uint32_t bucket, timeline_sample *sample)
{
    if (!history_partition) return false;
    const uint32_t group = bucket / SAMPLES_PER_SECTOR;
    const uint32_t address = page_address(false, group);
    const uint32_t offset = address + (bucket % SAMPLES_PER_SECTOR + 1) * sizeof(*sample);
    page_header header, after;
    return read_header(address, &header) && valid_header(header, false, group) &&
        esp_partition_read(history_partition, offset, sample, sizeof(*sample)) == ESP_OK &&
        read_header(address, &after) && valid_header(after, false, group) &&
        (sample->flags & 0xff00) == TIMELINE_SAMPLE_TAG && sample->crc == sample_crc(*sample);
}

void timeline_sample_write(uint32_t bucket, timeline_sample *sample)
{
    if (!history_partition) return;
    timeline_sample existing;
    if (timeline_sample_read(bucket, &existing)) return;
    const uint32_t group = bucket / SAMPLES_PER_SECTOR;
    const uint32_t address = page_address(false, group);
    sample->crc = sample_crc(*sample);
    if (!prepare_page(address, false, group) ||
        !write_record(address + (bucket % SAMPLES_PER_SECTOR + 1) * sizeof(*sample), sample, sizeof(*sample))) report_failure();
}

uint32_t timeline_event_cursor(void)
{
    return atomic_load(&event_cursor);
}

bool timeline_event_read(uint32_t sequence, timeline_event *event)
{
    const uint32_t cursor = timeline_event_cursor();
    if (!history_partition || sequence >= cursor || cursor - sequence > EVENT_SECTORS * EVENTS_PER_SECTOR) return false;
    const uint32_t group = sequence / EVENTS_PER_SECTOR;
    const uint32_t address = page_address(true, group);
    page_header header;
    return read_header(address, &header) && valid_header(header, true, group) &&
        esp_partition_read(history_partition, address + (sequence % EVENTS_PER_SECTOR + 1) * sizeof(*event),
                           event, sizeof(*event)) == ESP_OK && event->sequence == sequence && event->crc == event_crc(*event);
}

void timeline_queue_event(timeline_event event)
{
    if (!history_partition) return;
    if (queue_count == EVENT_QUEUE) {
        report_failure();
        return;
    }
    pending[(queue_head + queue_count++) % EVENT_QUEUE] = event;
}

void timeline_flush_events(void)
{
    for (unsigned i = 0; i < EVENT_FLUSH && queue_count; ++i) {
        timeline_event event = pending[queue_head];
        event.sequence = timeline_event_cursor();
        event.crc = event_crc(event);
        const uint32_t group = event.sequence / EVENTS_PER_SECTOR;
        const uint32_t address = page_address(true, group);
        if (!prepare_page(address, true, group) ||
            !write_record(address + (event.sequence % EVENTS_PER_SECTOR + 1) * sizeof(event), &event, sizeof(event))) report_failure();
        atomic_store(&event_cursor, event.sequence + 1);
        queue_head = (queue_head + 1) % EVENT_QUEUE;
        --queue_count;
    }
}

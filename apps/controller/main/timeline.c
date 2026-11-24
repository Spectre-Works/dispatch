// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "timeline.h"

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <stdatomic.h>
#include <string.h>

enum { SECTOR = 4096, OLD_SAMPLE_SECTORS = 12, OLD_EVENT_SECTORS = 2, OLD_TAG = 0x5400,
       EVENT_QUEUE = 32, EVENT_FLUSH = 4 };
typedef struct { uint32_t magic, group, inverse, reserved; } page_header;
typedef struct { unsigned first, sectors, stride; uint32_t magic; } flash_ring;

static const flash_ring samples[TIMELINE_TIERS] = {
    {0, 31, sizeof(timeline_sample), 0x44534111},
    {31, 40, sizeof(timeline_sample), 0x44534112},
    {71, 41, sizeof(timeline_sample), 0x44534113},
};
static const flash_ring events = {112, 24, sizeof(timeline_event), 0x44534104};

typedef struct {
    uint32_t paid_mwh;
    uint16_t identity, watts, cost_centicents;
    uint8_t reason, status;
} old_miner;

typedef struct {
    uint32_t spent_millicents, allowance_millicents;
    uint16_t solar_decawatts;
    int16_t grid_decawatts;
    uint16_t flags, crc;
    old_miner miner[4];
} old_sample;

static const flash_ring old_samples = {0, OLD_SAMPLE_SECTORS, sizeof(old_sample), 0x44535031};
static const flash_ring old_events = {OLD_SAMPLE_SECTORS, OLD_EVENT_SECTORS, sizeof(timeline_event), 0x44534531};
_Static_assert(sizeof(old_sample) == 64 && sizeof(timeline_event) == 16, "Legacy activity layout");
_Static_assert(sizeof(timeline_sample) == 196, "Archive record layout");
_Static_assert(((SECTOR - sizeof(page_header)) / sizeof(timeline_sample)) * 30 >= TIMELINE_BUCKETS, "48-hour ring");
_Static_assert(((SECTOR - sizeof(page_header)) / sizeof(timeline_sample)) * 39 >= 7 * 24 * 4, "Seven-day ring");
_Static_assert(((SECTOR - sizeof(page_header)) / sizeof(timeline_sample)) * 40 >= 30 * 24, "Thirty-day ring");
static const esp_partition_t *archive, *legacy;
static atomic_uint_least32_t event_cursor;
static timeline_event pending[EVENT_QUEUE];
static unsigned queue_head, queue_count;
static bool failure_logged;

unsigned timeline_period(unsigned tier)
{
    static const unsigned periods[] = {300, 900, 3600};
    return periods[tier];
}

unsigned timeline_retention(unsigned tier)
{
    static const unsigned buckets[] = {576, 672, 720};
    return buckets[tier];
}

static unsigned header_space(const flash_ring *ring)
{
    return ring == &old_samples || ring == &old_events ? ring->stride : sizeof(page_header);
}

static unsigned capacity(const flash_ring *ring)
{
    return (SECTOR - header_space(ring)) / ring->stride;
}

static bool valid(page_header header, const flash_ring *ring, uint32_t group)
{
    return header.magic == ring->magic && header.group == group &&
        header.inverse == ~group && header.reserved == 0;
}

static bool read_record(const esp_partition_t *part, const flash_ring *ring, uint32_t index, void *record)
{
    if (!part) return false;
    const unsigned count = capacity(ring);
    const uint32_t group = index / count;
    const uint32_t page = (ring->first + group % ring->sectors) * SECTOR;
    page_header before, after;
    return esp_partition_read(part, page, &before, sizeof(before)) == ESP_OK && valid(before, ring, group) &&
        esp_partition_read(part, page + header_space(ring) + index % count * ring->stride, record, ring->stride) == ESP_OK &&
        esp_partition_read(part, page, &after, sizeof(after)) == ESP_OK && valid(after, ring, group);
}

static bool erased(const uint32_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (words[i] != UINT32_MAX) return false;
    return true;
}

static bool append(const flash_ring *ring, uint32_t index, const void *record)
{
    const unsigned count = capacity(ring);
    const uint32_t group = index / count;
    const uint32_t page = (ring->first + group % ring->sectors) * SECTOR;
    const uint32_t address = page + header_space(ring) + index % count * ring->stride;
    page_header header;
    if (esp_partition_read(archive, page, &header, sizeof(header)) != ESP_OK) return false;
    if (!valid(header, ring, group)) {
        if (valid(header, ring, header.group) && header.group > group) return false;
        header = (page_header){ring->magic, group, ~group, 0};
        if (esp_partition_erase_range(archive, page, SECTOR) != ESP_OK ||
            esp_partition_write(archive, page, &header, sizeof(header)) != ESP_OK) return false;
    }
    uint32_t existing[sizeof(timeline_sample) / 4];
    return esp_partition_read(archive, address, existing, ring->stride) == ESP_OK &&
        erased(existing, ring->stride / 4) && esp_partition_write(archive, address, record, ring->stride) == ESP_OK;
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

static uint32_t last_event(const esp_partition_t *part, const flash_ring *ring)
{
    if (!part) return 0;
    const unsigned count = capacity(ring);
    page_header header;
    uint32_t group = 0;
    bool found = false;
    for (unsigned i = 0; i < ring->sectors; ++i) {
        const uint32_t page = (ring->first + i) * SECTOR;
        if (esp_partition_read(part, page, &header, sizeof(header)) != ESP_OK ||
            !valid(header, ring, header.group) || header.group % ring->sectors != i ||
            header.group >= UINT32_MAX / count || (found && header.group < group)) continue;
        group = header.group;
        found = true;
    }
    if (!found) return 0;
    for (unsigned j = count; j; --j) {
        uint32_t words[sizeof(timeline_event) / 4];
        if (read_record(part, ring, group * count + j - 1, words) &&
            !erased(words, sizeof(words) / sizeof(*words))) return group * count + j;
    }
    return group * count;
}

void timeline_start(void)
{
    archive = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "archive");
    if (archive && (archive->size != (events.first + events.sectors) * SECTOR ||
        archive->erase_size != SECTOR || archive->encrypted || archive->readonly)) archive = NULL;
    legacy = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "history");
    if (!legacy) esp_partition_register_external(NULL, 0x12000, 14 * SECTOR, "history",
                                                  ESP_PARTITION_TYPE_DATA, 0x40, &legacy);
    if (legacy && legacy->size != 14 * SECTOR) legacy = NULL;
    const uint32_t prior = last_event(legacy, &old_events), current = last_event(archive, &events);
    atomic_store(&event_cursor, prior > current ? prior : current);
}

bool timeline_available(void)
{
    return archive || legacy;
}

bool timeline_sample_read(unsigned tier, uint32_t bucket, timeline_sample *sample)
{
    if (read_record(archive, &samples[tier], bucket, sample) &&
        sample->crc == sample_crc(*sample)) return true;
    if (tier) return false;
    old_sample old;
    if (!read_record(legacy, &old_samples, bucket, &old) || (old.flags & 0xff00) != OLD_TAG) return false;
    const uint16_t crc = old.crc;
    old.crc = 0;
    if (crc != esp_crc16_le(0, (const uint8_t *)&old, sizeof(old))) return false;
    *sample = (timeline_sample){
        .spent_millicents = old.spent_millicents, .allowance_millicents = old.allowance_millicents,
        .solar_decawatts = old.solar_decawatts, .grid_decawatts = old.grid_decawatts,
        .solar_min = old.solar_decawatts, .solar_max = old.solar_decawatts,
        .grid_min = old.grid_decawatts, .grid_max = old.grid_decawatts,
        .solar_count = old.solar_decawatts != TIMELINE_UNKNOWN_WATTS,
        .grid_count = old.grid_decawatts != TIMELINE_UNKNOWN_GRID,
        .flags = old.flags & 15, .productive = (old.flags >> 4) & 15,
    };
    for (unsigned i = 0; i < 4; ++i) {
        const old_miner *m = &old.miner[i];
        sample->miner[i] = (timeline_miner){
            .paid_mwh = m->paid_mwh, .cost_millicents = m->cost_centicents * 10u,
            .identity = m->identity, .watts = m->watts, .minimum = m->watts, .maximum = m->watts,
            .observations = m->identity && m->watts != TIMELINE_UNKNOWN_WATTS,
            .reason = m->reason, .status = m->status,
        };
    }
    return true;
}

static void summarize(unsigned tier, unsigned ratio, uint32_t bucket)
{
    timeline_sample total = {0}, row;
    if (timeline_sample_read(tier, bucket, &row)) return;
    uint32_t solar = 0, watts[DISPATCH_LIMIT] = {0};
    int32_t grid = 0;
    unsigned observed = 0;
    for (unsigned i = 0; i < ratio; ++i) {
        if (!timeline_sample_read(tier - 1, bucket * ratio + i, &row)) continue;
        ++observed;
        total.flags |= row.flags & (TIMELINE_PARTIAL | TIMELINE_ACCOUNTING_GAP);
        total.flags = (total.flags & ~(TIMELINE_BUDGET | TIMELINE_BRIDGE)) |
            (row.flags & (TIMELINE_BUDGET | TIMELINE_BRIDGE));
        total.spent_millicents = row.spent_millicents;
        total.allowance_millicents = row.allowance_millicents;
        total.productive = row.productive;
        total.replaced_mwh += row.replaced_mwh;
        total.replaced_millicents += row.replaced_millicents;
        if (row.solar_count) {
            if (!total.solar_count || row.solar_min < total.solar_min) total.solar_min = row.solar_min;
            if (!total.solar_count || row.solar_max > total.solar_max) total.solar_max = row.solar_max;
            solar += (uint32_t)row.solar_decawatts * row.solar_count;
            total.solar_count += row.solar_count;
        }
        if (row.grid_count) {
            if (!total.grid_count || row.grid_min < total.grid_min) total.grid_min = row.grid_min;
            if (!total.grid_count || row.grid_max > total.grid_max) total.grid_max = row.grid_max;
            grid += (int32_t)row.grid_decawatts * row.grid_count;
            total.grid_count += row.grid_count;
        }
        for (unsigned slot = 0; slot < DISPATCH_LIMIT; ++slot) {
            const timeline_miner *src = &row.miner[slot];
            timeline_miner *dst = &total.miner[slot];
            if (!src->identity) continue;
            if (dst->identity && dst->identity != src->identity) {
                total.replaced_mwh += dst->paid_mwh;
                total.replaced_millicents += dst->cost_millicents;
                *dst = (timeline_miner){0};
                watts[slot] = 0;
                total.flags |= TIMELINE_PARTIAL;
            }
            dst->identity = src->identity;
            dst->paid_mwh += src->paid_mwh;
            dst->cost_millicents += src->cost_millicents;
            dst->reason = (dst->reason & ~TIMELINE_REASON_MASK) | src->reason;
            dst->status = (dst->status & (TIMELINE_METERED | TIMELINE_CATCHUP)) | src->status;
            if (!src->observations) continue;
            if (!dst->observations || src->minimum < dst->minimum) dst->minimum = src->minimum;
            if (!dst->observations || src->maximum > dst->maximum) dst->maximum = src->maximum;
            watts[slot] += (uint32_t)src->watts * src->observations;
            dst->observations += src->observations;
        }
    }
    if (!observed) return;
    if (observed < ratio) total.flags |= TIMELINE_PARTIAL;
    total.solar_decawatts = total.solar_count ? solar / total.solar_count : TIMELINE_UNKNOWN_WATTS;
    total.grid_decawatts = total.grid_count ? grid / total.grid_count : TIMELINE_UNKNOWN_GRID;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        timeline_miner *m = &total.miner[i];
        m->watts = m->observations ? watts[i] / m->observations : TIMELINE_UNKNOWN_WATTS;
    }
    total.crc = sample_crc(total);
    if (!append(&samples[tier], bucket, &total)) report_failure();
}

void timeline_sample_write(uint32_t bucket, timeline_sample *sample)
{
    if (!archive) return;
    timeline_sample existing;
    if (timeline_sample_read(0, bucket, &existing)) return;
    sample->crc = sample_crc(*sample);
    if (!append(&samples[0], bucket, sample)) {
        report_failure();
        return;
    }
}

void timeline_complete(uint32_t open_bucket)
{
    if (!archive || open_bucket < 12) return;
    // Idempotent at bucket boundaries and after a restart between writes.
    const uint32_t fifteen_minute = open_bucket / 3 - 1;
    const uint32_t hour = open_bucket / 12 - 1;
    summarize(1, 3, fifteen_minute);
    summarize(2, 4, hour);
}

uint32_t timeline_event_cursor(void)
{
    return atomic_load(&event_cursor);
}

unsigned timeline_event_capacity(void)
{
    return archive ? events.sectors * capacity(&events) : old_events.sectors * capacity(&old_events);
}

bool timeline_event_read(uint32_t sequence, timeline_event *event)
{
    const uint32_t cursor = timeline_event_cursor();
    if (sequence >= cursor || cursor - sequence > timeline_event_capacity()) return false;
    if (read_record(archive, &events, sequence, event) &&
        event->sequence == sequence && event->crc == event_crc(*event)) return true;
    if (!read_record(legacy, &old_events, sequence, event) ||
        event->sequence != sequence || event->crc != event_crc(*event)) return false;
    if (event->slot == 4) event->slot = DISPATCH_LIMIT;
    return true;
}

void timeline_queue_event(timeline_event event)
{
    if (!archive) return;
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
        if (!append(&events, event.sequence, &event)) report_failure();
        atomic_store(&event_cursor, event.sequence + 1);
        queue_head = (queue_head + 1) % EVENT_QUEUE;
        --queue_count;
    }
}

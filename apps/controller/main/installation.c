// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "installation.h"
#include "integration_profile.h"

#include "discovery.h"
#include "fronius.h"
#include "network.h"
#include "preset_credentials.h"
#include "timeline.h"
#include "operations_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { INSTALLATION_VERSION = 30, LEGACY_VERSION = 29, HOME_METER_VERSION = 28, PREVIOUS_VERSION = 27, POLL_SECONDS = 5, CHECKPOINT_SECONDS = 300,
       RETRY_SECONDS = 15, RETRY_LIMIT = 2, WORKER_STACK = 8192, PROFILE_SECONDS = 30, PATH_POLL_SECONDS = 30,
       DESTINATION_POLL_SECONDS = 30, PATH_FRESH_SECONDS = 95, IDENTITY_SECONDS = 300,
       STALL_SECONDS = 600, RECOVERY_OFF_SECONDS = 120,
       GRID_POLL = 3, GRID_FRESH = 12, GRID_SETTLE = 20, GRID_IMPORT_SECONDS = 15,
       MODE_FRESH_SECONDS = 300, MODE_SETTLE_SECONDS = 30, MODE_VERIFY_SECONDS = 30, MODE_RECHECK_SECONDS = 5,
       PRESET_PROBE_SECONDS = 120, PRESET_CONFIRM_SECONDS = 180, PRESET_SOLAR_DWELL = 600,
       PRESET_RETRY_SECONDS = 300, PRESET_LOAD_SECONDS = 120,
       SOLAR_DAY_POLL = 300, SOLAR_NIGHT_POLL = 1200, SOLAR_FRESH = 900, SOLAR_RETRY = 5,
       SOLAR_START_RESERVATION = 30, BRIDGE_SOLAR_EVIDENCE_SECONDS = 600 };
enum { PRESET_BINDING_BYTES = 192 };
enum { TEST_CONFIRM_SECONDS = 20, TEST_HASH_WARMUP = 30, TEST_METER_WARMUP = 120,
       TEST_HASH_STABLE = 15, TEST_METER_STABLE = 30 };

typedef struct {
    char model[24], firmware[16], hardware[16];
    uint16_t default_clock, reduced_clock;
    uint8_t alternate, boards;
} preset_identity;

typedef struct {
    preset_identity identity;
    dispatch_preset_evidence mode[PRESET_COUNT];
} miner_preset_evidence;

typedef struct {
    uint8_t slot, restore;
    bool active, restoring, canceled;
} preset_evaluation;

// The v27–29 NVS representation has four slots, independent of the active limit.
enum { LEGACY_SLOTS = 4 };
typedef struct {
    dispatch_policy policy;
    int16_t utc_offset_minutes;
    association device[LEGACY_SLOTS];
} legacy_settings;
typedef struct {
    dispatch_meter_anchor anchor[LEGACY_SLOTS];
    uint64_t used_nano_cents;
    uint32_t day;
    uint8_t gap_mask;
} legacy_budget;
typedef struct {
    uint16_t reserve, shed_margin, start_watts[LEGACY_SLOTS];
    uint8_t reserved;
    uint16_t restart_seconds, shed_delay_seconds;
} legacy_follow;
typedef struct {
    uint32_t day;
    dispatch_preset_totals device[LEGACY_SLOTS];
} legacy_preset_day;
typedef struct {
    uint32_t version;
    legacy_settings settings;
    legacy_budget budget;
    uint64_t pause_until;
    uint32_t pause_unclocked;
    uint8_t hold_mask, hold_on;
    bool enabled;
    uint8_t attempt_counts;
    _Alignas(uint64_t) char timezone[TIMEZONE_NAME_BYTES];
    dispatch_quiet_hours quiet;
    _Alignas(uint64_t) dispatch_tariff tariff;
    uint32_t highest_rate_day;
    dispatch_profile profile[LEGACY_SLOTS];
    dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS];
    slot_coordination coordination[LEGACY_SLOTS];
    mining_settings mining;
    _Alignas(uint64_t) bool budget_date_pending;
    _Alignas(uint64_t) solar_settings solar;
    grid_settings grid;
    _Alignas(uint64_t) uint8_t first_run[LEGACY_SLOTS];
    bool pause_indefinite;
    _Alignas(uint64_t) legacy_follow follow;
    _Alignas(uint64_t) solar_daily_evidence solar_days[DISPATCH_EVIDENCE_DAYS];
    _Alignas(uint64_t) bool plan_ready;
    _Alignas(uint64_t) dispatch_efficiency efficiency[LEGACY_SLOTS];
    char equipment_name[LEGACY_SLOTS][EQUIPMENT_NAME_BYTES];
    _Alignas(uint64_t) dispatch_weekend_plan weekend;
    _Alignas(uint64_t) notification_target notifications;
    _Alignas(uint64_t) miner_preset_evidence presets[LEGACY_SLOTS];
    legacy_preset_day preset_days[DISPATCH_EVIDENCE_DAYS];
    _Alignas(uint64_t) uint8_t reserved_preset, preset_authorized, pinned_preset[LEGACY_SLOTS];
    _Alignas(uint64_t) preset_evaluation evaluation;
    _Alignas(uint64_t) daylight_settings daylight;
    uint64_t bridge_started;
    _Alignas(uint64_t) dispatch_power_test_result power_test[LEGACY_SLOTS];
    uint8_t test_armed;
    _Alignas(uint64_t) uint8_t reserved_policy;
    _Alignas(uint64_t) dispatch_operating_plan operating;
    uint8_t preset_choice[LEGACY_SLOTS];
    _Alignas(uint64_t) home_settings home;
    mqtt_endpoint mqtt;
    home_meter_mapping home_meter[LEGACY_SLOTS];
    uint64_t profile_revision, grid_profile_revision;
} legacy_installation;

typedef struct {
    uint32_t version;
    installation_settings settings;
    dispatch_budget budget;
    uint64_t pause_until;
    uint32_t pause_unclocked;
    uint8_t hold_mask, hold_on;
    bool enabled;
    uint16_t attempt_counts;
    _Alignas(uint64_t) char timezone[TIMEZONE_NAME_BYTES];
    dispatch_quiet_hours quiet;
    _Alignas(uint64_t) dispatch_tariff tariff;
    uint32_t highest_rate_day;
    dispatch_profile profile[DISPATCH_LIMIT];
    dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS];
    slot_coordination coordination[DISPATCH_LIMIT];
    mining_settings mining;
    _Alignas(uint64_t) bool budget_date_pending;
    _Alignas(uint64_t) solar_settings solar;
    grid_settings grid;
    _Alignas(uint64_t) uint8_t first_run[DISPATCH_LIMIT];
    bool pause_indefinite;
    _Alignas(uint64_t) follow_settings follow;
    _Alignas(uint64_t) solar_daily_evidence solar_days[DISPATCH_EVIDENCE_DAYS];
    _Alignas(uint64_t) bool plan_ready;
    _Alignas(uint64_t) dispatch_efficiency efficiency[DISPATCH_LIMIT];
    char equipment_name[DISPATCH_LIMIT][EQUIPMENT_NAME_BYTES];
    _Alignas(uint64_t) dispatch_weekend_plan weekend;
    _Alignas(uint64_t) notification_target notifications;
    _Alignas(uint64_t) miner_preset_evidence presets[DISPATCH_LIMIT];
    preset_daily_evidence preset_days[DISPATCH_EVIDENCE_DAYS];
    _Alignas(uint64_t) uint8_t reserved_preset, preset_authorized, pinned_preset[DISPATCH_LIMIT];
    _Alignas(uint64_t) preset_evaluation evaluation;
    _Alignas(uint64_t) daylight_settings daylight;
    uint64_t bridge_started;
    _Alignas(uint64_t) dispatch_power_test_result power_test[DISPATCH_LIMIT];
    uint8_t test_armed;
    _Alignas(uint64_t) uint8_t reserved_policy;
    _Alignas(uint64_t) dispatch_operating_plan operating;
    uint8_t preset_choice[DISPATCH_LIMIT];
    _Alignas(uint64_t) home_settings home;
    mqtt_endpoint mqtt;
    home_meter_mapping home_meter[DISPATCH_LIMIT];
    uint64_t profile_revision, grid_profile_revision;
} installation_store;

_Static_assert(sizeof(grid_settings) == 136 && offsetof(grid_settings, direction_verified) == 135,
               "Native grid settings reuse reserved bytes");
// These offsets define the current installation record. Reserved bytes keep that
// record stable when a stored option is retired.
_Static_assert(offsetof(legacy_installation, first_run) == 1816, "Installation equipment layout");
_Static_assert(offsetof(legacy_installation, home) == 0xef8, "Version 27 record size");
_Static_assert(offsetof(legacy_installation, solar_days) == 1848, "Installation solar layout");
static const solar_settings default_solar = {.port = 8899, .night_start = 20 * 60, .night_end = 5 * 60};
static const grid_settings default_grid = {.reserve = 150};
static const follow_settings default_follow = {
    .reserve = 600, .shed_margin = 100, .restart_seconds = FOLLOW_RESTART_DEFAULT,
    .shed_delay_seconds = FOLLOW_SHED_DELAY_DEFAULT,
};
static const dispatch_quiet_hours default_quiet = {.start_minute = 22 * 60, .end_minute = 7 * 60};

typedef struct {
    device_view view;
    uint32_t since, solar_off_since, startup_at, next_command, connection_epoch, observation_day, next_profile;
    uint32_t next_probe, next_miner;
    uint8_t outlet_failures, miner_failures;
    bool startup_tracking;
    uint32_t zero_since, next_destination, destination_at, recovery_requested_at;
    dispatch_work_monitor work;
    struct {
        dispatch_preset_totals pending, window;
        uint32_t at, day;
        double watts, mhs;
    } preset_sample;
    bool recovery_exhausted, notice_destination;
    uint32_t notice_recovery_day;
    uint32_t power_checked, power_settle_until, next_power_probe, power_probe_epoch;
    uint32_t preset_mode_since, preset_next_attempt, preset_reserved_until, preset_reserved_watts;
    miner_destination destination;
    dispatch_action pending;
    unsigned tries;
} device_runtime;

static installation_store state, rollback;
static const dispatch_timezone *active_timezone;
static device_runtime runtime[DISPATCH_LIMIT];
static notification_queue notifications;
typedef enum { POWER_INSPECT, POWER_MANUAL, POWER_AUTHORIZE, POWER_SOLAR_LOWER, POWER_SOLAR_RESTORE,
               POWER_EVALUATE, POWER_EFFICIENCY, POWER_BRIDGE_LOWER } power_source;
static struct {
    bool pending, canceled;
    unsigned slot;
    uint32_t revision, epoch, verify_until, next_verify;
    goldshell_mode_reading expected;
    goldshell_mode desired;
    power_source source;
    char password[GOLDSHELL_PASSWORD_BYTES];
} power_job;
static struct {
    goldshell_mode_reading expected;
    integration_command command;
    goldshell_mode_status result;
    uint32_t revision, epoch, deadline, sent, before;
    unsigned slot;
    char password[GOLDSHELL_PASSWORD_BYTES];
} restart_job;
static uint32_t solar_preset_shed_until;
static uint8_t bridge_preset_attempts, adaptive_preset_attempts;
static uint32_t bridge_preset_after;
static struct {
    node_reading node;
    datum_reading datum;
    path_status identity;
    uint32_t node_at, datum_at, next_node, next_datum, next_identity, epoch;
    bool node_sampled, datum_sampled;
} path;
static struct {
    ecu_reading reading;
    uint32_t at, cycle_at, retry_at, revision, epoch, progress_at, supported_watts, higher_watts;
    uint32_t low_since, low_epoch;
    uint32_t lit_at, lit_epoch;
    ecu_result result;
    bool sampled, cycled, progress_seen, sustained, low_tracking;
} solar;
static struct {
    int64_t day;
    dispatch_daylight today, tomorrow;
} daylight_cache;
static struct {
    int32_t watts, previous_watts;
    uint32_t at, epoch, poll_at, revision, import_since, covered_at;
    shelly_result result;
    uint32_t fronius_watts, fronius_at;
    bool sampled, attempted, importing, consecutive, fronius_known;
} grid;
enum { SOURCE_GRACE_SECONDS = 60 };
static struct {
    int32_t observed, expected, requested;
    uint32_t deadline, next_poll, epoch, at;
    shelly_result result;
    integration_command command;
    bool known;
} gx;
static struct {
    ha_control_state observed;
    double battery, temperature, requested;
    char option[HA_OPTION_BYTES], before[HA_OPTION_BYTES];
    uint32_t epoch, revision, next_poll, deadline, at[3];
    bool known[3];
    uint8_t next;
    integration_command command;
    shelly_result result;
} home;
static struct {
    char identity[40];
    uint32_t revision;
    shelly_result result;
    bool pending, attempted, on;
} mqtt_probe;
static uint32_t last_usable_power;
static ecu_query solar_query = {.fd = -1};
static uint32_t query_revision, query_epoch;
static uint32_t next_energy_change;
static struct {
    uint32_t at, epoch;
    uint32_t watt_seconds[DISPATCH_LIMIT];
    uint32_t first_credit[DISPATCH_LIMIT];
} solar_credit;

static bool grid_fresh(uint32_t now);
static bool operating_grid(void);
static bool paused_at(time_t now);
static uint8_t preset_choice(unsigned slot);
static bool preset_authorized(unsigned slot);
static bool bridge_preset_efficiency(unsigned slot);
static uint32_t measured_preset_watts(unsigned slot, goldshell_mode mode);
static bool fresh(const device_runtime *device, uint32_t now);
static bool admit_on(void);
static bool restart_ready(unsigned slot, uint32_t now);
static history_event history[HISTORY_SIZE];
static unsigned history_count, history_next;
static uint32_t generation, next_start, next_checkpoint, worker_stack;
static bool dirty, storage_error;
static atomic_bool pause_requested;
static nvs_handle_t nvs;
static SemaphoreHandle_t mutex;
static struct {
    uint32_t bucket;
    timeline_sample sample;
    uint64_t cost[DISPATCH_LIMIT];
    uint32_t watts[DISPATCH_LIMIT], solar_count, grid_count;
    int64_t solar_watts, grid_watts;
    uint8_t samples[DISPATCH_LIMIT];
    bool observed;
} telemetry;
static bool bridge_active;
static uint8_t paid_authority;
static struct {
    dispatch_power_test_samples samples;
    dispatch_power_test_result result;
    uint32_t revision, started, lease, last_read, retry_off, next_save;
    unsigned slot;
    power_test_phase phase, outcome;
    const char *detail;
} power_test;

static bool testing_outlet(unsigned slot)
{
    return power_test.slot == slot && power_test.phase >= POWER_TEST_QUEUED &&
        power_test.phase <= POWER_TEST_STOPPING;
}

uint16_t installation_equipment_identity(const association *device)
{
    if (!device->shelly.ip[0]) return 0;
    uint32_t hash = 2166136261U;
    const char *fields[] = {device->shelly.identity, device->miner_ip,
        outlet_identity(device->shelly.identity) == OUTLET_HA ? device->shelly.ip : NULL};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        if (!fields[i]) continue;
        for (const unsigned char *c = (const unsigned char *)fields[i]; *c; ++c) {
            hash = (hash ^ *c) * 16777619U;
        }
        hash *= 16777619U;
    }
    return (uint16_t)(hash % UINT16_MAX + 1);
}

static bool telemetry_advance(time_t utc)
{
    if (!utc || !timeline_available()) return false;
    const uint32_t bucket = utc / TIMELINE_SECONDS;
    if (bucket < telemetry.bucket) return false;
    if (bucket == telemetry.bucket) return true;
    if (telemetry.bucket && telemetry.observed) {
        timeline_sample *sample = &telemetry.sample;
        if (bucket > telemetry.bucket + 1) sample->flags |= TIMELINE_PARTIAL;
        sample->solar_decawatts = telemetry.solar_count ?
            telemetry.solar_watts / telemetry.solar_count / 10 : TIMELINE_UNKNOWN_WATTS;
        sample->grid_decawatts = telemetry.grid_count ?
            telemetry.grid_watts / telemetry.grid_count / 10 : TIMELINE_UNKNOWN_GRID;
        sample->solar_count = telemetry.solar_count;
        sample->grid_count = telemetry.grid_count;
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            timeline_miner *miner = &sample->miner[i];
            miner->watts = telemetry.samples[i] ? telemetry.watts[i] / telemetry.samples[i] : TIMELINE_UNKNOWN_WATTS;
            const uint64_t cost = telemetry.cost[i] / 1000000;
            miner->cost_millicents = cost > UINT32_MAX ? UINT32_MAX : cost;
            miner->observations = telemetry.samples[i];
        }
        timeline_sample_write(telemetry.bucket, sample);
    }
    timeline_complete(bucket);
    const bool partial = !telemetry.bucket || bucket > telemetry.bucket + 1;
    memset(&telemetry, 0, sizeof(telemetry));
    telemetry.bucket = bucket;
    telemetry.sample.flags = partial && utc % TIMELINE_SECONDS > 10 ? TIMELINE_PARTIAL : 0;
    return true;
}

static timeline_miner *telemetry_miner(unsigned slot)
{
    timeline_miner *miner = &telemetry.sample.miner[slot];
    const uint16_t identity = installation_equipment_identity(&state.settings.device[slot]);
    if (miner->identity != identity) {
        if (miner->identity) {
            telemetry.sample.flags |= TIMELINE_PARTIAL;
            telemetry.sample.replaced_mwh += miner->paid_mwh;
            telemetry.sample.replaced_millicents += telemetry.cost[slot] / 1000000;
        }
        *miner = (timeline_miner){.identity = identity};
        telemetry.cost[slot] = telemetry.watts[slot] = telemetry.samples[slot] = 0;
    }
    return miner;
}

static void archive_event(unsigned slot, event_kind kind, dispatch_action action, unsigned reason, time_t utc)
{
    if (!utc) return;
    switch (kind) {
        case EVENT_OUTLET_ON: case EVENT_OUTLET_OFF: case EVENT_OUTLET_LOST:
        case EVENT_COMMAND_ON: case EVENT_COMMAND_OFF: case EVENT_COMMAND_ERROR: case EVENT_RECOVERY_ACK:
        case EVENT_MINER_RESTART: case EVENT_MINER_RESTART_CONFIRMED:
        case EVENT_DEFAULT_MODE: case EVENT_LOW_MODE: case EVENT_BALANCE_MODE: case EVENT_IDLE_MODE:
        case EVENT_POWER_MODE_UNCONFIRMED: case EVENT_PRESET_SOLAR_LOWER: case EVENT_PRESET_SOLAR_RESTORE:
        case EVENT_PRESET_EVAL_APPLIED: case EVENT_PRESET_EFFICIENT: case EVENT_PRESET_BRIDGE_LOWER:
        case EVENT_DAYLIGHT_BRIDGE_STARTED: case EVENT_DAYLIGHT_BRIDGE_REARMED: case EVENT_DAYLIGHT_BRIDGE_ENDED:
        case EVENT_ACCOUNTING_CLEARED:
        case EVENT_POWER_TEST_STARTED: case EVENT_POWER_TEST_COMPLETE:
        case EVENT_POWER_TEST_CANCELED: case EVENT_POWER_TEST_FAILED:
            timeline_queue_event((timeline_event){.epoch = utc, .slot = slot, .kind = kind, .action = action,
                .reason = reason, .identity = slot < DISPATCH_LIMIT ? installation_equipment_identity(&state.settings.device[slot]) : 0});
            break;
        default: break;
    }
}

static void reset_solar_credit(void)
{
    memset(&solar_credit, 0, sizeof(solar_credit));
}

static dispatch_daily_evidence *day_evidence(uint32_t day)
{
    const unsigned index = day % DISPATCH_EVIDENCE_DAYS;
    if (state.days[index].day != day) {
        state.solar_days[index] = (solar_daily_evidence){0};
        dirty = true;
    }
    return dispatch_evidence_day(state.days, day);
}

static void lock(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(mutex);
}

static bool local_time(time_t utc, uint32_t *day, uint32_t *minute)
{
    return (!state.timezone[0] || active_timezone) &&
        timezone_local(active_timezone, state.settings.utc_offset_minutes, utc, day, minute);
}

static bool local_minute(uint64_t epoch, uint32_t *minute)
{
    uint32_t day;
    return local_time(epoch, &day, minute);
}

static bool tariff_local_time(uint64_t epoch, uint32_t *day, uint32_t *minute)
{
    return local_time(epoch, day, minute);
}

static bool uses_rate_windows(void)
{
    for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
        if (state.tariff.window[i].enabled || (state.weekend.enabled && state.weekend.tariff.window[i].enabled)) {
            return true;
        }
    }
    return false;
}

static void queue_exception(unsigned index, event_kind kind, uint32_t day, uint32_t now)
{
    if (!state.notifications.ip[0] || index >= DISPATCH_LIMIT) return;
    device_runtime *device = &runtime[index];
    notification_kind notice;
    switch (kind) {
        case EVENT_COMMAND_HOLD: notice = NOTICE_SWITCH_HELD; break;
        case EVENT_DESTINATION_MATCH: device->notice_destination = false; return;
        case EVENT_DESTINATION_CHANGED:
            if (device->notice_destination) return;
            device->notice_destination = true;
            notice = NOTICE_DESTINATION_CHANGED;
            break;
        case EVENT_RECOVERY_EXHAUSTED:
        case EVENT_RECOVERY_ERROR:
            if (!day || device->notice_recovery_day == day) return;
            device->notice_recovery_day = day;
            notice = NOTICE_RECOVERY_EXHAUSTED;
            break;
        case EVENT_WORK_LOST: notice = NOTICE_WORK_MISSING; break;
        default: return;
    }
    notification_enqueue(&notifications, notice, index, now);
}

static void record_action(unsigned index, event_kind kind, dispatch_action action, unsigned reason)
{
    uint32_t day = 0, minute = 0;
    const time_t utc = network_clock();
    const uint32_t now = network_uptime();
    local_time(utc, &day, &minute);
    history[history_next] = (history_event){
        .uptime = now, .local_day = day, .local_minute = minute,
        .device = index, .kind = kind, .action = action, .reason = reason, .local_second = utc % 60,
    };
    history_next = (history_next + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) {
        ++history_count;
    }
    queue_exception(index, kind, day, now);
    archive_event(index, kind, action, reason, utc);
}

static void record(unsigned index, event_kind kind)
{
    record_action(index, kind, DISPATCH_KEEP, EVENT_NO_REASON);
}

static uint32_t reconcile_budget_date(time_t utc)
{
    uint32_t day;
    if (!local_time(utc, &day, NULL)) return 0;
    if (state.budget_date_pending || day < state.budget.day) {
        const uint32_t previous = state.budget.day;
        const dispatch_daily_evidence *today = &state.days[day % DISPATCH_EVIDENCE_DAYS];
        // Keep the measured totals, meter anchors and any known gaps.
        dispatch_budget_carry(&state.budget, day, today->day == day ? today->cost_nano_cents : 0);
        state.budget_date_pending = false;
        dirty = true;
        if (previous != day) {
            record(DISPATCH_LIMIT, EVENT_BUDGET_DATE);
            ESP_LOGI("dispatch", "Electricity budget carried from day %lu to %lu",
                     (unsigned long)previous, (unsigned long)day);
        }
    }
    return day;
}

const char *installation_event(event_kind kind)
{
    static const char *const notes[] = {
        [EVENT_OUTLET_ON] = "Observed outlet on", [EVENT_OUTLET_OFF] = "Observed outlet off",
        [EVENT_OUTLET_LOST] = "Outlet observation unavailable", [EVENT_MINER_LOST] = "Miner API unavailable",
        [EVENT_MINER_RESPONDING] = "Miner API responded", [EVENT_COMMAND_ON] = "On request acknowledged",
        [EVENT_COMMAND_OFF] = "Off request acknowledged", [EVENT_COMMAND_ERROR] = "No successful command acknowledgement",
        [EVENT_CONFIRMED] = "Requested outlet state observed", [EVENT_COMMAND_HOLD] = "Command held after two unconfirmed attempts",
        [EVENT_COUNTER_GAP] = "Electricity meter continuity lost", [EVENT_CONFIGURED] = "Configuration saved in Preview",
        [EVENT_PREVIEW] = "Preview enabled", [EVENT_ENABLED] = "Owner enabled outlet control",
        [EVENT_PAUSED] = "Owner paused operation", [EVENT_RESUMED] = "Operation pause ended",
        [EVENT_STORAGE_ERROR] = "Storage unavailable. Switching suspended",
        [EVENT_DESTINATION_CHANGED] = "Miner reports a different active destination",
        [EVENT_DESTINATION_MATCH] = "Miner reports the configured DATUM gateway",
        [EVENT_RECOVERY_ATTEMPT] = "Recovery attempt reserved",
        [EVENT_RECOVERY_ACK] = "Recovery off request acknowledged",
        [EVENT_RECOVERY_ERROR] = "Recovery off request unconfirmed. Attempt limit reached",
        [EVENT_COORDINATION] = "Miner coordination updated",
        [EVENT_PATH_CONFIGURED] = "Mining path settings updated",
        [EVENT_BUDGET_DATE] = "Electricity budget carried to the corrected date",
        [EVENT_METER_RECOVERED] = "Owner acknowledged a meter gap and resumed electricity accounting",
        [EVENT_ACCOUNTING_CLEARED] = "Owner cleared electricity accounting and daily reports",
        [EVENT_MAPPING_CONFIRMED] = "Owner confirmed the outlet-to-miner mapping",
        [EVENT_FIRST_POWER] = "First powered outlet observation recorded",
        [EVENT_FIRST_WORK] = "First positive recent hashrate reported by the miner",
        [EVENT_RECOVERY_OFF_OBSERVED] = "Outlet observed off after the recovery request",
        [EVENT_WORK_LOST] = "Powered miner has stopped reporting positive recent hashrate",
        [EVENT_WORK_RESTORED] = "Miner is reporting positive recent hashrate again",
        [EVENT_RECOVERY_EXHAUSTED] = "Reported work has not resumed after today's recovery attempt",
        [EVENT_DEFAULT_MODE] = "Miner confirmed Default power mode",
        [EVENT_LOW_MODE] = "Miner confirmed Low-Power mode",
        [EVENT_BALANCE_MODE] = "Miner confirmed Balance power mode",
        [EVENT_IDLE_MODE] = "Miner confirmed Idle and paused hashing",
        [EVENT_POWER_MODE_UNCONFIRMED] = "Miner power-mode change could not be confirmed",
        [EVENT_PRESET_AUTHORIZED] = "Owner authorized managed presets for this miner",
        [EVENT_PRESET_AUTH_FAILED] = "Miner preset authorization failed. Check the password",
        [EVENT_PRESET_REVOKED] = "Owner revoked managed preset access",
        [EVENT_PRESET_STRATEGY] = "Owner updated the installation preset strategy",
        [EVENT_PRESET_EVAL_STARTED] = "Owner started a preset comparison",
        [EVENT_PRESET_EVAL_FINISHED] = "Preset comparison finished and the starting preset was restored",
        [EVENT_PRESET_EVAL_CANCELED] = "Preset comparison canceled",
        [EVENT_PRESET_SOLAR_LOWER] = "Reduced miner draw to stay within available solar",
        [EVENT_PRESET_SOLAR_RESTORE] = "Restored Default with sustained solar headroom",
        [EVENT_PRESET_BRIDGE_LOWER] = "Switched to a more efficient preset to reduce paid draw during daylight coverage",
        [EVENT_MINER_RESTART] = "Requested a miner restart",
        [EVENT_MINER_RESTART_CONFIRMED] = "Observed a new mining process after restart",
        [EVENT_PRESET_EVAL_APPLIED] = "Applied a preset for the efficiency comparison",
        [EVENT_PRESET_EFFICIENT] = "Applied the owner-selected efficiency preset",
        [EVENT_DAYLIGHT_BRIDGE_STARTED] = "Daylight gap coverage started for running miners",
        [EVENT_DAYLIGHT_BRIDGE_REARMED] = "Sustained solar restored daylight gap coverage",
        [EVENT_DAYLIGHT_BRIDGE_ENDED] = "Daylight gap coverage ended",
        [EVENT_POWER_TEST_STARTED] = "Owner-approved first-run power test started",
        [EVENT_POWER_TEST_COMPLETE] = "First-run power test recorded the measured draw",
        [EVENT_POWER_TEST_CANCELED] = "First-run power test canceled",
        [EVENT_POWER_TEST_FAILED] = "First-run power test ended without saving a measurement",
    };
    return notes[kind];
}

static esp_err_t save(void)
{
    const esp_err_t result = operations_save(&state);
    if (result != ESP_OK && !storage_error) {
        record(DISPATCH_LIMIT, EVENT_STORAGE_ERROR);
    }
    storage_error = result != ESP_OK;
    if (!storage_error) {
        dirty = false;
    }
    next_checkpoint = network_uptime() + CHECKPOINT_SECONDS;
    return result;
}

static bool same_device(const association *a, const association *b)
{
    return !strcmp(a->shelly.identity, b->shelly.identity) &&
        !strcmp(a->shelly.ip, b->shelly.ip) && !strcmp(a->miner_ip, b->miner_ip);
}

static void clear_preset_evidence(unsigned slot)
{
    state.presets[slot] = (miner_preset_evidence){0};
    for (unsigned i = 0; i < DISPATCH_EVIDENCE_DAYS; ++i) {
        state.preset_days[i].device[slot] = (dispatch_preset_totals){0};
    }
}

static void forget_preset_authorization(unsigned slot)
{
    state.preset_authorized &= ~(1u << slot);
    state.preset_choice[slot] = PRESET_MANUAL;
    state.pinned_preset[slot] = GOLDSHELL_MODE_UNKNOWN;
    if (state.evaluation.active && state.evaluation.slot == slot) state.evaluation.active = false;
    if (preset_credentials_available(slot)) preset_credentials_forget(slot);
    dirty = true;
}

static bool duplicate_device(const association *a, const association *b)
{
    return a->shelly.ip[0] && b->shelly.ip[0] &&
        (outlet_same_target(&a->shelly, &b->shelly) ||
         (a->miner_ip[0] && !strcmp(a->miner_ip, b->miner_ip)));
}

static esp_err_t configure(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                           const dispatch_tariff *tariff, const dispatch_weekend_plan *weekend,
                           const dispatch_timezone *timezone,
                           unsigned first, unsigned count, const association *devices,
                           bool keep_password, bool importing, int offset, const operating_settings *operating)
{
    lock();
    if ((operating && (state.evaluation.active || testing_outlet(power_test.slot))) ||
        (operating && !importing && operating->daylight.enabled && !state.solar.ip[0] && !operating_grid()) ||
        (policy && !operating)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (testing_outlet(power_test.slot) && power_test.slot >= first && power_test.slot < first + count) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const association *a = i >= first && i < first + count ? &devices[i - first] : &state.settings.device[i];
        const outlet_kind kind = outlet_identity(a->shelly.identity);
        if (!importing && i >= first && i < first + count &&
            ((kind == OUTLET_HA && strcmp(a->shelly.ip, state.home.endpoint.ip)) ||
             (kind == OUTLET_MQTT && strcmp(a->shelly.ip, state.mqtt.ip)))) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
        for (unsigned j = 0; j < i; ++j) {
            const association *b = j >= first && j < first + count ? &devices[j - first] : &state.settings.device[j];
            if (duplicate_device(a, b)) {
                unlock();
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    rollback = state;
    unsigned changed = 0;
    for (unsigned i = first; i < first + count; ++i) {
        association *old = &state.settings.device[i];
        association next = devices[i - first];
        if (!same_device(old, &next)) {
            changed |= 1u << i;
            state.first_run[i] = 0;
            state.follow.start_watts[i] = 0;
            state.profile[i] = (dispatch_profile){0};
            state.power_test[i] = (dispatch_power_test_result){0};
            state.efficiency[i] = (dispatch_efficiency){0};
            clear_preset_evidence(i);
            state.preset_authorized &= ~(1u << i);
            state.preset_choice[i] = PRESET_MANUAL;
            state.pinned_preset[i] = GOLDSHELL_MODE_UNKNOWN;
            if (state.evaluation.active && state.evaluation.slot == i) state.evaluation.active = false;
            state.coordination[i] = (slot_coordination){.priority = state.coordination[i].priority};
            if (strcmp(old->shelly.identity, next.shelly.identity) || strcmp(old->shelly.ip, next.shelly.ip)) {
                state.home_meter[i] = (home_meter_mapping){0};
                if (state.budget.anchor[i].epoch) {
                    state.budget.gap_mask |= 1u << i;
                }
                state.budget.anchor[i] = (dispatch_meter_anchor){0};
            }
        }
        if (!next.shelly.ip[0] || strcmp(old->shelly.identity, next.shelly.identity)) {
            memset(state.equipment_name[i], 0, sizeof(state.equipment_name[i]));
        }
        if (keep_password && !strcmp(old->shelly.ip, next.shelly.ip) &&
            !strcmp(old->shelly.identity, next.shelly.identity)) {
            memcpy(next.shelly.password, old->shelly.password, sizeof(next.shelly.password));
        }
        *old = next;
        explicit_bzero(&next, sizeof(next));
    }
    if (policy) {
        state.settings.policy = *policy;
        state.plan_ready = true;
    }
    if (quiet) state.quiet = *quiet;
    if (tariff) state.tariff = *tariff;
    if (weekend) state.weekend = *weekend;
    if (operating) {
        state.operating = operating->plan;
        state.daylight = operating->daylight;
        if (rollback.daylight.enabled != state.daylight.enabled ||
            rollback.daylight.latitude_e4 != state.daylight.latitude_e4 ||
            rollback.daylight.longitude_e4 != state.daylight.longitude_e4) state.bridge_started = 0;
    }
    if (importing) {
        state.settings.utc_offset_minutes = offset;
        state.timezone[0] = 0;
    }
    if (timezone) strcpy(state.timezone, timezone->name);
    if (state.budget.day && (strcmp(state.timezone, rollback.timezone) ||
        (!state.timezone[0] && state.settings.utc_offset_minutes != rollback.settings.utc_offset_minutes))) {
        state.budget_date_pending = true;
    }
    state.enabled = false;
    state.hold_mask = 0;
    state.hold_on = 0;
    state.attempt_counts = 0;
    esp_err_t result = save();
    if (result == ESP_OK) {
        if (timezone || importing) active_timezone = timezone;
        reset_solar_credit();
        last_usable_power = 0;
        if (operating) {
            daylight_cache.day = 0;
            solar.low_tracking = false;
            adaptive_preset_attempts = 0;
            solar_preset_shed_until = 0;
        }
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (changed & (1u << i)) {
                forget_preset_authorization(i);
                runtime[i] = (device_runtime){0};
                if (power_test.slot == i) power_test.phase = POWER_TEST_IDLE;
                for (unsigned event = 0; event < history_count; ++event) {
                    if (history[event].device == i) history[event].previous_equipment = true;
                }
            }
            runtime[i].pending = DISPATCH_KEEP;
            runtime[i].tries = 0;
            runtime[i].next_probe = 0;
        }
        ++generation;
        record(importing || !count ? DISPATCH_LIMIT : first, EVENT_CONFIGURED);
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_configure(unsigned index, const association *device, bool keep_password)
{
    return index < DISPATCH_LIMIT ? configure(NULL, NULL, NULL, NULL, NULL, index, 1,
        device, keep_password, false, 0, NULL) : ESP_ERR_INVALID_ARG;
}

esp_err_t installation_set_plan(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                               const dispatch_tariff *tariff, const dispatch_weekend_plan *weekend,
                               const dispatch_timezone *timezone, const operating_settings *operating)
{
    return configure(policy, quiet, tariff, weekend, timezone, 0, 0, NULL, false, false, 0, operating);
}

esp_err_t installation_import(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                              const dispatch_tariff *tariff, const dispatch_weekend_plan *weekend,
                              const dispatch_timezone *timezone,
                              int offset, const association devices[DISPATCH_LIMIT], const operating_settings *operating)
{
    return configure(policy, quiet, tariff, weekend, timezone, 0, DISPATCH_LIMIT, devices, true, true, offset, operating);
}

esp_err_t installation_mining(const mining_settings *settings)
{
    lock();
    mining_settings next = *settings;
    if (next.node_ip[0] && !next.rpc_user[0]) {
        if (strcmp(next.node_ip, state.mining.node_ip) || strcmp(next.node_host, state.mining.node_host) ||
            next.node_port != state.mining.node_port ||
            !state.mining.rpc_user[0]) {
            unlock();
            explicit_bzero(&next, sizeof(next));
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(next.rpc_user, state.mining.rpc_user);
        strcpy(next.rpc_password, state.mining.rpc_password);
    }
    rollback = state;
    state.mining = next;
    explicit_bzero(&next, sizeof(next));
    const esp_err_t result = save();
    if (result == ESP_OK) {
        memset(&path, 0, sizeof(path));
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            runtime[i].destination = DESTINATION_UNKNOWN;
            runtime[i].next_destination = 0;
            runtime[i].zero_since = 0;
            runtime[i].notice_destination = false;
        }
        ++generation;
        record(DISPATCH_LIMIT, EVENT_PATH_CONFIGURED);
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_notifications(const notification_target *target)
{
    lock();
    notification_target next = *target;
    if (next.ip[0] && !next.path[0]) {
        if (strcmp(next.ip, state.notifications.ip) || next.port != state.notifications.port ||
            !state.notifications.path[0]) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(next.path, state.notifications.path, sizeof(next.path));
    }
    if (next.port == state.notifications.port && !strcmp(next.ip, state.notifications.ip) &&
        !strcmp(next.path, state.notifications.path)) {
        explicit_bzero(&next, sizeof(next));
        unlock();
        return ESP_OK;
    }
    notification_target previous = state.notifications;
    state.notifications = next;
    const esp_err_t result = save();
    if (result == ESP_OK) {
        notifications = (notification_queue){.sequence = notifications.sequence};
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            runtime[i].notice_destination = false;
            runtime[i].notice_recovery_day = 0;
        }
    } else {
        state.notifications = previous;
        dirty = true;
    }
    explicit_bzero(&previous, sizeof(previous));
    explicit_bzero(&next, sizeof(next));
    unlock();
    return result;
}

esp_err_t installation_notification_test(void)
{
    lock();
    const bool queued = state.notifications.ip[0] &&
        notification_enqueue(&notifications, NOTICE_TEST, 0, network_uptime());
    unlock();
    return queued ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void energy_preview(void)
{
    state.enabled = false;
    state.hold_mask = state.hold_on = state.attempt_counts = 0;
}

static void clear_energy_commands(void)
{
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        runtime[i].pending = DISPATCH_KEEP;
        runtime[i].tries = 0;
    }
}

esp_err_t installation_solar(const solar_settings *settings)
{
    lock();
    const bool rearm = !operating_grid();
    if ((!settings->ip[0] && rearm && state.daylight.enabled) ||
        (settings->ip[0] && !settings->port &&
         (strcmp(settings->ip, state.home.endpoint.ip) || !state.home.endpoint.token[0]))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rollback = state;
    state.solar = *settings;
    if (rearm) energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        const uint32_t revision = solar.revision + 1;
        memset(&solar, 0, sizeof(solar));
        solar.revision = revision;
        if (rearm) last_usable_power = 0;
        reset_solar_credit();
        if (rearm) clear_energy_commands();
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

static esp_err_t save_power_settings(void)
{
    energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        solar.low_tracking = false;
        last_usable_power = 0;
        solar_preset_shed_until = 0;
        adaptive_preset_attempts = 0;
        reset_solar_credit();
        clear_energy_commands();
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    return result;
}

esp_err_t installation_follow(const follow_settings *settings)
{
    lock();
    rollback = state;
    state.follow = *settings;
    const esp_err_t result = save_power_settings();
    unlock();
    return result;
}

esp_err_t installation_power_policy(const follow_settings *settings, uint16_t grid_reserve)
{
    lock();
    rollback = state;
    state.follow.reserve = settings->reserve;
    state.follow.shed_margin = settings->shed_margin;
    state.follow.restart_seconds = settings->restart_seconds;
    state.follow.shed_delay_seconds = settings->shed_delay_seconds;
    state.grid.reserve = grid_reserve;
    const esp_err_t result = save_power_settings();
    unlock();
    return result;
}

esp_err_t installation_startup_draw(unsigned slot, uint16_t identity, uint16_t watts)
{
    if (slot >= DISPATCH_LIMIT || watts > 10000) return ESP_ERR_INVALID_ARG;
    lock();
    if (!identity || identity != installation_equipment_identity(&state.settings.device[slot])) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rollback = state;
    state.follow.start_watts[slot] = watts;
    const esp_err_t result = save_power_settings();
    unlock();
    return result;
}

bool installation_grid_identity(const char *identity)
{
    return grid_shelly_identity(identity) || victron_grid_phases(identity) || !strcmp(identity, "fronius-solar-api") ||
        ha_valid_entity(identity, HA_SENSOR);
}

static uint64_t grid_profile_revision(const char *identity)
{
    if (victron_grid_phases(identity)) return integration_profile_revision(PROFILE_VICTRON);
    if (!strcmp(identity, "fronius-solar-api")) return integration_profile_revision(PROFILE_FRONIUS);
    if (grid_shelly_identity(identity) && strncmp(identity, "shellyem3-", 10))
        return integration_profile_revision(PROFILE_SHELLY_RPC);
    return 0;
}

static uint64_t operating_profile_revision(void)
{
    uint64_t combined = 0;
    for (profile_family family = PROFILE_SHELLY_GEN1; family < PROFILE_HOME_ASSISTANT; ++family)
        combined = combined * 31u ^ integration_profile_revision(family);
    return combined;
}

static ha_entity_kind actuator_kind(const char *entity)
{
    if (ha_valid_entity(entity, HA_NUMBER)) return HA_NUMBER;
    if (ha_valid_entity(entity, HA_SELECT)) return HA_SELECT;
    return HA_BUTTON;
}

static bool integration_busy(integration_command command)
{
    return command == COMMAND_QUEUED || command == COMMAND_VERIFYING;
}

esp_err_t installation_mqtt_probe(const char *identity)
{
    if (outlet_identity(identity) != OUTLET_MQTT) return ESP_ERR_INVALID_ARG;
    lock();
    if (!state.mqtt.ip[0] || mqtt_probe.pending || !network_online()) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    strcpy(mqtt_probe.identity, identity);
    mqtt_probe.pending = true;
    mqtt_probe.attempted = false;
    unlock();
    return ESP_OK;
}

esp_err_t installation_mqtt(const mqtt_endpoint *endpoint, bool keep_credentials)
{
    lock();
    const bool same_host = !strcmp(endpoint->ip, state.mqtt.ip) && endpoint->port == state.mqtt.port &&
        !strcmp(endpoint->tls_name, state.mqtt.tls_name);
    bool used = false;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i)
        used |= outlet_identity(state.settings.device[i].shelly.identity) == OUTLET_MQTT;
    if ((used && !same_host && state.mqtt.ip[0]) ||
        (keep_credentials && (!same_host || !state.mqtt.user[0]))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rollback = state;
    state.mqtt = *endpoint;
    if (keep_credentials) state.mqtt = rollback.mqtt;
    if (used) energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        ++mqtt_probe.revision;
        mqtt_probe.pending = mqtt_probe.attempted = false;
    }
    if (result == ESP_OK && used) {
        clear_energy_commands();
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (outlet_identity(state.settings.device[i].shelly.identity) != OUTLET_MQTT) continue;
            runtime[i].view.observed = false;
            runtime[i].next_probe = 0;
        }
        ++generation;
    } else if (result != ESP_OK) {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_ha(const home_settings *settings, bool keep_token)
{
    lock();
    const bool same_host = !strcmp(settings->endpoint.ip, state.home.endpoint.ip) &&
        settings->endpoint.port == state.home.endpoint.port;
    const bool solar_bridge = state.solar.ip[0] && !state.solar.port;
    bool used = solar_bridge || ha_valid_entity(state.grid.target.identity, HA_SENSOR);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i)
        used |= outlet_identity(state.settings.device[i].shelly.identity) == OUTLET_HA;
    if ((used && !same_host && state.home.endpoint.ip[0]) ||
        (keep_token && (!same_host || !state.home.endpoint.token[0]))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rollback = state;
    state.home = *settings;
    if (keep_token) strcpy(state.home.endpoint.token, rollback.home.endpoint.token);
    if (used) energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        const uint32_t revision = home.revision + 1;
        memset(&home, 0, sizeof(home));
        home.revision = revision;
        if (used) {
            clear_energy_commands();
            for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
                if (outlet_identity(state.settings.device[i].shelly.identity) != OUTLET_HA) continue;
                runtime[i].view.observed = false;
                runtime[i].next_probe = 0;
            }
            if (ha_valid_entity(state.grid.target.identity, HA_SENSOR)) {
                const uint32_t grid_revision = grid.revision + 1;
                memset(&grid, 0, sizeof(grid));
                grid.revision = grid_revision;
                reset_solar_credit();
                last_usable_power = 0;
            }
            if (solar_bridge) {
                const uint32_t solar_revision = solar.revision + 1;
                memset(&solar, 0, sizeof(solar));
                solar.revision = solar_revision;
                reset_solar_credit();
                last_usable_power = 0;
            }
            ++generation;
        }
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_ha_meter(unsigned slot, uint16_t identity, const home_meter_mapping *mapping)
{
    if (slot >= DISPATCH_LIMIT) return ESP_ERR_INVALID_ARG;
    lock();
    const association *device = &state.settings.device[slot];
    if (outlet_identity(device->shelly.identity) != OUTLET_HA || !identity ||
        installation_equipment_identity(device) != identity || testing_outlet(slot)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (i != slot && ((mapping->power[0] && !strcmp(state.home_meter[i].power, mapping->power)) ||
                         (mapping->energy[0] && !strcmp(state.home_meter[i].energy, mapping->energy)))) {
            unlock();
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (mapping->power[0] && !strcmp(state.grid.target.identity, mapping->power)) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    home_meter_mapping *stored = &state.home_meter[slot];
    if (!strcmp(stored->power, mapping->power) && !strcmp(stored->energy, mapping->energy)) {
        unlock();
        return ESP_OK;
    }
    rollback = state;
    if (strcmp(stored->energy, mapping->energy) && state.budget.anchor[slot].epoch) {
        state.budget.anchor[slot] = (dispatch_meter_anchor){0};
        state.budget.gap_mask |= 1u << slot;
    }
    *stored = *mapping;
    energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        clear_energy_commands();
        runtime[slot].view.observed = false;
        runtime[slot].next_probe = 0;
        ++generation;
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_ha_action(const char *entity, double value, const char *option)
{
    lock();
    if (!state.home.endpoint.ip[0] || !state.home.actuator[0] || strcmp(entity, state.home.actuator) ||
        !home.known[2] || network_uptime() - home.at[2] > OBSERVATION_TTL || integration_busy(home.command) ||
        home.epoch != network_epoch() || !admit_on() || paused_at(network_clock())) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const ha_entity_kind kind = actuator_kind(entity);
    if ((kind == HA_NUMBER && (value < home.observed.min || value > home.observed.max)) ||
        (kind == HA_SELECT && !option[0])) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    home.requested = value;
    strcpy(home.option, option);
    strcpy(home.before, home.observed.text);
    home.command = COMMAND_QUEUED;
    home.result = SHELLY_OK;
    home.deadline = network_uptime() + 20;
    unlock();
    return ESP_OK;
}

esp_err_t installation_gx_target(const char *ip, int32_t expected, int32_t watts)
{
    if (watts < -VICTRON_TARGET_MAX || watts > VICTRON_TARGET_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    if (strcmp(ip, state.grid.target.ip) || !victron_grid_phases(state.grid.target.identity) ||
        !state.grid.direction_verified || !grid_fresh(network_uptime()) || !gx.known || gx.epoch != network_epoch() ||
        network_uptime() - gx.at > 2 * GRID_FRESH || expected != gx.observed ||
        integration_busy(gx.command) || !admit_on() || paused_at(network_clock())) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    gx.expected = expected;
    gx.requested = watts;
    gx.command = COMMAND_QUEUED;
    gx.result = SHELLY_OK;
    gx.deadline = network_uptime() + 20;
    unlock();
    return ESP_OK;
}

esp_err_t installation_grid(const grid_settings *settings, bool keep_password, bool keep_reserve)
{
    lock();
    const bool same_meter = !strcmp(settings->target.ip, state.grid.target.ip) &&
        !strcmp(settings->target.identity, state.grid.target.identity) &&
        (!native_grid_type(settings->target.identity) ||
         (settings->reverse == state.grid.reverse && !memcmp(&settings->native, &state.grid.native, sizeof(settings->native))));
    if (ha_valid_entity(settings->target.identity, HA_SENSOR)) {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (!strcmp(state.home_meter[i].power, settings->target.identity)) {
                unlock();
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    if ((ha_valid_entity(settings->target.identity, HA_SENSOR) && strcmp(settings->target.ip, state.home.endpoint.ip)) ||
        (settings->direction_verified && (!same_meter || (!operating_grid() && !grid_fresh(network_uptime())))) ||
        (!settings->direction_verified && state.daylight.enabled && !state.solar.ip[0])) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rollback = state;
    const bool rearm = operating_grid() || settings->direction_verified;
    state.grid = *settings;
    if (keep_reserve) state.grid.reserve = rollback.grid.reserve;
    state.grid_profile_revision = grid_profile_revision(settings->target.identity);
    if (keep_password && same_meter) {
        strcpy(state.grid.target.password, rollback.grid.target.password);
    }
    if (operating_grid() != rollback.grid.direction_verified || !same_meter)
        state.bridge_started = 0;
    if (rearm) energy_preview();
    const esp_err_t result = save();
    if (result == ESP_OK) {
        const uint32_t revision = grid.revision + 1;
        memset(&grid, 0, sizeof(grid));
        grid.revision = revision;
        memset(&gx, 0, sizeof(gx));
        reset_solar_credit();
        last_usable_power = 0;
        if (rearm) clear_energy_commands();
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

esp_err_t installation_coordination(unsigned slot, unsigned priority, bool recovery)
{
    if (slot >= DISPATCH_LIMIT || priority > 2) return ESP_ERR_INVALID_ARG;
    lock();
    if (!state.settings.device[slot].shelly.ip[0] || (recovery && !state.settings.device[slot].miner_ip[0])) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    slot_coordination previous = state.coordination[slot];
    state.coordination[slot].priority = priority;
    state.coordination[slot].recovery = recovery;
    const esp_err_t result = save();
    if (result == ESP_OK) {
        if (!previous.recovery && recovery) runtime[slot].zero_since = 0;
        record(slot, EVENT_COORDINATION);
    } else {
        state.coordination[slot] = previous;
        dirty = true;
    }
    unlock();
    return result;
}

esp_err_t installation_rename(unsigned slot, const char *identity, const char *name)
{
    const size_t length = strlen(name);
    if (slot >= DISPATCH_LIMIT || length >= EQUIPMENT_NAME_BYTES) return ESP_ERR_INVALID_ARG;
    lock();
    const shelly_target *outlet = &state.settings.device[slot].shelly;
    if (!outlet->ip[0] || strcmp(outlet->identity, identity)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char *saved = state.equipment_name[slot];
    esp_err_t result = ESP_OK;
    if (strcmp(saved, name)) {
        char previous[EQUIPMENT_NAME_BYTES];
        memcpy(previous, saved, sizeof(previous));
        memset(saved, 0, EQUIPMENT_NAME_BYTES);
        memcpy(saved, name, length);
        result = save();
        if (result != ESP_OK) {
            memcpy(saved, previous, sizeof(previous));
            dirty = true;
        }
    }
    unlock();
    return result;
}

esp_err_t installation_override(unsigned slot, override_mode mode, unsigned seconds)
{
    if (slot >= DISPATCH_LIMIT || (mode != OVERRIDE_NONE && mode != OVERRIDE_HOLD && mode != OVERRIDE_PREFER)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const time_t utc = network_clock();
    if (!state.settings.device[slot].shelly.ip[0] || (mode != OVERRIDE_NONE && !utc)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    slot_coordination previous = state.coordination[slot];
    state.coordination[slot].mode = mode;
    state.coordination[slot].override_until = mode == OVERRIDE_NONE ? 0 : utc + seconds;
    const esp_err_t result = save();
    if (result == ESP_OK) {
        record(slot, EVENT_COORDINATION);
    } else {
        state.coordination[slot] = previous;
        dirty = true;
    }
    unlock();
    return result;
}

static bool mapping_allows_control(unsigned i)
{
    return state.first_run[i] & RUN_MAPPING;
}

esp_err_t installation_confirm_mapping(unsigned i, const association *expected)
{
    if (i >= DISPATCH_LIMIT || !expected->shelly.ip[0]) return ESP_ERR_INVALID_ARG;
    lock();
    if (!same_device(&state.settings.device[i], expected) || !fresh(&runtime[i], network_uptime()) ||
        (state.enabled && !mapping_allows_control(i))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (state.first_run[i] & RUN_MAPPING) {
        unlock();
        return ESP_OK;
    }
    const uint8_t previous = state.first_run[i];
    state.first_run[i] = RUN_MAPPING;
    const esp_err_t result = save();
    if (result == ESP_OK) {
        runtime[i].next_power_probe = 0;
        record(i, EVENT_MAPPING_CONFIRMED);
    } else {
        state.first_run[i] = previous;
        dirty = true;
    }
    unlock();
    return result;
}

esp_err_t installation_control(bool enable)
{
    lock();
    bool observed = false;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        observed |= mapping_allows_control(i) && fresh(&runtime[i], network_uptime());
    }
    uint32_t day;
    if (enable && (state.enabled || !state.plan_ready || !observed ||
        state.budget_date_pending || !local_time(network_clock(), &day, NULL) || day < state.budget.day)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    bool previous = state.enabled;
    uint8_t previous_hold = state.hold_mask, previous_hold_on = state.hold_on;
    uint16_t previous_attempts = state.attempt_counts;
    state.enabled = enable;
    if (!enable) {
        state.hold_mask = 0;
        state.hold_on = 0;
        state.attempt_counts = 0;
    }
    esp_err_t result = save();
    if (result == ESP_OK) {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            runtime[i].tries = 0;
            runtime[i].pending = DISPATCH_KEEP;
        }
        record(DISPATCH_LIMIT, enable ? EVENT_ENABLED : EVENT_PREVIEW);
    } else {
        state.enabled = previous;
        state.hold_mask = previous_hold;
        state.hold_on = previous_hold_on;
        state.attempt_counts = previous_attempts;
        dirty = true;
    }
    unlock();
    return result;
}

esp_err_t installation_pause(unsigned seconds, bool indefinite)
{
    const bool requested = seconds || indefinite;
    if (requested) {
        atomic_store(&pause_requested, true);
    }
    lock();
    const time_t now = network_clock();
    const uint64_t previous = state.pause_until;
    const uint32_t previous_unclocked = state.pause_unclocked;
    const bool previous_indefinite = state.pause_indefinite;
    const uint8_t previous_hold = state.hold_mask, previous_hold_on = state.hold_on;
    const uint16_t previous_attempts = state.attempt_counts;
    if (requested) {
        state.hold_mask &= ~state.hold_on;
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (state.hold_on & (1u << i)) {
                state.attempt_counts &= ~(3u << (2 * i));
            }
        }
        state.hold_on = 0;
    }
    state.pause_until = now && seconds && !indefinite ? (uint64_t)now + seconds : 0;
    state.pause_unclocked = now || indefinite ? 0 : seconds;
    state.pause_indefinite = indefinite;
    esp_err_t result = save();
    if (result == ESP_OK) {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (runtime[i].pending == DISPATCH_ON) {
                runtime[i].pending = DISPATCH_KEEP;
                runtime[i].tries = 0;
            }
        }
        if (requested && home.command == COMMAND_QUEUED) home.command = COMMAND_CANCELED;
        if (requested && gx.command == COMMAND_QUEUED) gx.command = COMMAND_CANCELED;
        if (requested && restart_job.command == COMMAND_QUEUED) {
            restart_job.command = COMMAND_CANCELED;
            explicit_bzero(restart_job.password, sizeof(restart_job.password));
        }
        record(DISPATCH_LIMIT, requested ? EVENT_PAUSED : EVENT_RESUMED);
    } else {
        state.pause_until = previous;
        state.pause_unclocked = previous_unclocked;
        state.pause_indefinite = previous_indefinite;
        state.hold_mask = previous_hold;
        state.hold_on = previous_hold_on;
        state.attempt_counts = previous_attempts;
        dirty = true;
    }
    atomic_store(&pause_requested, false);
    unlock();
    return result;
}

static bool fresh(const device_runtime *device, uint32_t now)
{
    const device_view *view = &device->view;
    return network_online() && device->connection_epoch == network_epoch() && view->observed && view->outlet_result == SHELLY_OK &&
        now - view->outlet_time <= OBSERVATION_TTL;
}

static bool meter_current(unsigned i, bool observed, time_t utc, uint32_t day)
{
    const shelly_sample *sample = &runtime[i].view.outlet;
    const dispatch_meter_anchor *anchor = &state.budget.anchor[i];
    uint32_t anchor_day;
    return observed && sample->energy_known && anchor->epoch && anchor->epoch <= (uint64_t)utc &&
        anchor->total_mwh == sample->total_mwh && local_time(anchor->epoch, &anchor_day, NULL) && anchor_day == day;
}

esp_err_t installation_accounting(bool clear)
{
    lock();
    const time_t utc = network_clock();
    const uint32_t now = network_uptime();
    uint32_t day = 0;
    local_time(utc, &day, NULL);
    const unsigned gaps = state.budget.gap_mask;
    if (!clear && !gaps) {
        unlock();
        return ESP_OK;
    }
    if (!clear && (!day || day != state.budget.day || state.budget_date_pending)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    dispatch_budget reset = {.day = day};
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (!state.settings.device[i].shelly.ip[0] || (!clear && !(gaps & (1u << i)))) continue;
        const bool ready = day && meter_current(i, fresh(&runtime[i], now), utc, day);
        if (!clear && !ready) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (ready) reset.anchor[i] = state.budget.anchor[i];
    }

    rollback = state;
    if (clear) {
        state.budget = reset;
        state.budget_date_pending = false;
        state.highest_rate_day = 0;
        memset(state.days, 0, sizeof(state.days));
        memset(state.solar_days, 0, sizeof(state.solar_days));
        memset(state.preset_days, 0, sizeof(state.preset_days));
    } else {
        state.budget.gap_mask = 0;
        day_evidence(day)->gap_accepted = true;
    }
    const esp_err_t result = save();
    if (result == ESP_OK) {
        if (clear) {
            reset_solar_credit();
            for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
                runtime[i].observation_day = 0;
                runtime[i].next_probe = 0;
            }
            ++generation;
        }
        record(DISPATCH_LIMIT, clear ? EVENT_ACCOUNTING_CLEARED : EVENT_METER_RECOVERED);
    } else {
        state = rollback;
        dirty = true;
    }
    explicit_bzero(&rollback, sizeof(rollback));
    unlock();
    return result;
}

static bool paused_at(time_t now)
{
    return state.pause_indefinite || state.pause_unclocked ||
        (state.pause_until && (!now || state.pause_until > (uint64_t)now));
}

static bool path_fresh(uint32_t now, uint32_t stamp, bool sampled)
{
    return sampled && network_online() && path.epoch == network_epoch() && now - stamp <= PATH_FRESH_SECONDS;
}

static bool grid_fresh(uint32_t now)
{
    return grid.sampled && grid.result == SHELLY_OK && now - grid.at <= GRID_FRESH &&
        network_online() && grid.epoch == network_epoch();
}

static bool operating_grid(void)
{
    return state.grid.target.ip[0] && state.grid.direction_verified;
}

static bool following_generation(void)
{
    return state.solar.ip[0] && !operating_grid();
}

static dispatch_daylight daylight_at(time_t utc)
{
    if (!state.daylight.enabled || !utc)
        return (dispatch_daylight){0};
    const int64_t day = dispatch_solar_day(utc, state.daylight.longitude_e4);
    if (daylight_cache.day != day) {
        daylight_cache.today = dispatch_sun_times(day, state.daylight.latitude_e4, state.daylight.longitude_e4);
        daylight_cache.tomorrow = dispatch_sun_times(day + 1, state.daylight.latitude_e4, state.daylight.longitude_e4);
        daylight_cache.day = day;
    }
    return daylight_cache.today;
}

static bool solar_night(time_t utc)
{
    const dispatch_daylight sun = daylight_at(utc);
    if (sun.kind != DAYLIGHT_UNKNOWN) return !dispatch_in_daylight(sun, utc);
    uint32_t minute;
    if (!local_minute(utc, &minute)) return false;
    const unsigned start = state.solar.night_start, end = state.solar.night_end;
    return start <= end ? minute >= start && minute < end : minute >= start || minute < end;
}

static bool solar_control_fresh(uint32_t now)
{
    return solar.sampled && now - solar.at <= SOLAR_FRESH && solar.result != ECU_IDENTITY &&
        (!solar.reading.watts || (solar.reading.online && now - solar.progress_at <= SOLAR_FRESH));
}

static path_status node_status(uint32_t now)
{
    if (!path_fresh(now, path.node_at, path.node_sampled)) return PATH_WAITING;
    if (path.node.status != PATH_READY) return path.node.status;
    return path.identity;
}

static path_status datum_status(uint32_t now)
{
    return path_fresh(now, path.datum_at, path.datum_sampled) ? path.datum.status : PATH_WAITING;
}

static miner_destination destination_at(unsigned i, uint32_t now)
{
    return network_online() && runtime[i].connection_epoch == network_epoch() &&
        now - runtime[i].destination_at <= PATH_FRESH_SECONDS ? runtime[i].destination : DESTINATION_UNKNOWN;
}

static bool miner_idle(const device_runtime *device, uint32_t now)
{
    return device->view.power_mode.status == GOLDSHELL_MODE_READY && device->view.power_mode.mode == GOLDSHELL_MODE_IDLE &&
        device->power_checked && now - device->power_checked <= MODE_FRESH_SECONDS &&
        device->power_probe_epoch == network_epoch();
}

static bool miner_work_paused(unsigned slot, uint32_t now)
{
    return miner_idle(&runtime[slot], now) ||
        (restart_job.slot == slot && restart_job.revision == generation && integration_busy(restart_job.command)) ||
        (power_job.pending && power_job.slot == slot &&
        power_job.revision == generation && power_job.desired == GOLDSHELL_MODE_IDLE);
}

static bool stalled(unsigned i, uint32_t now)
{
    const device_runtime *d = &runtime[i];
    return d->zero_since && now - d->zero_since >= STALL_SECONDS &&
        now - d->view.miner_time <= OBSERVATION_TTL && fresh(d, now) && d->view.outlet.on && !miner_work_paused(i, now);
}

static const char *recovery_status(unsigned i, uint32_t now, uint32_t day, time_t utc)
{
    const slot_coordination *slot = &state.coordination[i];
    if (!slot->recovery) return "Recovery is off";
    if (slot->recovery_until > (uint64_t)utc) return "Recovery restart hold";
    if (day && slot->recovery_day >= day) return "Daily recovery attempt used";
    if (!fresh(&runtime[i], now) || !runtime[i].view.outlet.on) return "Waiting for powered outlet evidence";
    if (restart_job.slot == i && restart_job.revision == generation && integration_busy(restart_job.command))
        return "Waiting for the miner restart";
    if (miner_work_paused(i, now)) return "Waiting for the miner to resume from Idle";
    if (node_status(now) != PATH_READY) return "Recovery blocked while XBT node evidence is unavailable";
    const path_status datum = datum_status(now);
    if (datum != PATH_READY && datum != PATH_SOLO) return "Recovery blocked while DATUM evidence is unavailable";
    if (destination_at(i, now) != DESTINATION_MATCH) return "Recovery blocked until the miner reports the DATUM destination";
    if (!stalled(i, now)) return "Waiting for ten minutes of reported zero hashrate";
    return "Recovery eligible when the operating policy permits";
}

static bool recovery_eligible(unsigned i, const dispatch_inputs *in, time_t utc, uint32_t day)
{
    const slot_coordination *slot = &state.coordination[i];
    if (!slot->recovery || !day || slot->recovery_day >= day || slot->recovery_until > (uint64_t)utc ||
        !stalled(i, in->now) || in->now - in->outlet[i].since < DISPATCH_MIN_ON ||
        node_status(in->now) != PATH_READY || destination_at(i, in->now) != DESTINATION_MATCH) return false;
    const path_status datum = datum_status(in->now);
    return datum == PATH_READY || datum == PATH_SOLO;
}

static uint16_t startup_watts(unsigned i, uint16_t configured)
{
    const dispatch_profile *profile = &state.profile[i];
    if (configured) return configured;
    float peak = state.power_test[i].peak;
    if (profile->power_samples) peak = fmaxf(peak, profile->max_watts);
    // An unpowered miner can boot into a different saved preset than the latest active one.
    for (unsigned mode = 0; mode < PRESET_COUNT; ++mode) {
        peak = fmaxf(peak, state.presets[i].mode[mode].totals.peak_watts);
    }
    return peak > 0 && peak <= 10000 ? (uint16_t)peak + 1 : 0;
}

static bool solar_shed_reading_current(void)
{
    return solar.result == ECU_OK && solar.epoch == network_epoch() && network_online();
}

static bool solar_reply_fresh(uint32_t now)
{
    return solar_shed_reading_current() && solar.sampled && now - solar.at <= SOLAR_FRESH;
}

static bool solar_first_fresh(uint32_t now)
{
    // A current reading within the reserve cannot admit a start or earn solar credit.
    return solar_reply_fresh(now) && (solar.reading.watts <= state.follow.reserve || solar_control_fresh(now));
}

static void accrue_solar_credit(uint32_t now)
{
    const bool measured = operating_grid();
    if (!measured && !following_generation()) return;
    const uint32_t epoch = network_epoch();
    const uint32_t elapsed = now - solar_credit.at;
    solar_credit.at = now;
    if (solar_credit.epoch != epoch || elapsed > OBSERVATION_TTL) {
        solar_credit.epoch = epoch;
        memset(solar_credit.watt_seconds, 0, sizeof(solar_credit.watt_seconds));
        return;
    }
    if (!elapsed || (measured ? !grid_fresh(now) :
        !solar_first_fresh(now) || solar.reading.watts <= state.follow.reserve)) return;

    uint32_t draw[DISPATCH_LIMIT] = {0};
    uint32_t total = 0;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (!state.settings.device[i].shelly.ip[0]) continue;
        const device_runtime *device = &runtime[i];
        if (!fresh(device, now)) return;
        if (!device->view.outlet.on) continue;
        if (!device->view.outlet.power_known) return;
        draw[i] = (uint32_t)device->view.outlet.watts;
        total += draw[i];
    }
    if (!total) return;
    const uint32_t available = measured ? dispatch_solar_coverage(total, grid.watts > 0 ? grid.watts : 0, total) :
        solar.reading.watts - state.follow.reserve;
    const uint32_t limit = DISPATCH_PRORATE_SECONDS * DISPATCH_METER_CHANNEL_WATTS;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (!draw[i]) continue;
        uint32_t bank = solar_credit.watt_seconds[i];
        if (!bank || now - solar_credit.first_credit[i] > DISPATCH_PRORATE_SECONDS) {
            bank = 0;
            solar_credit.first_credit[i] = now - elapsed;
        }
        // Share generation after the reserve, including headroom for meter timing
        // and load variation. Accounting caps the credit at the actual energy delta.
        const uint64_t watt_seconds = bank + (uint64_t)available * draw[i] * elapsed / total;
        solar_credit.watt_seconds[i] = watt_seconds < limit ? watt_seconds : limit;
    }
}

static void solar_shed_status(dispatch_inputs *in, bool powered)
{
    dispatch_generation *power = &in->solar;
    const bool paid = dispatch_paid_covering(dispatch_paid_permission(in));
    power->low = !in->paused && !paid && powered && dispatch_solar_deficit(power);
    if (in->grid_source) {
        if (power->low) {
            power->shed_needs_reading = !grid.consecutive || grid.previous_watts <= 0;
            const uint32_t elapsed = grid.importing ? in->now - grid.import_since : 0;
            power->shed_wait_seconds = grid.importing && elapsed < GRID_IMPORT_SECONDS ? GRID_IMPORT_SECONDS - elapsed : 0;
        }
        return;
    }
    const uint16_t delay = state.follow.shed_delay_seconds;
    if (!power->low || !delay) return;
    const bool tracking = solar.low_tracking && solar.low_epoch == network_epoch() && solar_shed_reading_current();
    const uint32_t elapsed = tracking ? in->now - solar.low_since : 0;
    power->shed_wait_seconds = elapsed < delay ? delay - elapsed : 0;
    // A held low value can run the timer, but shedding requires another
    // usable report after low production was first observed.
    power->shed_needs_reading = !tracking || solar.at <= solar.low_since;
}

static void generation_load(dispatch_inputs *in, unsigned i, const dispatch_outlet *outlet, uint32_t now)
{
    if (!outlet->present) return;
    dispatch_generation *solar_power = &in->solar;
    const device_runtime *device = &runtime[i];
    const uint16_t expected = startup_watts(i, state.follow.start_watts[i]);
    solar_power->start_watts[i] = expected;
    if (!outlet->fresh) {
        solar_power->load_known = false;
        return;
    }
    if (outlet->on) {
        if (!device->view.outlet.power_known) {
            solar_power->load_known = false;
            return;
        }
        const double watts = device->view.outlet.watts;
        const uint32_t rounded = (uint32_t)watts;
        const uint32_t draw = rounded + (rounded < watts);
        in->draw_watts[i] = draw;
        solar_power->running_watts += draw;
        if (now - outlet->since < DISPATCH_MIN_ON && expected > draw) {
            solar_power->committed_watts += expected - draw;
        }
        if (now < device->preset_reserved_until && device->preset_reserved_watts > draw) {
            solar_power->committed_watts += device->preset_reserved_watts - draw;
        }
    } else if (device->pending == DISPATCH_ON && device->tries &&
               now < device->next_command + SOLAR_START_RESERVATION) {
        if (!expected) solar_power->load_known = false;
        solar_power->reserved_start_mask |= 1u << i;
        solar_power->committed_watts += expected;
    }
}

static dispatch_inputs inputs(uint32_t now, time_t utc_now)
{
    const bool grid_source = operating_grid();
    const bool following = following_generation();
    dispatch_inputs in = {
        .policy = state.settings.policy,
        .quiet = state.quiet,
        .now = now, .next_start = next_start,
        .used_nano_cents = state.budget.used_nano_cents,
        .operating = state.operating, .grid_source = grid_source,
        .source_grace = last_usable_power && now - last_usable_power <= SOURCE_GRACE_SECONDS,
        .grid_recent_export = grid.covered_at && grid.epoch == network_epoch() &&
            now - grid.covered_at <= BRIDGE_SOLAR_EVIDENCE_SECONDS,
        .paused = paused_at(utc_now),
        .accounting_valid = state.budget.gap_mask == 0,
        .surplus = {
            .grid_watts = grid.watts,
            .start_reserve = state.grid.reserve, .next_change = next_energy_change,
        },
        .solar = {
            .configured = following || grid_source,
            .reserve = grid_source ? 0 : state.follow.reserve, .shed_margin = state.follow.shed_margin,
            .restart_seconds = state.follow.restart_seconds,
            .production = solar.reading.watts, .supported_production = solar.supported_watts,
            .fresh = grid_source ? grid_fresh(now) : following && solar_first_fresh(now),
            .sustained = following && solar.sustained && solar.result == ECU_OK &&
                solar.epoch == network_epoch() && !solar_night(utc_now),
            .load_known = true,
        },
    };
    uint32_t day;
    const bool clock = local_time(utc_now, &day, &in.minute);
    if (clock) dispatch_apply_weekend(&in, &state.weekend, day);
    in.clock_valid = clock && !state.budget_date_pending && day >= state.budget.day;
    in.accounting_valid = in.clock_valid && day == state.budget.day && in.accounting_valid;
    const dispatch_tariff *tariff = clock && state.weekend.enabled && dispatch_is_weekend(day) ?
        &state.weekend.tariff : &state.tariff;
    in.guard_rate = dispatch_highest_rate(in.policy.rate_millicents_per_kwh, tariff);
    if (state.weekend.enabled) {
        const dispatch_tariff *other = tariff == &state.tariff ? &state.weekend.tariff : &state.tariff;
        in.guard_rate = dispatch_highest_rate(in.guard_rate, other);
    }
    if ((following || grid_source) && state.daylight.enabled) {
        const dispatch_daylight sun = daylight_at(utc_now);
        const uint32_t duration = state.daylight.minutes * 60U;
        const bool same_day = state.bridge_started &&
            dispatch_solar_day(state.bridge_started, state.daylight.longitude_e4) == daylight_cache.day;
        const uint64_t elapsed = same_day && (uint64_t)utc_now >= state.bridge_started ?
            (uint64_t)utc_now - state.bridge_started : same_day ? duration : 0;
        in.solar.bridge = (dispatch_daylight_bridge){.enabled = true,
            .daylight = in.clock_valid && dispatch_in_daylight(sun, utc_now),
            .seconds = elapsed < duration ? duration - elapsed : 0,
            .forecast_rate = dispatch_rate_at(in.policy.rate_millicents_per_kwh, tariff, in.minute, NULL),
            .recent_solar = solar.lit_at && now - solar.lit_at <= BRIDGE_SOLAR_EVIDENCE_SECONDS &&
                solar.lit_epoch == network_epoch() && solar.result != ECU_IDENTITY};
        if (sun.kind == DAYLIGHT_RISE_SET && in.solar.bridge.daylight) {
            in.solar.bridge.seconds_to_sunset = sun.sunset - utc_now;
            if (in.solar.bridge.seconds_to_sunset < in.solar.bridge.seconds)
                in.solar.bridge.seconds = in.solar.bridge.seconds_to_sunset;
        }
    }
    bool powered = false;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const device_runtime *device = &runtime[i];
        const bool present = state.settings.device[i].shelly.ip[0];
        in.outlet[i] = (dispatch_outlet){
            present, fresh(device, now), device->view.outlet.on, device->since,
        };
        if (in.outlet[i].fresh && in.outlet[i].on &&
            device->view.miner.activity_known && device->view.miner.rate_window_seconds &&
            device->view.miner.mhs > 0 && device->connection_epoch == network_epoch() &&
            now - device->view.miner_time <= OBSERVATION_TTL) in.productive_mask |= 1u << i;
        in.solar_off_since[i] = device->solar_off_since;
        const slot_coordination *slot = &state.coordination[i];
        generation_load(&in, i, &in.outlet[i], now);
        powered |= present && in.outlet[i].fresh && in.outlet[i].on;
        const bool override = utc_now && slot->override_until > (uint64_t)utc_now;
        in.priority[i] = override && slot->mode == OVERRIDE_PREFER ? UINT8_MAX : slot->priority;
        if (override && slot->mode == OVERRIDE_HOLD) in.manual_hold_mask |= 1u << i;
        if (utc_now && slot->recovery_until > (uint64_t)utc_now) in.recovery_hold_mask |= 1u << i;
        if (present && !meter_current(i, in.outlet[i].fresh, utc_now, state.budget.day)) {
            in.accounting_valid = false;
        }
    }
    if (grid_source) {
        const int32_t conservative = grid.previous_watts > grid.watts ? grid.previous_watts : grid.watts;
        const int64_t current = (int64_t)in.solar.running_watts - grid.watts;
        const int64_t qualified = (int64_t)in.solar.running_watts - conservative;
        in.solar.production = current > 0 ? current : 0;
        in.solar.supported_production = qualified > 0 ? qualified : 0;
        in.solar.sustained = grid.consecutive && in.solar.fresh;
    }
    if (following || grid_source) solar_shed_status(&in, powered);
    return in;
}

static void update_solar_shed_timer(uint32_t now)
{
    const dispatch_inputs in = inputs(now, network_clock());
    const dispatch_reason permission = dispatch_paid_permission(&in);
    paid_authority = !state.enabled || storage_error ? 0 :
        permission == DISPATCH_DAYLIGHT_BRIDGE ? TIMELINE_PAID_GAP :
        permission == DISPATCH_WINDOW_OPEN ? TIMELINE_PAID_WINDOW : 0;
    if (in.solar.configured && in.solar.fresh && in.solar.load_known) last_usable_power = now;
    if (!following_generation() ||
        !state.follow.shed_delay_seconds || !solar_shed_reading_current()) {
        solar.low_tracking = false;
        return;
    }
    if (!in.solar.low) {
        solar.low_tracking = false;
    } else if (!solar.low_tracking || solar.low_epoch != solar.epoch) {
        solar.low_since = now;
        solar.low_epoch = solar.epoch;
        solar.low_tracking = true;
    }
}

static void update_daylight_bridge(uint32_t now)
{
    const time_t utc = network_clock();
    bool active = false, transition_recorded = false;
    unsigned reason = EVENT_NO_REASON;
    if (state.daylight.enabled && state.enabled && !storage_error && utc) {
        const dispatch_inputs in = inputs(now, utc);
        reason = dispatch_paid_permission(&in);
        const bool same_day = state.bridge_started &&
            dispatch_solar_day(state.bridge_started, state.daylight.longitude_e4) == daylight_cache.day;
        const bool recovered = in.solar.fresh && in.solar.sustained && in.solar.load_known && in.solar.running_watts &&
            dispatch_solar_headroom(&in.solar) >= 0;
        active = in.solar.bridge.daylight && !recovered && reason == DISPATCH_DAYLIGHT_BRIDGE;
        const bool opening = !same_day && in.solar.bridge.daylight && reason == DISPATCH_DAYLIGHT_BRIDGE;
        if (in.solar.bridge.daylight && (opening || (same_day && recovered))) {
            const uint64_t previous = state.bridge_started;
            state.bridge_started = opening ? (uint64_t)utc : 0;
            if (save() == ESP_OK) {
                record(DISPATCH_LIMIT, opening ? EVENT_DAYLIGHT_BRIDGE_STARTED : EVENT_DAYLIGHT_BRIDGE_REARMED);
                bridge_preset_attempts = 0;
                transition_recorded = true;
            } else {
                state.bridge_started = previous;
                active = false;
                dirty = true;
            }
        }
    }
    if (utc && bridge_active != active && !transition_recorded) record_action(DISPATCH_LIMIT,
        active ? EVENT_DAYLIGHT_BRIDGE_STARTED : EVENT_DAYLIGHT_BRIDGE_ENDED, DISPATCH_KEEP, reason);
    bridge_active = active;
}

static void decide(const dispatch_inputs *in, dispatch_decision *decisions, time_t utc)
{
    dispatch_inputs eligible = *in;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (!mapping_allows_control(i) || (state.hold_mask & (1u << i)) || testing_outlet(i)) {
            eligible.outlet[i].present = false;
        }
    }
    dispatch_plan(&eligible, decisions);
    uint32_t day = 0;
    if (in->clock_valid) local_time(utc, &day, NULL);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (testing_outlet(i)) {
            decisions[i] = (dispatch_decision){DISPATCH_KEEP, DISPATCH_POWER_TEST};
        } else if (in->outlet[i].present && !mapping_allows_control(i)) {
            decisions[i] = (dispatch_decision){DISPATCH_KEEP, DISPATCH_CONFIRM_MAPPING};
        } else if (state.hold_mask & (1u << i)) {
            decisions[i] = (dispatch_decision){DISPATCH_KEEP, DISPATCH_COMMAND_HOLD};
        } else if (decisions[i].action == DISPATCH_KEEP &&
                   (decisions[i].reason == DISPATCH_WINDOW_OPEN || decisions[i].reason == DISPATCH_PAID_COVERAGE ||
                    decisions[i].reason == DISPATCH_SOLAR_COVERED || decisions[i].reason == DISPATCH_DAYLIGHT_BRIDGE) &&
                   recovery_eligible(i, in, utc, day)) {
            decisions[i] = (dispatch_decision){DISPATCH_OFF, DISPATCH_RECOVERY};
        }
    }
}

void installation_preview_plan(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                               const dispatch_tariff *tariff, const operating_settings *operating,
                               operating_scenario cases[OPERATING_SCENARIOS])
{
    lock();
    const uint32_t now = network_uptime();
    dispatch_inputs base = inputs(now, network_clock());
    base.policy = *policy;
    base.quiet = *quiet;
    base.operating = operating->plan;
    base.grid_source = operating_grid();
    base.guard_rate = dispatch_highest_rate(policy->rate_millicents_per_kwh, tariff);
    base.solar.configured = base.grid_source || state.solar.ip[0];
    base.solar.reserve = base.grid_source ? 0 : state.follow.reserve;
    base.solar.committed_watts = base.solar.reserved_start_mask = 0;
    base.solar.shed_wait_seconds = 0;
    base.solar.shed_needs_reading = false;
    base.solar.bridge = (dispatch_daylight_bridge){.enabled = operating->daylight.enabled,
        .daylight = true, .recent_solar = true, .seconds = operating->daylight.minutes * 60U};
    base.clock_valid = base.accounting_valid = base.solar.load_known = true;
    base.paused = false;
    base.used_nano_cents = 0;
    base.next_start = base.surplus.next_change = 0;
    base.manual_hold_mask = base.recovery_hold_mask = 0;
    base.now += DISPATCH_MIN_ON;
    const bool source = base.solar.configured;
    dispatch_solar_preset_slot presets[DISPATCH_LIMIT] = {0};
    uint32_t running = 0, largest_start = 0;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        base.outlet[i].fresh = true;
        base.outlet[i].since = 0;
        base.solar_off_since[i] = 0;
        if (!base.outlet[i].present || !mapping_allows_control(i)) {
            base.outlet[i].present = false;
            continue;
        }
        const uint16_t watts = startup_watts(i, state.follow.start_watts[i]);
        base.solar.start_watts[i] = watts;
        if (watts > largest_start) largest_start = watts;
        const uint32_t normal = measured_preset_watts(i, GOLDSHELL_MODE_DEFAULT);
        const uint32_t reduced = measured_preset_watts(i, state.presets[i].identity.alternate);
        if (!base.draw_watts[i]) base.draw_watts[i] = normal ? normal : watts;
        running += base.draw_watts[i];
        presets[i] = (dispatch_solar_preset_slot){
            .ready = preset_choice(i) == PRESET_SOLAR && preset_authorized(i) && normal && reduced,
            .draw = normal, .default_watts = normal, .reduced_watts = reduced,
            .bridge_efficient = bridge_preset_efficiency(i),
        };
    }
    uint32_t mine = 1440, gap = 1440, paid = 1440;
    for (base.minute = 0; base.minute < 1440; ++base.minute) {
        if (dispatch_operation_limit(&base) != DISPATCH_WINDOW_OPEN) continue;
        if (mine == 1440) mine = base.minute;
        if (dispatch_paid_permission(&base) == DISPATCH_WINDOW_OPEN) {
            if (paid == 1440) paid = base.minute;
        } else if (gap == 1440) gap = base.minute;
        if (paid < 1440 && gap < 1440) break;
    }
    for (unsigned test = 0; test < OPERATING_SCENARIOS; ++test) {
        dispatch_inputs in = base;
        in.minute = mine;
        in.solar.sustained = in.solar.fresh = source;
        in.solar.running_watts = running;
        in.solar.supported_production = in.solar.production = in.solar.reserve + running / 2;
        cases[test].applicable = mine < 1440;
        cases[test].preset = (dispatch_solar_preset_decision){.slot = DISPATCH_LIMIT};
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i)
            in.outlet[i].on = in.outlet[i].present && in.draw_watts[i] && test != 0 && test != 6;
        switch (test) {
            case 0: // Room for one start.
                cases[test].applicable &= source;
                in.solar.running_watts = 0;
                in.surplus.grid_watts = -(int32_t)(largest_start + in.surplus.start_reserve);
                in.solar.production = in.solar.supported_production = in.solar.reserve - in.surplus.grid_watts;
                break;
            case 1: // Daylight shortfall outside the scheduled paid window, if possible.
                cases[test].applicable &= source;
                if (gap < 1440) in.minute = gap;
                break;
            case 2:
                cases[test].applicable &= source;
                if (gap < 1440) in.minute = gap;
                in.solar.bridge.daylight = false;
                in.solar.supported_production = in.solar.production = 0;
                break;
            case 3:
                cases[test].applicable = base.quiet.enabled;
                in.minute = base.quiet.start_minute;
                break;
            case 4:
                cases[test].applicable &= policy->daily_allowance_millicents > 0;
                in.used_nano_cents = (uint64_t)policy->daily_allowance_millicents * 1000000;
                break;
            case 5:
                cases[test].applicable &= source;
                if (gap < 1440) in.minute = gap;
                in.solar.fresh = in.source_grace = false;
                break;
            case 6:
                cases[test].applicable = paid < 1440;
                in.minute = paid;
                in.solar.running_watts = 0;
                in.solar.supported_production = in.solar.production = 0;
                break;
        }
        if (!cases[test].applicable) continue;
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i)
            if (in.outlet[i].on) cases[test].on_mask |= 1u << i;
        dispatch_plan(&in, cases[test].decision);
        if (test == 1) cases[test].preset = dispatch_solar_choose_preset(&in, cases[test].decision, presets, true);
    }
    unlock();
}

static bool test_budget_available(unsigned slot, const dispatch_inputs *in, unsigned seconds)
{
    const uint64_t limit = (uint64_t)in->policy.daily_allowance_millicents * 1000000;
    if (!limit) return true;
    if (!in->accounting_valid || in->used_nano_cents >= limit) return false;
    const uint32_t rate = dispatch_highest_rate(in->policy.rate_millicents_per_kwh, &state.tariff);
    const uint32_t highest = state.weekend.enabled ? dispatch_highest_rate(rate, &state.weekend.tariff) : rate;
    unsigned running = 0;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (i != slot && in->outlet[i].fresh && in->outlet[i].on) ++running;
    }
    const uint64_t mwh = ((uint64_t)DISPATCH_METER_CHANNEL_WATTS * (seconds + 20 * running) * 1000 + 3599) / 3600;
    return mwh * highest <= limit - in->used_nano_cents;
}

static const char *test_start_block(unsigned slot, const dispatch_inputs *in, bool queued)
{
    if (!in->outlet[slot].present) return "Pair a metered outlet first.";
    if (!mapping_allows_control(slot)) return "Confirm what this outlet powers first.";
    if (!state.enabled) return "Enable outlet control before starting the test.";
    if (storage_error) return "Wait for controller storage to recover.";
    if (!in->clock_valid) return "Wait for the controller clock to synchronize.";
    if (dispatch_operation_limit(in) == DISPATCH_WINDOW_CLOSED)
        return "Wait for allowed mining hours.";
    if (in->paused || atomic_load(&pause_requested)) return "Resume operation before running a test.";
    if (dispatch_quiet_active(in)) return "Wait for Quiet Hours to end.";
    if (testing_outlet(power_test.slot) && !queued) return "Wait for the current power test to finish.";
    const device_runtime *device = &runtime[slot];
    if (!in->outlet[slot].fresh) return "Wait for a fresh outlet reading.";
    if (in->outlet[slot].on) return "Start the test with this outlet off.";
    if (!outlet_timed_test(state.settings.device[slot].shelly.identity))
        return "This outlet profile does not provide a timed power test. Enter a startup estimate in Energy.";
    if (!device->view.outlet.power_known || !device->view.outlet.energy_known) return "A working outlet power and energy meter is required.";
    if (device->view.outlet.timer_active) return "Wait for the outlet's existing timer to finish.";
    if (((in->manual_hold_mask | in->recovery_hold_mask | state.hold_mask) & (1u << slot)) ||
        device->pending != DISPATCH_KEEP || (power_job.pending && power_job.slot == slot) ||
        (state.evaluation.active && state.evaluation.slot == slot)) return "Resolve the hold or pending equipment action first.";
    if (in->now - device->since < DISPATCH_MIN_OFF || dispatch_solar_restart_remaining(in, slot))
        return "Wait for the outlet restart lockout to end.";
    if (!test_budget_available(slot, in, POWER_TEST_SECONDS)) return "The test needs fresh electricity accounting and enough remaining daily allowance to cover a four-minute run.";
    return NULL;
}

static void stop_power_test(power_test_phase outcome, const char *detail)
{
    power_test.phase = POWER_TEST_STOPPING;
    power_test.outcome = outcome;
    power_test.detail = detail;
}

// Only the worker performs relay RPCs. Release the settings lock during
// network I/O so status and owner requests can run while a relay is slow.
static uint32_t relay_revision, relay_epoch;

static bool relay_current(void)
{
    lock();
    const bool current = relay_revision == generation && relay_epoch == network_epoch() && network_online();
    unlock();
    return current;
}

static bool relay_may_start(void)
{
    lock();
    const bool allowed = relay_revision == generation && relay_epoch == network_epoch() &&
        admit_on() && state.enabled && !storage_error && !paused_at(network_clock());
    unlock();
    return allowed;
}

static shelly_result send_relay(unsigned slot, bool on, uint16_t timed, bool *current)
{
    shelly_target target = state.settings.device[slot].shelly;
    ha_endpoint ha = {0};
    mqtt_endpoint *mqtt = NULL;
    const outlet_kind kind = outlet_identity(target.identity);
    if (kind == OUTLET_HA) ha = state.home.endpoint;
    if (kind == OUTLET_MQTT) {
        mqtt = malloc(sizeof(*mqtt));
        if (!mqtt) {
            explicit_bzero(&target, sizeof(target));
            *current = true;
            return SHELLY_NETWORK;
        }
        *mqtt = state.mqtt;
    }
    relay_revision = generation;
    relay_epoch = network_epoch();
    unlock();
    const shelly_result result = timed ? outlet_timed_on(&target, timed, relay_may_start) :
        outlet_set(&target, &ha, mqtt, on, on ? relay_may_start : relay_current);
    explicit_bzero(&target, sizeof(target));
    explicit_bzero(&ha, sizeof(ha));
    if (mqtt) {
        explicit_bzero(mqtt, sizeof(*mqtt));
        free(mqtt);
    }
    lock();
    *current = relay_revision == generation && relay_epoch == network_epoch();
    return result;
}

esp_err_t installation_power_test(unsigned slot, uint16_t identity, bool start, const char **conflict)
{
    if (slot >= DISPATCH_LIMIT) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!identity || identity != installation_equipment_identity(&state.settings.device[slot])) {
        *conflict = "The outlet pairing changed. Reopen the test for the current outlet.";
    } else if (!start) {
        if (testing_outlet(slot)) {
            if (power_test.phase != POWER_TEST_STOPPING)
                stop_power_test(POWER_TEST_CANCELED, "You stopped the power test.");
            result = ESP_OK;
        } else {
            *conflict = "This outlet has no active power test.";
        }
    } else {
        const dispatch_inputs in = inputs(network_uptime(), network_clock());
        const char *blocked = test_start_block(slot, &in, false);
        if (blocked) *conflict = blocked;
        else {
            memset(&power_test, 0, sizeof(power_test));
            power_test.slot = slot;
            power_test.revision = generation;
            power_test.phase = POWER_TEST_QUEUED;
            result = ESP_OK;
        }
    }
    unlock();
    return result;
}

static void finish_power_test(uint32_t now)
{
    const unsigned slot = power_test.slot;
    const dispatch_power_test_result previous = state.power_test[slot];
    const uint8_t armed = state.test_armed;
    state.test_armed = 0;
    if (power_test.outcome == POWER_TEST_COMPLETE) state.power_test[slot] = power_test.result;
    if (armed && save() != ESP_OK) {
        state.test_armed = armed;
        state.power_test[slot] = previous;
        power_test.next_save = now + RETRY_SECONDS;
        dirty = true;
        return;
    }
    power_test.phase = power_test.outcome;
    record(slot, power_test.outcome == POWER_TEST_COMPLETE ? EVENT_POWER_TEST_COMPLETE :
        power_test.outcome == POWER_TEST_CANCELED ? EVENT_POWER_TEST_CANCELED : EVENT_POWER_TEST_FAILED);
}

static void power_test_tick(uint32_t now)
{
    const unsigned slot = power_test.slot;
    if (!testing_outlet(slot)) return;
    device_runtime *device = &runtime[slot];
    const time_t utc = network_clock();
    const dispatch_inputs in = inputs(now, utc);
    if (power_test.phase == POWER_TEST_QUEUED) {
        if (power_test.revision != generation) {
            stop_power_test(POWER_TEST_FAILED, "The operating configuration changed before the test started.");
            return;
        }
        const char *block = test_start_block(slot, &in, true);
        if (block) {
            stop_power_test(POWER_TEST_FAILED, block);
            return;
        }
        if (now < next_start || now < next_energy_change) return;
        state.test_armed = slot + 1;
        if (save() != ESP_OK) {
            state.test_armed = 0;
            dirty = true;
            stop_power_test(POWER_TEST_FAILED, "The controller could not save the test before switching.");
            return;
        }
        power_test.last_read = device->view.outlet_time;
        power_test.started = now;
        power_test.lease = now + POWER_TEST_SECONDS;
        power_test.phase = POWER_TEST_STARTING;
        bool current;
        const shelly_result result = send_relay(slot, true, POWER_TEST_SECONDS, &current);
        if (!current) return;
        if (result == SHELLY_DEFERRED) {
            // The read-only preflight or pause check stopped the ON request.
            state.test_armed = 0;
            if (save() != ESP_OK) dirty = true;
            stop_power_test(POWER_TEST_FAILED, "The outlet state changed before the timed start.");
            return;
        }
        next_start = network_uptime() + DISPATCH_STAGGER;
        next_energy_change = network_uptime() + GRID_SETTLE;
        device->next_probe = 0;
        record_action(slot, result == SHELLY_OK ? EVENT_COMMAND_ON : EVENT_COMMAND_ERROR, DISPATCH_ON, DISPATCH_POWER_TEST);
        record(slot, EVENT_POWER_TEST_STARTED);
        return;
    }
    if (power_test.phase == POWER_TEST_STOPPING) {
        const bool confirmed_off = in.outlet[slot].fresh && !in.outlet[slot].on &&
            device->view.outlet_time != power_test.last_read;
        if (!state.test_armed || confirmed_off) {
            if (now >= power_test.next_save) finish_power_test(now);
        } else if (network_online() && now >= power_test.retry_off) {
            if (in.outlet[slot].fresh) power_test.last_read = device->view.outlet_time;
            bool current;
            const shelly_result result = send_relay(slot, false, 0, &current);
            if (!current) return;
            power_test.retry_off = network_uptime() + RETRY_SECONDS;
            next_energy_change = network_uptime() + GRID_SETTLE;
            device->next_probe = 0;
            record_action(slot, result == SHELLY_OK ? EVENT_COMMAND_OFF : EVENT_COMMAND_ERROR, DISPATCH_OFF, DISPATCH_POWER_TEST);
        }
        return;
    }
    if (!state.enabled || storage_error || !in.clock_valid || in.paused || atomic_load(&pause_requested) ||
        dispatch_quiet_active(&in) || power_test.revision != generation ||
        ((in.manual_hold_mask | in.recovery_hold_mask) & (1u << slot))) {
        stop_power_test(POWER_TEST_CANCELED, "The operating policy changed or suspended the test.");
        return;
    }
    if (now >= power_test.lease) {
        stop_power_test(POWER_TEST_FAILED, power_test.samples.peak < 10 ? "No equipment draw of at least 10 W was measured." :
            state.settings.device[slot].miner_ip[0] ? "Stable power and miner-reported hashing were not confirmed during the test." :
            "The outlet did not show stable running power before the test ended.");
        return;
    }
    if (!test_budget_available(slot, &in, power_test.lease - now)) {
        stop_power_test(POWER_TEST_FAILED, "Electricity accounting or available budget changed during the test.");
        return;
    }
    if (in.solar.configured) {
        dispatch_decision decisions[DISPATCH_LIMIT];
        decide(&in, decisions, utc);
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (decisions[i].reason == DISPATCH_SOLAR_STOP || decisions[i].reason == DISPATCH_SURPLUS_STOP ||
                (i != slot && in.outlet[i].on && decisions[i].reason == DISPATCH_SOLAR_DEFICIT_PENDING)) {
                stop_power_test(POWER_TEST_FAILED, "Available power fell. The test stopped to give running miners priority.");
                return;
            }
        }
    }
    const bool starting = power_test.phase == POWER_TEST_STARTING;
    if (!in.outlet[slot].fresh || device->view.outlet_time == power_test.last_read) {
        if ((starting || !in.outlet[slot].fresh) && now - power_test.started >= TEST_CONFIRM_SECONDS)
            stop_power_test(POWER_TEST_FAILED, "The controller lost fresh outlet confirmation.");
        return;
    }
    if (!in.outlet[slot].on) {
        if (!starting || now - power_test.started >= TEST_CONFIRM_SECONDS)
            stop_power_test(POWER_TEST_FAILED, "The outlet did not remain on during the test.");
        return;
    }
    const shelly_sample *meter = &device->view.outlet;
    if (!meter->flip_back_timer || meter->timer_seconds != POWER_TEST_SECONDS) {
        stop_power_test(POWER_TEST_FAILED, "The outlet did not confirm its automatic shutoff timer.");
        return;
    }
    power_test.last_read = device->view.outlet_time;
    if (!meter->power_known || !meter->energy_known || meter->watts > DISPATCH_METER_CHANNEL_WATTS) {
        stop_power_test(POWER_TEST_FAILED, "The outlet meter stopped reporting usable power or exceeded the test range.");
        return;
    }
    const bool miner_api = state.settings.device[slot].miner_ip[0];
    const cgminer_sample *miner = &device->view.miner;
    const unsigned warmup = miner_api ? TEST_HASH_WARMUP : TEST_METER_WARMUP;
    const bool operating = now - power_test.started >= warmup &&
        (!miner_api || (device->view.miner_polled && now - device->view.miner_time <= POLL_SECONDS &&
         miner->responding && miner->activity_known && miner->rate_window_seconds && miner->mhs > 0));
    power_test.phase = operating ? POWER_TEST_MEASURING : POWER_TEST_WARMING;
    const unsigned seconds = miner_api ? TEST_HASH_STABLE : TEST_METER_STABLE;
    if (dispatch_power_test_sample(&power_test.samples, now, (uint16_t)ceil(meter->watts), operating,
                                   seconds, &power_test.result)) {
        stop_power_test(POWER_TEST_COMPLETE, "Measured draw saved. Solar control can use the learned estimate.");
    }
}

static void telemetry_budget(uint32_t day)
{
    if (!day || day != state.budget.day) return;
    const uint64_t used = state.budget.used_nano_cents / 1000000;
    // Weekend plans share the installation allowance.
    telemetry.sample.spent_millicents = used > UINT32_MAX ? UINT32_MAX : used;
    telemetry.sample.allowance_millicents = state.settings.policy.daily_allowance_millicents;
    telemetry.sample.flags |= TIMELINE_BUDGET;
    if (state.budget.gap_mask) telemetry.sample.flags |= TIMELINE_ACCOUNTING_GAP;
}

static void telemetry_tick(uint32_t now)
{
    const time_t utc = network_clock();
    if (!telemetry_advance(utc)) return;
    const dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    uint32_t day = 0;
    if (local_time(utc, &day, NULL)) telemetry_budget(day);
    if (bridge_active) telemetry.sample.flags |= TIMELINE_BRIDGE;
    else telemetry.sample.flags &= ~TIMELINE_BRIDGE;
    telemetry.sample.productive = in.productive_mask;
    if (solar_reply_fresh(now)) {
        const uint32_t max_watts = (UINT16_MAX - 1) * 10U;
        const uint32_t watts = solar.reading.watts > max_watts ? max_watts : solar.reading.watts;
        const uint16_t decawatts = watts / 10;
        if (!telemetry.solar_count || decawatts < telemetry.sample.solar_min) telemetry.sample.solar_min = decawatts;
        if (!telemetry.solar_count || decawatts > telemetry.sample.solar_max) telemetry.sample.solar_max = decawatts;
        telemetry.solar_watts += watts;
        ++telemetry.solar_count;
        telemetry.observed = true;
    }
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        timeline_miner *miner = telemetry_miner(i);
        if (!miner->identity) continue;
        const device_runtime *device = &runtime[i];
        const dispatch_outlet *outlet = &in.outlet[i];
        uint8_t status = miner->status & (TIMELINE_METERED | TIMELINE_CATCHUP);
        if (!state.enabled || storage_error) status |= TIMELINE_PREVIEW;
        if (outlet->fresh) {
            status |= outlet->on ? TIMELINE_OUTLET_ON : TIMELINE_OUTLET_OFF;
            if (outlet->on && device->view.power_mode.status == GOLDSHELL_MODE_READY &&
                device->power_checked && now - device->power_checked <= MODE_FRESH_SECONDS &&
                device->power_probe_epoch == network_epoch()) status |= device->view.power_mode.mode;
        }
        miner->status = status;
        uint8_t reason = decisions[i].reason;
        miner->reason = (miner->reason & ~TIMELINE_REASON_MASK) | reason;
        telemetry.observed = true;
    }
}

const char *installation_next_label(operation_change kind)
{
    static const char *const labels[] = {
        [NEXT_PAUSE_END] = "Pause expires",
        [NEXT_WINDOW_OPEN] = "Mining hours open",
        [NEXT_WINDOW_CLOSE] = "Mining hours end",
        [NEXT_PAID_OPEN] = "Paid mining window opens",
        [NEXT_PAID_CLOSE] = "Paid mining window closes",
        [NEXT_SOURCE_GRACE_END] = "Grace period for missing energy readings ends",
        [NEXT_QUIET_START] = "Quiet Hours begin",
        [NEXT_QUIET_END] = "Quiet Hours end",
        [NEXT_RATE] = "Electricity rate boundary",
        [NEXT_BUDGET] = "New daily allowance",
        [NEXT_OVERRIDE] = "Temporary override expires",
        [NEXT_RECOVERY] = "Recovery hold expires",
        [NEXT_DWELL] = "Minimum run or off time ends",
        [NEXT_STAGGER] = "Staggered start interval ends",
        [NEXT_TIME_CHANGE] = "Local clock offset changes",
        [NEXT_SOLAR_RESTART] = "Outlet restart lockout ends",
        [NEXT_SOLAR_SHED] = "Solar shedding delay ends",
        [NEXT_WEEKEND_PLAN] = "Weekend plan takes effect",
        [NEXT_WEEKDAY_PLAN] = "Weekday plan takes effect",
        [NEXT_SUNRISE] = "Sunrise opens daylight gap coverage",
        [NEXT_SUNSET] = "Sunset ends daylight gap coverage",
        [NEXT_DAYLIGHT_BRIDGE_END] = "Daylight gap time limit ends",
    };
    return labels[kind];
}

static void consider_next(operation_next *next, operation_change kind, unsigned slot, uint32_t seconds)
{
    if (seconds && (!next->kind || seconds < next->seconds)) {
        *next = (operation_next){.kind = kind, .slot = slot, .seconds = seconds};
    }
}

static uint32_t until_minute(uint32_t minute, uint32_t now_minute, time_t utc)
{
    unsigned delta = (minute + 1440 - now_minute) % 1440;
    return (delta ? delta : 1440) * 60 - utc % 60;
}

static operation_next next_operation(const dispatch_inputs *in, const dispatch_decision *decisions, time_t utc)
{
    operation_next next = {0};
    uint32_t day, minute;
    if (!local_time(utc, &day, &minute) || storage_error) return next;
    if (in->paused) {
        if (!state.pause_indefinite && state.pause_until > (uint64_t)utc)
            consider_next(&next, NEXT_PAUSE_END, DISPATCH_LIMIT, state.pause_until - utc);
    } else if (in->clock_valid) {
        const bool weekend = state.weekend.enabled && dispatch_is_weekend(day);
        const dispatch_tariff *tariff = weekend ? &state.weekend.tariff : &state.tariff;
        if (state.weekend.enabled && weekend != dispatch_is_weekend(day + 1)) {
            consider_next(&next, weekend ? NEXT_WEEKDAY_PLAN : NEXT_WEEKEND_PLAN,
                          DISPATCH_LIMIT, until_minute(0, minute, utc));
        }
        if (in->quiet.enabled) {
            consider_next(&next, NEXT_QUIET_START, DISPATCH_LIMIT, until_minute(in->quiet.start_minute, minute, utc));
            consider_next(&next, NEXT_QUIET_END, DISPATCH_LIMIT, until_minute(in->quiet.end_minute, minute, utc));
        }
        if (in->solar.bridge.enabled) {
            const dispatch_daylight sun = daylight_at(utc);
            if (sun.kind == DAYLIGHT_RISE_SET && sun.sunrise > utc)
                consider_next(&next, NEXT_SUNRISE, DISPATCH_LIMIT, sun.sunrise - utc);
            else if (sun.kind == DAYLIGHT_RISE_SET && sun.sunset > utc)
                consider_next(&next, NEXT_SUNSET, DISPATCH_LIMIT, sun.sunset - utc);
            else if (daylight_cache.tomorrow.kind == DAYLIGHT_RISE_SET && daylight_cache.tomorrow.sunrise > utc)
                consider_next(&next, NEXT_SUNRISE, DISPATCH_LIMIT, daylight_cache.tomorrow.sunrise - utc);
            if (dispatch_paid_permission(in) == DISPATCH_DAYLIGHT_BRIDGE)
                consider_next(&next, NEXT_DAYLIGHT_BRIDGE_END, DISPATCH_LIMIT, in->solar.bridge.seconds);
        }
        if (in->policy.start_minute != in->policy.end_minute) {
            consider_next(&next, NEXT_WINDOW_OPEN, DISPATCH_LIMIT, until_minute(in->policy.start_minute, minute, utc));
            consider_next(&next, NEXT_WINDOW_CLOSE, DISPATCH_LIMIT, until_minute(in->policy.end_minute, minute, utc));
        }
        if (in->operating.paid_enabled && in->operating.paid_start != in->operating.paid_end) {
            consider_next(&next, NEXT_PAID_OPEN, DISPATCH_LIMIT, until_minute(in->operating.paid_start, minute, utc));
            consider_next(&next, NEXT_PAID_CLOSE, DISPATCH_LIMIT, until_minute(in->operating.paid_end, minute, utc));
        }
        if (in->source_grace && (!in->solar.fresh || !in->solar.load_known))
            consider_next(&next, NEXT_SOURCE_GRACE_END, DISPATCH_LIMIT,
                          SOURCE_GRACE_SECONDS + 1 - (in->now - last_usable_power));
        for (unsigned i = 0; i < DISPATCH_RATE_WINDOWS; ++i) {
            const dispatch_rate_window *window = &tariff->window[i];
            if (!window->enabled) continue;
            consider_next(&next, NEXT_RATE, DISPATCH_LIMIT, until_minute(window->start_minute, minute, utc));
            consider_next(&next, NEXT_RATE, DISPATCH_LIMIT, until_minute(window->end_minute, minute, utc));
        }
        if (in->policy.daily_allowance_millicents) consider_next(&next, NEXT_BUDGET, DISPATCH_LIMIT, until_minute(0, minute, utc));
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (!in->outlet[i].present || !mapping_allows_control(i)) continue;
            const slot_coordination *slot = &state.coordination[i];
            if (slot->override_until > (uint64_t)utc) consider_next(&next, NEXT_OVERRIDE, i, slot->override_until - utc);
            if (slot->recovery_until > (uint64_t)utc) consider_next(&next, NEXT_RECOVERY, i, slot->recovery_until - utc);
            const dispatch_reason reason = decisions[i].reason;
            if (reason == DISPATCH_DWELL || reason == DISPATCH_QUIET_DWELL) {
                const uint32_t dwell = in->outlet[i].on ? DISPATCH_MIN_ON : DISPATCH_MIN_OFF;
                const uint32_t elapsed = in->now - in->outlet[i].since;
                if (elapsed < dwell) consider_next(&next, NEXT_DWELL, i, dwell - elapsed);
            }
            if (reason == DISPATCH_START_PENDING && in->now < in->next_start)
                consider_next(&next, NEXT_STAGGER, DISPATCH_LIMIT, in->next_start - in->now);
            if (reason == DISPATCH_SOLAR_RESTART)
                consider_next(&next, NEXT_SOLAR_RESTART, i, dispatch_solar_restart_remaining(in, i));
            if (reason == DISPATCH_SOLAR_SHED_DELAY)
                consider_next(&next, NEXT_SOLAR_SHED, DISPATCH_LIMIT, in->solar.shed_wait_seconds);
        }
        const time_t transition = timezone_transition(active_timezone, utc);
        if (transition > utc && next.kind && transition - utc <= next.seconds) {
            next = (operation_next){.kind = NEXT_TIME_CHANGE, .slot = DISPATCH_LIMIT, .seconds = transition - utc};
        }
    }
    if (next.kind && !local_time(utc + next.seconds, &next.day, &next.minute)) return (operation_next){0};
    return next;
}

static const char *power_mode_block(unsigned slot, const dispatch_inputs *in, dispatch_decision decision, bool preview);
static bool preset_observed(unsigned slot, uint32_t now);
static dispatch_solar_preset_decision solar_preset_choice(const dispatch_inputs *in,
    const dispatch_decision decisions[DISPATCH_LIMIT], bool preview, bool recheck);

void installation_read(installation_view *view)
{
    lock();
    const uint32_t now = network_uptime();
    const time_t utc = network_clock();
    dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    *view = (installation_view){
        .now = now, .enabled = state.enabled, .day = state.budget.day,
        .used_nano_cents = state.budget.used_nano_cents,
        .worker_stack = worker_stack, .clock_valid = in.clock_valid,
        .accounting_complete = in.accounting_valid, .paused = in.paused,
        .pause_indefinite = state.pause_indefinite,
        .next = next_operation(&in, decisions, utc),
        .pause_seconds = utc && state.pause_until > (uint64_t)utc
            ? state.pause_until - utc : state.pause_unclocked,
        .storage_error = storage_error, .time_synchronized = utc != 0, .meter_gap = state.budget.gap_mask != 0,
        .quiet = state.quiet, .quiet_active = dispatch_quiet_active(&in),
        .tariff = state.tariff, .rate_known = in.clock_valid || !uses_rate_windows(),
        .highest_rate_used = state.budget.day && state.highest_rate_day == state.budget.day,
    };
    local_time(utc, &view->local_day, NULL);
    view->weekend = state.weekend;
    view->active_weekend = view->local_day && state.weekend.enabled && dispatch_is_weekend(view->local_day);
    memcpy(view->notifications.ip, state.notifications.ip, sizeof(view->notifications.ip));
    view->notifications.port = state.notifications.port;
    view->notifications.sent = notifications.sent;
    view->notifications.failed = notifications.failed;
    view->notifications.dropped = notifications.dropped;
    view->notifications.test = notifications.test;
    for (unsigned i = 0; i < NOTIFICATION_QUEUE_SIZE; ++i) {
        view->notifications.pending += notifications.jobs[i].sequence != 0;
    }
    strcpy(view->mining.node_ip, state.mining.node_ip);
    strcpy(view->mining.datum_ip, state.mining.datum_ip);
    strcpy(view->mining.miner_host, state.mining.miner_host);
    strcpy(view->mining.node_host, state.mining.node_host);
    strcpy(view->mining.datum_host, state.mining.datum_host);
    view->mining.node_port = state.mining.node_port;
    view->mining.datum_port = state.mining.datum_port;
    view->mining.miner_port = state.mining.miner_port;
    view->rpc_configured = state.mining.rpc_user[0];
    view->node_sampled = path_fresh(now, path.node_at, path.node_sampled);
    view->datum_sampled = path_fresh(now, path.datum_at, path.datum_sampled);
    view->node_seen = path.node_sampled;
    view->datum_seen = path.datum_sampled;
    view->node = (node_reading){.status = node_status(now)};
    view->datum = (datum_reading){.status = datum_status(now)};
    if (view->node_sampled) {
        view->node.blocks = path.node.blocks;
        view->node.headers = path.node.headers;
        view->node_age = now - path.node_at;
    }
    if (view->datum_sampled) {
        view->datum = path.datum;
        view->datum_age = now - path.datum_at;
    }
    if (view->rate_known) {
        view->active_rate = dispatch_rate_at(in.policy.rate_millicents_per_kwh,
                                             view->active_weekend ? &state.weekend.tariff : &state.tariff,
                                             in.minute, &view->active_rate_window);
    }
    view->settings.policy = state.settings.policy;
    view->operating = state.operating;
    view->plan_ready = state.plan_ready;
    view->settings.utc_offset_minutes = state.settings.utc_offset_minutes;
    view->energy.solar = state.solar;
    view->energy.follow = state.follow;
    view->energy.generation = in.solar;
    view->energy.paid_permission = dispatch_paid_permission(&in);
    if (view->energy.paid_permission == DISPATCH_DAYLIGHT_BRIDGE)
        view->energy.gap_budget = dispatch_daylight_forecast(&in, 0);
    view->energy.daylight = state.daylight;
    const dispatch_daylight sun = daylight_at(utc);
    view->energy.daylight_kind = sun.kind;
    if (sun.kind == DAYLIGHT_RISE_SET) {
        uint32_t sunrise, sunset;
        if (local_minute(sun.sunrise, &sunrise) && local_minute(sun.sunset, &sunset)) {
            view->energy.sunrise_minute = sunrise;
            view->energy.sunset_minute = sunset;
        } else view->energy.daylight_kind = DAYLIGHT_UNKNOWN;
    }
    if (in.solar.configured && in.solar.fresh && in.solar.load_known) {
        view->energy.solar_covered_watts = dispatch_solar_coverage(in.solar.production, in.solar.reserve, in.solar.running_watts);
    }
    view->energy.grid.reserve = state.grid.reserve;
    if (native_grid_type(state.grid.target.identity)) {
        view->energy.grid.native = state.grid.native;
        view->energy.grid.reverse = state.grid.reverse;
    }
    view->energy.grid.direction_verified = state.grid.direction_verified;
    strcpy(view->energy.grid.target.ip, state.grid.target.ip);
    strcpy(view->energy.grid.target.identity, state.grid.target.identity);
    view->energy.production = solar.reading;
    view->energy.solar_result = solar.result;
    view->energy.solar_sampled = solar.sampled;
    view->energy.solar_age = now - solar.at;
    view->energy.solar_fresh = solar_reply_fresh(now);
    view->energy.solar_night = solar_night(utc);
    view->energy.grid_watts = grid.watts;
    view->energy.grid_result = grid.result;
    view->energy.grid_sampled = grid.sampled;
    view->energy.grid_attempted = grid.attempted;
    view->energy.grid_age = now - grid.at;
    view->energy.grid_fresh = grid_fresh(now);
    view->energy.grid_source = in.grid_source;
    view->energy.fronius_solar_watts = grid.fronius_watts;
    view->energy.fronius_solar_known = grid.fronius_known && now - grid.fronius_at <= GRID_FRESH &&
        grid.epoch == network_epoch() && network_online();
    view->energy.gx_target = gx.observed;
    view->energy.gx_requested = gx.requested;
    view->energy.gx_target_known = gx.known && now - gx.at <= 2 * GRID_FRESH &&
        gx.epoch == network_epoch() && network_online();
    view->energy.gx_command = gx.command;
    view->energy.gx_result = gx.result;
    strcpy(view->mqtt_ip, state.mqtt.ip);
    strcpy(view->mqtt_tls_name, state.mqtt.tls_name);
    view->mqtt_port = state.mqtt.port;
    view->mqtt_probe_pending = mqtt_probe.pending;
    view->mqtt_probe_attempted = mqtt_probe.attempted;
    view->mqtt_probe_result = mqtt_probe.result;
    view->mqtt_probe_on = mqtt_probe.on;
    memcpy(view->home_meter, state.home_meter, sizeof(view->home_meter));
    home_view *bridge = &view->home;
    strcpy(bridge->ip, state.home.endpoint.ip);
    strcpy(bridge->battery, state.home.battery);
    strcpy(bridge->temperature, state.home.temperature);
    strcpy(bridge->actuator, state.home.actuator);
    bridge->port = state.home.endpoint.port;
    const bool home_current = network_online() && home.epoch == network_epoch();
    bridge->battery_known = home_current && home.known[0] && now - home.at[0] <= OBSERVATION_TTL;
    bridge->temperature_known = home_current && home.known[1] && now - home.at[1] <= OBSERVATION_TTL;
    bridge->actuator_known = home_current && home.known[2] && now - home.at[2] <= OBSERVATION_TTL;
    bridge->battery_percent = home.battery;
    bridge->temperature_c = home.temperature;
    bridge->control = home.observed;
    bridge->command = home.command;
    bridge->result = home.result;
    strcpy(view->timezone, state.timezone);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        // Copy endpoint labels only. The HTTP task never receives equipment secrets.
        const association *source = &state.settings.device[i];
        association *target = &view->settings.device[i];
        strcpy(target->shelly.ip, source->shelly.ip);
        strcpy(target->shelly.identity, source->shelly.identity);
        strcpy(target->miner_ip, source->miner_ip);
        view->device[i] = runtime[i].view;
        view->device[i].first_run = state.first_run[i];
        view->device[i].restart_ready = restart_ready(i, now);
        if (restart_job.slot == i && restart_job.revision == generation) {
            view->device[i].restart_command = restart_job.command;
            view->device[i].restart_result = restart_job.result;
        }
        view->device[i].running_seconds = in.outlet[i].fresh && in.outlet[i].on ? now - runtime[i].since : 0;
        view->device[i].solar_restart_seconds = decisions[i].reason == DISPATCH_SOLAR_RESTART ?
            dispatch_solar_restart_remaining(&in, i) : 0;
        view->device[i].profile = state.profile[i];
        view->device[i].learned_startup_watts = startup_watts(i, 0);
        power_test_view *test = &view->device[i].power_test;
        test->measured = state.power_test[i];
        test->blocked = test_start_block(i, &in, false);
        if (power_test.slot == i) {
            test->phase = power_test.phase;
            test->seconds = now < power_test.lease ? power_test.lease - now : 0;
            test->peak = power_test.samples.peak;
            test->stable_seconds = power_test.samples.count && now >= power_test.samples.since ?
                now - power_test.samples.since : 0;
            test->detail = power_test.detail;
        }
        view->device[i].efficiency = state.efficiency[i];
        memcpy(view->device[i].presets, state.presets[i].mode, sizeof(view->device[i].presets));
        view->device[i].measured_alternate = state.presets[i].identity.alternate;
        view->device[i].preset_authorized = preset_authorized(i);
        view->device[i].preset_evaluable = preset_observed(i, now);
        view->device[i].preset_evaluating = state.evaluation.active && state.evaluation.slot == i;
        view->device[i].preset_restoring = view->device[i].preset_evaluating && state.evaluation.restoring;
        view->preset_choice[i] = preset_choice(i);
        view->device[i].preset_target = state.pinned_preset[i];
        view->device[i].preset_request = view->device[i].preset_evaluating ? PRESET_EVALUATE :
            view->device[i].preset_target && view->device[i].preset_authorized ? PRESET_APPLY_EFFICIENCY : PRESET_NO_REQUEST;
        memcpy(view->device[i].name, state.equipment_name[i], sizeof(view->device[i].name));
        view->device[i].destination = destination_at(i, now);
        view->device[i].coordination = state.coordination[i];
        view->device[i].override_seconds = utc && state.coordination[i].override_until > (uint64_t)utc
            ? state.coordination[i].override_until - utc : 0;
        view->device[i].recovery_seconds = utc && state.coordination[i].recovery_until > (uint64_t)utc
            ? state.coordination[i].recovery_until - utc : 0;
        view->device[i].stalled = stalled(i, now);
        view->device[i].work_missing = runtime[i].work.active;
        view->device[i].recovery_exhausted = runtime[i].recovery_exhausted;
        view->device[i].power_block = power_mode_block(i, &in, decisions[i], false);
        view->device[i].power_pending = power_job.pending && power_job.slot == i;
        view->device[i].power_current = view->device[i].power_mode.status == GOLDSHELL_MODE_READY &&
            runtime[i].power_checked && now - runtime[i].power_checked <= MODE_FRESH_SECONDS &&
            in.outlet[i].fresh && in.outlet[i].on;
        view->device[i].recovery_status = recovery_status(i, now, view->local_day, utc);
        view->device[i].decision = decisions[i];
        view->device[i].observed = in.outlet[i].fresh;
        view->device[i].command_pending = runtime[i].pending != DISPATCH_KEEP &&
            !(state.hold_mask & (1u << i));
        if (!network_online() || runtime[i].connection_epoch != network_epoch() ||
            now - view->device[i].miner_time > OBSERVATION_TTL) {
            view->device[i].miner_polled = false;
            view->device[i].miner = (cgminer_sample){0};
        }
    }
    view->preset_credentials_persistent = preset_credentials_persistent();
    const dispatch_solar_preset_decision preset = solar_preset_choice(&in, decisions, !state.enabled, false);
    const unsigned bridge_slot = preset.action == DISPATCH_PRESET_BRIDGE_LOWER ? preset.slot :
        power_job.pending && power_job.source == POWER_BRIDGE_LOWER ? power_job.slot : DISPATCH_LIMIT;
    if (bridge_slot < DISPATCH_LIMIT && view->energy.gap_budget.known) {
        const device_runtime *device = &runtime[bridge_slot];
        const uint32_t draw = (uint32_t)ceil(device->view.outlet.watts);
        const uint32_t reduced = measured_preset_watts(bridge_slot, device->view.power_mode.alternate);
        if (reduced && draw > reduced) view->energy.gap_budget_after = dispatch_daylight_forecast(&in, draw - reduced);
    }
    if (preset.action != DISPATCH_PRESET_KEEP) {
        view->device[preset.slot].preset_request = preset.action == DISPATCH_PRESET_BRIDGE_LOWER ? PRESET_LOWER_BRIDGE :
            preset.action == DISPATCH_PRESET_LOWER ? PRESET_LOWER_SOLAR : PRESET_RESTORE_SOLAR;
    } else if (power_job.pending && power_job.source == POWER_BRIDGE_LOWER) {
        view->device[power_job.slot].preset_request = PRESET_LOWER_BRIDGE;
    } else if (power_job.pending && power_job.source == POWER_SOLAR_LOWER) {
        view->device[power_job.slot].preset_request = PRESET_LOWER_SOLAR;
    } else if (power_job.pending && power_job.source == POWER_SOLAR_RESTORE) {
        view->device[power_job.slot].preset_request = PRESET_RESTORE_SOLAR;
    }
    memcpy(view->days, state.days, sizeof(state.days));
    memcpy(view->solar_days, state.solar_days, sizeof(state.solar_days));
    memcpy(view->preset_days, state.preset_days, sizeof(state.preset_days));
    view->history_count = history_count;
    for (unsigned i = 0; i < history_count; ++i) {
        view->history[i] = history[(history_next + HISTORY_SIZE - 1 - i) % HISTORY_SIZE];
    }
    unlock();
}

static void account(unsigned index, const shelly_sample *sample, time_t utc, dispatch_daily_evidence *summary)
{
    const uint32_t day = summary->day;
    const uint32_t previous_day = state.budget.day;
    if (!dispatch_budget_day(&state.budget, day)) {
        return;
    }
    if (previous_day != day) state.budget.used_nano_cents = summary->cost_nano_cents;
    const dispatch_meter_anchor old = state.budget.anchor[index];
    const bool expired = solar_credit.watt_seconds[index] &&
        solar_credit.at - solar_credit.first_credit[index] > DISPATCH_PRORATE_SECONDS;
    const uint64_t available_solar = expired ? 0 : solar_credit.watt_seconds[index] * 1000ULL / 3600;
    // Meter counters can report the same total for several polls. Keep
    // capacity for those intervals until the meter actually advances.
    if (expired || !old.epoch || (uint64_t)utc < old.epoch || sample->total_mwh != old.total_mwh)
        solar_credit.watt_seconds[index] = 0;
    const uint8_t previous_gap = state.budget.gap_mask;
    const uint64_t previous_cost = state.budget.used_nano_cents;
    uint32_t rate = state.settings.policy.rate_millicents_per_kwh;
    bool highest = false;
    if (old.epoch && sample->total_mwh > old.total_mwh && uses_rate_windows()) {
        rate = dispatch_meter_rate(rate, &state.tariff, state.weekend.enabled ? &state.weekend.tariff : NULL,
                                   old.epoch, utc, tariff_local_time, &highest);
    }
    const dispatch_energy_sample energy = dispatch_budget_split_sample(&state.budget, index, sample->total_mwh,
        utc, rate, available_solar);
    if (telemetry_advance(utc)) {
        timeline_miner *miner = telemetry_miner(index);
        if (old.epoch && (uint64_t)utc >= old.epoch && sample->total_mwh >= old.total_mwh &&
            energy.metered_mwh == sample->total_mwh - old.total_mwh) {
            const uint64_t paid = energy.metered_mwh - energy.solar_mwh;
            miner->paid_mwh += paid > UINT32_MAX - miner->paid_mwh ? UINT32_MAX - miner->paid_mwh : paid;
            if (paid && sample->on && state.enabled && !storage_error && !testing_outlet(index) &&
                (uint64_t)utc - old.epoch <= OBSERVATION_TTL) miner->reason |= paid_authority;
            telemetry.cost[index] += state.budget.used_nano_cents - previous_cost;
            miner->status |= TIMELINE_METERED;
            if ((uint64_t)utc - old.epoch > OBSERVATION_TTL && paid) miner->status |= TIMELINE_CATCHUP;
            telemetry.observed = true;
        }
        telemetry_budget(day);
    }
    solar_daily_evidence *credit = &state.solar_days[day % DISPATCH_EVIDENCE_DAYS];
    summary->energy_mwh += energy.metered_mwh;
    credit->energy_mwh += energy.solar_mwh;
    credit->savings_nano_cents += energy.solar_mwh * rate;
    summary->cost_nano_cents += state.budget.used_nano_cents - previous_cost;
    if (!summary->metered) {
        summary->metered = true;
        dirty = true;
    }
    if (highest && state.budget.used_nano_cents > previous_cost) {
        state.highest_rate_day = day;
    }
    if (previous_gap != state.budget.gap_mask) {
        record(index, EVENT_COUNTER_GAP);
        dirty = true;
    }
    if (state.budget.day != previous_day || !old.epoch || old.total_mwh != sample->total_mwh) {
        dirty = true;
    }
}

static void observe_first_run(unsigned index, const cgminer_sample *miner)
{
    uint8_t *flags = &state.first_run[index];
    if (!(*flags & RUN_MAPPING)) return;
    if (!(*flags & RUN_POWER)) {
        *flags |= RUN_POWER;
        record(index, EVENT_FIRST_POWER);
        dirty = true;
    }
    if (!(*flags & RUN_WORK) && miner && miner->responding && miner->activity_known &&
        miner->rate_window_seconds && miner->mhs > 0) {
        *flags |= RUN_WORK;
        record(index, EVENT_FIRST_WORK);
        dirty = true;
    }
}

static void reset_preset_sample(device_runtime *device)
{
    memset(&device->preset_sample, 0, sizeof(device->preset_sample));
}

static bool hashing_preset(const goldshell_mode_reading *reading)
{
    return reading->status == GOLDSHELL_MODE_READY && reading->model &&
        (reading->alternate == GOLDSHELL_MODE_LOW || reading->alternate == GOLDSHELL_MODE_BALANCE) &&
        (reading->mode == GOLDSHELL_MODE_DEFAULT || reading->mode == reading->alternate);
}

static void observe_preset(unsigned slot, uint32_t now, uint32_t day, bool qualified, double watts, double mhs)
{
    device_runtime *device = &runtime[slot];
    if (!qualified || !(watts > 0) || !day || !mapping_allows_control(slot) ||
        !hashing_preset(&device->view.power_mode) || !device->power_checked ||
        now - device->power_checked > PRESET_CONFIRM_SECONDS || now < device->power_settle_until ||
        now - device->since < DISPATCH_MIN_ON || (power_job.pending && power_job.slot == slot && power_job.desired)) {
        reset_preset_sample(device);
        return;
    }

    const uint32_t elapsed = now - device->preset_sample.at;
    if (!device->preset_sample.at || device->preset_sample.day != day || elapsed > OBSERVATION_TTL) {
        reset_preset_sample(device);
    } else if (watts < device->preset_sample.watts * 0.8 || watts * 0.8 > device->preset_sample.watts ||
               mhs < device->preset_sample.mhs * 0.8 || mhs * 0.8 > device->preset_sample.mhs) {
        // Reconfirm after a step change before assigning subsequent work to the old preset.
        reset_preset_sample(device);
        device->power_checked = 0;
        device->next_power_probe = 0;
        return;
    } else if (!dispatch_preset_sample(&device->preset_sample.pending, elapsed,
                   (watts + device->preset_sample.watts) / 2, (mhs + device->preset_sample.mhs) / 2,
                   fmax(watts, device->preset_sample.watts))) {
        reset_preset_sample(device);
    }
    device->preset_sample.day = day;
    device->preset_sample.at = now;
    device->preset_sample.watts = watts;
    device->preset_sample.mhs = mhs;
}

static void observation(unsigned index, shelly_result result, const shelly_sample *outlet,
                        const cgminer_sample *miner, bool outlet_update, uint32_t now)
{
    device_runtime *d = &runtime[index];
    device_view *v = &d->view;
    const bool recent = v->observed && d->connection_epoch == network_epoch() &&
        now - v->outlet_time <= OBSERVATION_TTL;
    bool was_fresh = fresh(d, now);
    if (outlet_update && (!was_fresh || result != SHELLY_OK || !outlet->on)) {
        d->power_checked = 0;
        d->next_power_probe = 0;
    }
    const bool hashing = miner && miner->activity_known && miner->rate_window_seconds && miner->mhs > 0;
    const bool was_hashing = v->miner.activity_known && v->miner.rate_window_seconds && v->miner.mhs > 0;
    if (miner && v->miner_polled && v->power_mode.idle_supported &&
        (hashing != was_hashing || miner->responding != v->miner.responding)) d->next_power_probe = 0;
    if (hashing && miner_idle(d, now)) {
        d->power_checked = 0;
        d->next_power_probe = 0;
    }
    const bool work_paused = miner_work_paused(index, now);
    const bool lost_outlet = outlet_update && v->observed && v->outlet_result == SHELLY_OK &&
        (result != SHELLY_OK || !was_fresh);
    const bool lost_miner = miner && !work_paused && result == SHELLY_OK && was_fresh && v->outlet.on && outlet->on &&
        v->miner_polled && v->miner.responding && !miner->responding;
    const time_t utc = network_clock();
    const uint32_t day = reconcile_budget_date(utc);
    if (day) accrue_solar_credit(now);
    else reset_solar_credit();
    if (day && (result == SHELLY_OK || lost_outlet || lost_miner)) {
        dispatch_daily_evidence *summary = day_evidence(day);
        if (outlet_update && result == SHELLY_OK && outlet->energy_known) account(index, outlet, utc, summary);
        if (outlet_update && result == SHELLY_OK && was_fresh && outlet->on && v->outlet.on && d->observation_day == day) {
            summary->powered_seconds += now - v->outlet_time;
            dirty = true;
        }
        if (lost_outlet || lost_miner) {
            ++summary->interruptions;
            dirty = true;
        }
    }
    if (outlet_update) d->observation_day = day;
    if (d->connection_epoch != network_epoch()) {
        d->destination = DESTINATION_UNKNOWN;
        d->next_destination = 0;
    }
    const bool zero = miner && !work_paused && result == SHELLY_OK && outlet->on &&
        miner->activity_known && miner->rate_window_seconds && miner->mhs == 0;
    const path_status datum = zero ? datum_status(now) : PATH_WAITING;
    if (miner || result != SHELLY_OK || !outlet->on) {
        if (!zero || !state.coordination[index].recovery || node_status(now) != PATH_READY ||
            (datum != PATH_READY && datum != PATH_SOLO) || destination_at(index, now) != DESTINATION_MATCH) {
            d->zero_since = 0;
        } else if (!d->zero_since || !was_fresh || now - v->miner_time > OBSERVATION_TTL) {
            d->zero_since = now ? now : 1;
        }
    }
    if (outlet_update) d->connection_epoch = network_epoch();
    if (outlet_update && result == SHELLY_OK) {
        const bool changed = v->observed && outlet->on != v->outlet.on;
        // A recovered reading of an already off outlet is not a new stop.
        if (outlet->on) d->solar_off_since = 0;
        else if (changed) d->solar_off_since = now;
        if (outlet->on && (!v->outlet.on || !was_fresh)) d->next_miner = 0;
        if (!recent || changed) {
            d->since = now;
            d->startup_tracking = was_fresh && outlet->on;
            d->startup_at = now;
            record(index, outlet->on ? EVENT_OUTLET_ON : EVENT_OUTLET_OFF);
        }
        if (d->pending != DISPATCH_KEEP && (d->pending == DISPATCH_ON) == outlet->on) {
            record_action(index, EVENT_CONFIRMED, d->pending, EVENT_NO_REASON);
            d->pending = DISPATCH_KEEP;
            d->tries = 0;
            state.attempt_counts &= ~(3u << (2 * index));
            if (!(state.hold_mask & (1u << index))) {
                state.hold_on &= ~(1u << index);
            }
            dirty = true;
        }
        if (d->recovery_requested_at) {
            if (now - d->recovery_requested_at >= RECOVERY_OFF_SECONDS) {
                d->recovery_requested_at = 0;
            } else if (!outlet->on) {
                record_action(index, EVENT_RECOVERY_OFF_OBSERVED, DISPATCH_OFF, EVENT_NO_REASON);
                d->recovery_requested_at = 0;
            }
        }
        if (outlet->power_known && telemetry_advance(utc)) {
            timeline_miner *sample = telemetry_miner(index);
            const uint16_t watts = outlet->watts >= UINT16_MAX ? UINT16_MAX - 1 : outlet->watts;
            if (!telemetry.samples[index] || watts < sample->minimum) sample->minimum = watts;
            if (!telemetry.samples[index] || watts > sample->maximum) sample->maximum = watts;
            telemetry.watts[index] += watts;
            ++telemetry.samples[index];
            telemetry.observed = true;
        }
        v->outlet = *outlet;
        v->observed = true;
        v->outlet_time = now;
    } else if (outlet_update) {
        if (was_fresh) {
            record(index, EVENT_OUTLET_LOST);
        }
        d->startup_tracking = false;
    }
    if (outlet_update) v->outlet_result = result;
    if (miner && (!v->miner_polled || v->miner.responding != miner->responding) && (miner->responding || !work_paused)) {
        record(index, miner->responding ? EVENT_MINER_RESPONDING : EVENT_MINER_LOST);
    }
    if (miner) {
        if (result == SHELLY_OK && outlet->on) observe_first_run(index, miner);
        v->miner = *miner;
        v->miner_polled = true;
        v->miner_time = now;
    }
    const bool testing = testing_outlet(index);
    const bool measured = !testing && hashing && result == SHELLY_OK && was_fresh && outlet->on && outlet->power_known;
    if (miner || lost_outlet || (outlet_update && !outlet->on)) observe_preset(index, now, day, measured, measured ? outlet->watts : 0, measured ? miner->mhs : 0);
    if (!testing && hashing && result == SHELLY_OK && outlet->on) {
        dispatch_profile *profile = &state.profile[index];
        if (d->startup_tracking) {
            dispatch_profile_start(profile, now - d->startup_at);
            d->startup_tracking = false;
            dirty = true;
        }
        if (now >= d->next_profile && now >= d->power_settle_until) {
            dispatch_profile_sample(profile, miner->mhs, outlet->power_known, outlet->watts);
            if (!state.presets[index].identity.alternate && outlet->power_known && was_fresh &&
                now - d->since >= DISPATCH_MIN_ON) {
                dispatch_efficiency_sample(&state.efficiency[index], outlet->watts, miner->mhs);
            }
            d->next_profile = now + PROFILE_SECONDS;
            dirty = true;
        }
    }
}

static bool admit_on(void)
{
    return network_online() && !atomic_load(&pause_requested);
}

static bool try_command(unsigned i, uint32_t now, dispatch_action desired,
                        const dispatch_inputs *in, dispatch_decision decision)
{
    if (!state.enabled || storage_error || !network_online()) {
        return false;
    }
    const time_t utc = network_clock();
    const dispatch_action action = decision.action;
    device_runtime *d = &runtime[i];
    if (action == DISPATCH_ON && power_job.pending &&
        power_job.desired != GOLDSHELL_MODE_UNKNOWN) return false;
    if (now < solar_preset_shed_until &&
        in->solar.fresh && in->solar.load_known &&
        (decision.reason == DISPATCH_SOLAR_STOP || decision.reason == DISPATCH_PAID_START)) return false;
    if (action != desired || (action == DISPATCH_ON && !admit_on())) {
        return false;
    }
    if (decision.reason == DISPATCH_RECOVERY) {
        uint32_t day;
        if (d->pending != DISPATCH_KEEP || !local_time(utc, &day, NULL)) return false;
        slot_coordination previous = state.coordination[i];
        state.coordination[i].recovery_day = day;
        state.coordination[i].recovery_until = utc + RECOVERY_OFF_SECONDS;
        if (save() != ESP_OK) {
            state.coordination[i] = previous;
            dirty = true;
            return false;
        }
        d->zero_since = 0;
        record_action(i, EVENT_RECOVERY_ATTEMPT, DISPATCH_OFF, DISPATCH_RECOVERY);
        d->recovery_requested_at = now;
        bool current;
        const shelly_result result = send_relay(i, false, 0, &current);
        if (!current) return true;
        d->next_probe = 0;
        next_energy_change = network_uptime() + GRID_SETTLE;
        record_action(i, result == SHELLY_OK ? EVENT_RECOVERY_ACK : EVENT_RECOVERY_ERROR,
                      DISPATCH_OFF, EVENT_NO_REASON);
        return true;
    }
    if (d->pending != action) {
        d->pending = action;
        d->tries = 0;
        d->next_command = now;
    }
    if (now < d->next_command) {
        return false;
    }
    if (d->tries >= RETRY_LIMIT) {
        state.hold_mask |= 1u << i;
        if (action == DISPATCH_ON) {
            state.hold_on |= 1u << i;
        }
        record_action(i, EVENT_COMMAND_HOLD, action, decision.reason);
        save();
        return false;
    }

    // Commit the attempt before the RPC. A power loss cannot reset the retry
    // allowance between an accepted command and its observed confirmation.
    const uint16_t previous_attempts = state.attempt_counts;
    const uint8_t previous_on = state.hold_on;
    state.attempt_counts = (state.attempt_counts & ~(3u << (2 * i))) | ((d->tries + 1) << (2 * i));
    if (action == DISPATCH_ON) {
        state.hold_on |= 1u << i;
    } else {
        state.hold_on &= ~(1u << i);
    }
    if (save() != ESP_OK) {
        state.attempt_counts = previous_attempts;
        state.hold_on = previous_on;
        dirty = true;
        return false;
    }
    // The adapter checks for a queued pause again after the read-only preflight.
    bool current;
    const shelly_result result = send_relay(i, action == DISPATCH_ON, 0, &current);
    if (!current || !state.enabled) return true;
    if (result == SHELLY_DEFERRED) {
        state.attempt_counts = previous_attempts;
        state.hold_on = previous_on;
        dirty = true;
        return false;
    }
    d->next_probe = 0;
    ++d->tries;
    d->next_command = network_uptime() + RETRY_SECONDS;
    next_energy_change = network_uptime() + GRID_SETTLE;
    if (action == DISPATCH_ON) {
        next_start = network_uptime() + DISPATCH_STAGGER;
    }
    record_action(i, result != SHELLY_OK ? EVENT_COMMAND_ERROR :
                  action == DISPATCH_ON ? EVENT_COMMAND_ON : EVENT_COMMAND_OFF,
                  action, decision.reason);
    return true;
}

static bool gx_allowed(void)
{
    return gx.epoch == network_epoch() && admit_on();
}

static bool home_allowed(void)
{
    return home.epoch == network_epoch() && admit_on();
}

static void gx_tick(void)
{
    lock();
    const uint32_t now = network_uptime(), epoch = network_epoch();
    if (!victron_grid_phases(state.grid.target.identity)) {
        unlock();
        return;
    }
    if (gx.epoch != epoch || !network_online()) {
        gx.known = false;
        gx.next_poll = 0;
        if (gx.command == COMMAND_QUEUED) gx.command = COMMAND_CANCELED;
        if (gx.command == COMMAND_VERIFYING) gx.command = COMMAND_UNCONFIRMED;
        gx.epoch = epoch;
    }
    if (gx.command == COMMAND_QUEUED) {
        if (now >= gx.deadline || !grid_fresh(now) || !admit_on() || paused_at(network_clock())) {
            gx.command = COMMAND_CANCELED;
        } else {
            gx.result = victron_set_grid_target(state.grid.target.ip, gx.expected, gx.requested, gx_allowed);
            gx.command = gx.result == SHELLY_DEFERRED ? COMMAND_CANCELED :
                (gx.result == SHELLY_OK || gx.result == SHELLY_NETWORK) ? COMMAND_VERIFYING : COMMAND_UNCONFIRMED;
            gx.next_poll = 0;
        }
        unlock();
        return;
    }
    if (!network_online() || now < gx.next_poll) {
        unlock();
        return;
    }

    char ip[16];
    strcpy(ip, state.grid.target.ip);
    const uint32_t revision = grid.revision;
    unlock();
    int32_t target;
    const shelly_result result = victron_setpoint(ip, &target);
    lock();
    if (revision == grid.revision && epoch == network_epoch() && network_online()) {
        const bool verifying = gx.command == COMMAND_VERIFYING;
        gx.known = result == SHELLY_OK;
        if (gx.known) {
            gx.observed = target;
            gx.at = network_uptime();
        }
        if (verifying) {
            if (gx.known && target == gx.requested && (target || gx.result == SHELLY_OK) &&
                (gx.expected != gx.requested || gx.result == SHELLY_OK)) {
                gx.command = COMMAND_CONFIRMED;
            } else if (network_uptime() >= gx.deadline) {
                gx.command = COMMAND_UNCONFIRMED;
            }
        }
        gx.next_poll = network_uptime() + (gx.command == COMMAND_VERIFYING ? 2 : 10);
    }
    unlock();
}

static void home_tick(void)
{
    lock();
    const uint32_t now = network_uptime(), epoch = network_epoch();
    if (home.epoch != epoch || !network_online()) {
        memset(home.known, 0, sizeof(home.known));
        home.next_poll = 0;
        if (home.command == COMMAND_QUEUED) home.command = COMMAND_CANCELED;
        if (home.command == COMMAND_VERIFYING) home.command = COMMAND_UNCONFIRMED;
        home.epoch = epoch;
    }
    if (!state.home.endpoint.ip[0] || !network_online()) {
        unlock();
        return;
    }

    const ha_entity_kind kind = actuator_kind(state.home.actuator);
    if (home.command == COMMAND_QUEUED) {
        if (now >= home.deadline || !admit_on() || paused_at(network_clock())) {
            home.command = COMMAND_CANCELED;
        } else {
            const ha_endpoint *endpoint = &state.home.endpoint;
            const char *entity = state.home.actuator;
            if (kind == HA_NUMBER) {
                home.result = ha_number_set(endpoint, entity, home.requested, home_allowed);
            } else if (kind == HA_SELECT) {
                home.result = ha_select_set(endpoint, entity, home.option, home_allowed);
            } else {
                ha_control_state baseline;
                home.result = ha_control_read(endpoint, entity, kind, &baseline);
                if (home.result == SHELLY_OK) {
                    strcpy(home.before, baseline.text);
                    home.result = ha_button_press(endpoint, entity, home_allowed);
                }
            }
            home.command = home.result == SHELLY_DEFERRED ? COMMAND_CANCELED :
                (home.result == SHELLY_OK || home.result == SHELLY_NETWORK) ? COMMAND_VERIFYING : COMMAND_UNCONFIRMED;
            home.next_poll = 0;
        }
        unlock();
        return;
    }
    if (now < home.next_poll) {
        unlock();
        return;
    }

    const char *const sources[] = {state.home.battery, state.home.temperature, state.home.actuator};
    unsigned item = 2;
    if (home.command != COMMAND_VERIFYING) {
        for (unsigned i = 0; i < 3; ++i) {
            item = home.next;
            home.next = (home.next + 1) % 3;
            if (sources[item][0]) break;
        }
    }
    if (!sources[item][0]) {
        unlock();
        return;
    }

    const uint32_t revision = home.revision;
    ha_endpoint endpoint = state.home.endpoint;
    char entity[HA_ENTITY_BYTES];
    strcpy(entity, sources[item]);
    unlock();
    double value;
    ha_control_state control;
    const shelly_result result = item == 2 ? ha_control_read(&endpoint, entity, kind, &control) :
        ha_read_sensor(&endpoint, entity, item == 0 ? HA_BATTERY_PERCENT : HA_TEMPERATURE_C, &value);
    explicit_bzero(&endpoint, sizeof(endpoint));
    lock();
    if (revision == home.revision && epoch == network_epoch() && network_online()) {
        home.known[item] = result == SHELLY_OK;
        if (home.known[item]) {
            home.at[item] = network_uptime();
            if (item == 0) home.battery = value;
            if (item == 1) home.temperature = value;
            if (item == 2) home.observed = control;
        }
        if (item == 2 && home.command == COMMAND_VERIFYING) {
            const bool matched = result == SHELLY_OK && (kind == HA_NUMBER ?
                fabs(control.value - home.requested) <= fmax(control.step * 1e-6, fabs(home.requested) * 1e-12) :
                kind == HA_SELECT ? !strcmp(control.text, home.option) : strcmp(control.text, home.before) != 0);
            if (matched) home.command = COMMAND_CONFIRMED;
            else if (network_uptime() >= home.deadline) home.command = COMMAND_UNCONFIRMED;
        }
        home.next_poll = network_uptime() + (home.command == COMMAND_VERIFYING ? 2 : 5);
    }
    unlock();
}

static void mqtt_probe_tick(void)
{
    lock();
    if (!mqtt_probe.pending) {
        unlock();
        return;
    }
    const uint32_t revision = mqtt_probe.revision, epoch = network_epoch();
    mqtt_endpoint *endpoint = malloc(sizeof(*endpoint));
    if (!endpoint || !network_online()) {
        free(endpoint);
        mqtt_probe.pending = false;
        mqtt_probe.attempted = true;
        mqtt_probe.result = SHELLY_NETWORK;
        unlock();
        return;
    }
    *endpoint = state.mqtt;
    shelly_target target = {0};
    strcpy(target.ip, endpoint->ip);
    strcpy(target.identity, mqtt_probe.identity);
    unlock();

    shelly_sample sample;
    const shelly_result result = outlet_status(&target, NULL, endpoint, &sample);
    explicit_bzero(endpoint, sizeof(*endpoint));
    free(endpoint);
    lock();
    if (revision == mqtt_probe.revision) {
        mqtt_probe.pending = false;
        mqtt_probe.attempted = true;
        mqtt_probe.result = epoch == network_epoch() && network_online() ? result : SHELLY_NETWORK;
        mqtt_probe.on = mqtt_probe.result == SHELLY_OK && sample.on;
    }
    unlock();
}

static void expire_pause(time_t utc)
{
    if (utc && state.pause_unclocked) {
        state.pause_until = utc + state.pause_unclocked;
        state.pause_unclocked = 0;
        save();
    } else if (utc && state.pause_until && (uint64_t)utc >= state.pause_until) {
        state.pause_until = 0;
        record(DISPATCH_LIMIT, EVENT_RESUMED);
        dirty = true;
    }
}

static void log_grid_power(int32_t reading)
{
    if (telemetry_advance(network_clock())) {
        const int32_t max_watts = INT16_MAX * 10;
        const int32_t watts = reading > max_watts ? max_watts : reading < -max_watts ? -max_watts : reading;
        const int16_t decawatts = watts / 10;
        if (!telemetry.grid_count || decawatts < telemetry.sample.grid_min) telemetry.sample.grid_min = decawatts;
        if (!telemetry.grid_count || decawatts > telemetry.sample.grid_max) telemetry.sample.grid_max = decawatts;
        telemetry.grid_watts += watts;
        ++telemetry.grid_count;
        telemetry.observed = true;
    }
}

static void grid_tick(void)
{
    const uint32_t now = network_uptime(), epoch = network_epoch();
    lock();
    if (!state.grid.target.ip[0] || !network_online() || (grid.epoch == epoch && now < grid.poll_at)) {
        unlock();
        return;
    }
    shelly_target target = state.grid.target;
    const native_grid_kind kind = native_grid_type(target.identity);
    const native_grid_config native = state.grid.native;
    const bool reverse = kind != NATIVE_GRID_NONE && state.grid.reverse;
    ha_endpoint ha = {0};
    const bool via_ha = ha_valid_entity(target.identity, HA_SENSOR);
    if (via_ha) ha = state.home.endpoint;
    const uint32_t revision = grid.revision;
    unlock();
    int32_t watts;
    const unsigned phases = victron_grid_phases(target.identity);
    fronius_sample power_flow = {0};
    shelly_result result;
    if (via_ha) {
        double reading;
        result = strcmp(ha.ip, target.ip) ? SHELLY_IDENTITY : ha_read_sensor(&ha, target.identity, HA_POWER_W, &reading);
        if (result == SHELLY_OK) {
            if (reading < -1000000 || reading > 1000000) result = SHELLY_PROTOCOL;
            else watts = (int32_t)ceil(reading);
        }
    } else if (!strcmp(target.identity, "fronius-solar-api")) {
        result = fronius_power_flow(target.ip, &power_flow);
        if (result == SHELLY_OK) {
            if (power_flow.grid_known) watts = power_flow.grid_watts;
            else result = SHELLY_PROTOCOL;
        }
    } else if (kind != NATIVE_GRID_NONE) {
        result = native_grid_power(target.ip, target.identity, &native, reverse, network_clock(), &watts);
    } else if (phases) {
        result = victron_grid_power(target.ip, phases, &watts);
    } else {
        result = grid_shelly_power(&target, &watts);
    }
    explicit_bzero(&ha, sizeof(ha));
    explicit_bzero(&target, sizeof(target));
    const uint32_t finished = network_uptime();
    lock();
    if (revision == grid.revision && epoch == network_epoch() && network_online()) {
        if (operating_grid()) accrue_solar_credit(finished);
        const bool previously_fresh = grid_fresh(finished);
        if (grid.epoch != epoch) grid.covered_at = 0;
        const bool importing = result == SHELLY_OK &&
            watts > 0;
        if (importing && (!grid.importing || !previously_fresh)) grid.import_since = finished;
        grid.importing = importing;
        grid.attempted = true;
        grid.epoch = epoch;
        grid.result = result;
        grid.fronius_known = power_flow.solar_known;
        grid.fronius_watts = power_flow.solar_watts;
        grid.fronius_at = finished;
        grid.poll_at = finished + (result == SHELLY_AUTH ? 30 : GRID_POLL);
        grid.consecutive = result == SHELLY_OK && previously_fresh;
        if (result == SHELLY_OK) {
            grid.previous_watts = grid.watts;
            grid.watts = watts;
            if (watts <= 0) grid.covered_at = finished;
            grid.at = finished;
            grid.sampled = true;
            log_grid_power(watts);
        }
    }
    unlock();
}

static void remember_solar_reading(const ecu_reading *reading, uint32_t finished, uint32_t epoch)
{
    const uint32_t elapsed = finished - solar.at;
    const ecu_reading *previous = &solar.reading;
    const bool consecutive = solar.sampled && solar.epoch == epoch && solar.result != ECU_IDENTITY &&
        !strcmp(previous->id, reading->id) && elapsed >= SOLAR_DAY_POLL && elapsed <= 2 * SOLAR_DAY_POLL;
    const bool advanced = consecutive && (reading->watts != previous->watts ||
        reading->today_centi_kwh > previous->today_centi_kwh ||
        reading->lifetime_deci_kwh > previous->lifetime_deci_kwh);
    if (!consecutive) solar.progress_seen = false;
    if (!solar.sampled || advanced || !consecutive) solar.progress_at = finished;
    if (advanced) solar.progress_seen = true;
    // The summary carries no inverter observation timestamp. Two positive readings
    // and evidence of recent progress qualify only the lower reported wattage.
    solar.sustained = consecutive && solar.progress_seen && finished - solar.progress_at <= SOLAR_FRESH &&
        previous->watts && previous->online && reading->watts && reading->online;
    solar.supported_watts = solar.sustained ? (previous->watts < reading->watts ? previous->watts : reading->watts) : 0;
    solar.higher_watts = solar.sustained ? (previous->watts > reading->watts ? previous->watts : reading->watts) : 0;
    solar.reading = *reading;
    solar.at = finished;
    solar.epoch = epoch;
    solar.sampled = true;
}

static void solar_tick(void)
{
    const uint32_t now = network_uptime(), epoch = network_epoch();
    lock();
    const uint32_t revision = solar.revision;
    const bool ready = state.solar.ip[0] && network_online();
    const uint32_t interval = solar_night(network_clock()) ? SOLAR_NIGHT_POLL : SOLAR_DAY_POLL;
    const bool due = solar.retry_at ? now >= solar.retry_at :
        !solar.cycled || now - solar.cycle_at >= interval;
    const solar_settings endpoint = state.solar;
    ha_endpoint bridge = {0};
    if (ready && due && !endpoint.port) bridge = state.home.endpoint;
    unlock();

    if (solar_query.fd >= 0 && (!ready || revision != query_revision || epoch != query_epoch)) {
        ecu_r_cancel(&solar_query);
    }
    if (!ready || (solar_query.fd < 0 && !due)) return;
    ecu_reading reading;
    ecu_result status = ECU_WAITING;
    if (!endpoint.port) {
        status = sdk_solar_read(&bridge, &reading);
        explicit_bzero(&bridge, sizeof(bridge));
    } else {
        if (solar_query.fd < 0) {
            query_revision = revision;
            query_epoch = epoch;
            status = ecu_r_start(&solar_query, endpoint.ip, endpoint.port);
        }
        if (status == ECU_WAITING) status = ecu_r_step(&solar_query, &reading);
    }
    if (status == ECU_WAITING) return;
    if (status == ECU_OK && endpoint.id[0] && strcmp(endpoint.id, reading.id)) status = ECU_IDENTITY;
    const uint32_t finished = network_uptime();
    lock();
    if (revision == solar.revision && epoch == network_epoch() && network_online()) {
        accrue_solar_credit(finished);
        if (status == ECU_OK) {
            remember_solar_reading(&reading, finished, epoch);
            if (reading.watts > state.follow.reserve && reading.online && solar_control_fresh(finished)) {
                solar.lit_at = finished;
                solar.lit_epoch = epoch;
            }
            ESP_LOGI("dispatch", "Solar sample: %lu W, source %s, %u/%u inverters",
                     (unsigned long)reading.watts, reading.firmware, reading.online, reading.inverters);
        }
        solar.result = status;
        if ((status == ECU_NETWORK || status == ECU_PROTOCOL) && !solar.retry_at) {
            solar.retry_at = finished + SOLAR_RETRY;
        } else {
            solar.cycle_at = finished;
            solar.cycled = true;
            solar.retry_at = 0;
        }
    }
    unlock();
}

static bool mining_tick(void)
{
    enum { NONE, NODE, IDENTITY, DATUM, DESTINATION } work = NONE;
    unsigned slot = 0;
    mining_settings settings;
    char miner_ip[16] = {0};
    const uint32_t now = network_uptime(), epoch = network_epoch();
    lock();
    if (!network_online()) {
        unlock();
        return false;
    }
    if (path.epoch != epoch) {
        memset(&path, 0, sizeof(path));
        path.epoch = epoch;
    }
    if (state.mining.node_ip[0] && now >= path.next_node) {
        work = NODE;
    } else if (state.mining.node_ip[0] && path.node.status == PATH_READY && now >= path.next_identity) {
        work = IDENTITY;
    } else if (state.mining.datum_ip[0] && now >= path.next_datum) {
        work = DATUM;
    } else if (state.mining.miner_host[0]) {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (state.settings.device[i].miner_ip[0] && now >= runtime[i].next_destination) {
                slot = i;
                work = DESTINATION;
                strcpy(miner_ip, state.settings.device[i].miner_ip);
                break;
            }
        }
    }
    if (work == NONE) {
        unlock();
        return false;
    }
    const uint32_t revision = generation;
    settings = state.mining;
    unlock();
    node_reading node = {0};
    datum_reading datum = {0};
    path_status identity = PATH_WAITING;
    miner_destination destination = DESTINATION_UNKNOWN;
    switch (work) {
    case NODE: node = xbt_node_status(&settings); break;
    case IDENTITY: identity = xbt_node_identity(&settings); break;
    case DATUM: datum = datum_gateway_status(&settings); break;
    case DESTINATION: destination = cgminer_destination(miner_ip, settings.miner_host, settings.miner_port); break;
    default: break;
    }
    explicit_bzero(&settings, sizeof(settings));
    const uint32_t finished = network_uptime();
    lock();
    if (revision == generation && epoch == network_epoch() && network_online()) {
        switch (work) {
        case NODE:
            if (node.status != PATH_READY || path.node.status != PATH_READY) {
                path.identity = PATH_WAITING;
                path.next_identity = 0;
            }
            path.node = node;
            path.node_sampled = true;
            path.node_at = finished;
            path.next_node = finished + PATH_POLL_SECONDS;
            break;
        case IDENTITY:
            path.identity = identity;
            path.next_identity = finished + (identity == PATH_READY ? IDENTITY_SECONDS : PATH_POLL_SECONDS);
            break;
        case DATUM:
            path.datum = datum;
            path.datum_sampled = true;
            path.datum_at = finished;
            path.next_datum = finished + PATH_POLL_SECONDS;
            break;
        case DESTINATION:
            if (destination != DESTINATION_UNKNOWN && destination != runtime[slot].destination) {
                record(slot, destination == DESTINATION_MATCH ? EVENT_DESTINATION_MATCH : EVENT_DESTINATION_CHANGED);
            }
            runtime[slot].destination = destination;
            runtime[slot].destination_at = finished;
            runtime[slot].next_destination = finished + DESTINATION_POLL_SECONDS;
            break;
        default: break;
        }
    }
    unlock();
    return true;
}

static const char *power_mode_block(unsigned slot, const dispatch_inputs *in, dispatch_decision decision, bool preview)
{
    if (testing_outlet(slot)) return "Wait for the first-run power test to finish.";
    if (!state.enabled && !preview) return "Enable outlet control to change presets.";
    if (storage_error) return "Resolve the controller storage error first.";
    if (!mapping_allows_control(slot)) return "Confirm the outlet mapping first.";
    if (in->paused || atomic_load(&pause_requested)) return "Resume the installation to change presets.";
    if (!in->clock_valid) return "Wait for the controller clock to synchronize.";
    if (dispatch_operation_limit(in) == DISPATCH_WINDOW_CLOSED)
        return "Wait for allowed mining hours.";
    if (!in->outlet[slot].fresh || !in->outlet[slot].on) return "Wait for a fresh powered-on outlet reading.";
    if ((state.hold_mask & (1u << slot)) || runtime[slot].pending != DISPATCH_KEEP)
        return "Resolve the pending or unconfirmed outlet switch first.";
    if (in->recovery_hold_mask & (1u << slot)) return "Wait for the recovery hold to end.";
    if (in->manual_hold_mask & (1u << slot)) return "Clear the miner hold to change presets.";
    if (dispatch_quiet_active(in)) return "Wait for Quiet Hours to end.";
    if (decision.action != DISPATCH_KEEP) return "Wait for the operating plan's outlet change to finish.";
    if (in->now - runtime[slot].since < DISPATCH_MIN_ON) return "startup";
    return NULL;
}

static bool can_change_power_mode(unsigned slot, uint32_t now)
{
    const time_t utc = network_clock();
    const dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    return !power_mode_block(slot, &in, decisions[slot], false);
}

static bool restart_ready(unsigned slot, uint32_t now)
{
    const device_runtime *device = &runtime[slot];
    return device->view.power_mode.restart_supported && device->power_checked &&
        now - device->power_checked <= MODE_FRESH_SECONDS && device->power_probe_epoch == network_epoch() &&
        device->view.miner.elapsed_known && now - device->view.miner_time <= OBSERVATION_TTL &&
        !power_job.pending && !state.evaluation.active && !testing_outlet(slot) && admit_on() &&
        can_change_power_mode(slot, now);
}

esp_err_t installation_miner_restart(unsigned slot, uint16_t identity, const char *password)
{
    if (slot >= DISPATCH_LIMIT || !password[0] || strlen(password) >= GOLDSHELL_PASSWORD_BYTES)
        return ESP_ERR_INVALID_ARG;
    lock();
    const uint32_t now = network_uptime();
    if (!identity || installation_equipment_identity(&state.settings.device[slot]) != identity ||
        integration_busy(restart_job.command) || !restart_ready(slot, now)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    restart_job.expected = runtime[slot].view.power_mode;
    restart_job.command = COMMAND_QUEUED;
    restart_job.result = GOLDSHELL_MODE_UNCHECKED;
    restart_job.slot = slot;
    restart_job.revision = generation;
    restart_job.epoch = network_epoch();
    restart_job.deadline = now + 20;
    strcpy(restart_job.password, password);
    unlock();
    return ESP_OK;
}

static bool restart_allowed(void)
{
    return restart_job.epoch == network_epoch() && admit_on();
}

static bool restart_tick(void)
{
    lock();
    if (!integration_busy(restart_job.command)) {
        unlock();
        return false;
    }
    const uint32_t now = network_uptime();
    const unsigned slot = restart_job.slot;
    const device_view *view = &runtime[slot].view;
    const bool same_connection = restart_job.revision == generation &&
        restart_job.epoch == network_epoch() && network_online();
    if (restart_job.command == COMMAND_QUEUED) {
        if (!same_connection || now >= restart_job.deadline || !restart_ready(slot, now)) {
            restart_job.command = COMMAND_CANCELED;
            explicit_bzero(restart_job.password, sizeof(restart_job.password));
            unlock();
            return false;
        }
        restart_job.before = view->miner.elapsed_seconds;
        restart_job.sent = now;
        restart_job.result = goldshell_restart(state.settings.device[slot].miner_ip, restart_job.password,
                                               &restart_job.expected, restart_allowed);
        explicit_bzero(restart_job.password, sizeof(restart_job.password));
        if (restart_job.result == GOLDSHELL_MODE_DEFERRED) {
            restart_job.command = COMMAND_CANCELED;
        } else if (restart_job.result == GOLDSHELL_MODE_ACKNOWLEDGED ||
                   restart_job.result == GOLDSHELL_MODE_UNCONFIRMED) {
            restart_job.command = COMMAND_VERIFYING;
            restart_job.deadline = now + 120;
            record(slot, EVENT_MINER_RESTART);
        } else {
            restart_job.command = COMMAND_UNCONFIRMED;
        }
        unlock();
        return true;
    }
    if (!same_connection || now >= restart_job.deadline || !fresh(&runtime[slot], now) || !view->outlet.on) {
        restart_job.command = COMMAND_UNCONFIRMED;
    } else if (view->miner_time > restart_job.sent && view->miner.elapsed_known &&
               view->miner.elapsed_seconds < restart_job.before &&
               view->miner.elapsed_seconds <= view->miner_time - restart_job.sent + 5) {
        restart_job.command = COMMAND_CONFIRMED;
        record(slot, EVENT_MINER_RESTART_CONFIRMED);
    }
    unlock();
    return false;
}

static bool preset_authorized(unsigned slot)
{
    return (state.preset_authorized & (1u << slot)) && preset_credentials_available(slot);
}

static uint8_t preset_choice(unsigned slot)
{
    return state.preset_choice[slot];
}

static bool preset_observed(unsigned slot, uint32_t now)
{
    const device_runtime *device = &runtime[slot];
    return state.presets[slot].identity.alternate && hashing_preset(&device->view.power_mode) &&
        device->power_checked && now - device->power_checked <= MODE_FRESH_SECONDS &&
        device->power_probe_epoch == network_epoch() && now - device->view.miner_time <= OBSERVATION_TTL &&
        device->view.miner.activity_known && device->view.miner.rate_window_seconds && device->view.miner.mhs > 0;
}

static goldshell_mode learned_winner(unsigned slot)
{
    const miner_preset_evidence *history = &state.presets[slot];
    const dispatch_preset_comparison result = dispatch_preset_compare(&history->mode[PRESET_DEFAULT], &history->mode[PRESET_REDUCED]);
    return result == DISPATCH_PRESET_DEFAULT ? GOLDSHELL_MODE_DEFAULT :
        result == DISPATCH_PRESET_REDUCED ? history->identity.alternate : GOLDSHELL_MODE_UNKNOWN;
}

static void prepare_power_job(unsigned slot, power_source source, goldshell_mode desired, const char *password)
{
    power_job.pending = true;
    power_job.canceled = false;
    power_job.slot = slot;
    power_job.revision = generation;
    power_job.epoch = network_epoch();
    power_job.expected = runtime[slot].view.power_mode;
    power_job.desired = desired;
    power_job.source = source;
    power_job.verify_until = 0;
    power_job.next_verify = 0;
    strcpy(power_job.password, password);
    if (desired != GOLDSHELL_MODE_UNKNOWN) reset_preset_sample(&runtime[slot]);
}

static void preset_binding(unsigned slot, char binding[PRESET_BINDING_BYTES])
{
    const preset_identity *identity = &state.presets[slot].identity;
    const association *device = &state.settings.device[slot];
    snprintf(binding, PRESET_BINDING_BYTES, "%u/%s/%s/%s/%s/%s/%s/%u/%u/%u/%u", slot, device->shelly.identity,
        device->shelly.ip, device->miner_ip, identity->model, identity->firmware, identity->hardware,
        identity->boards, identity->alternate, identity->default_clock, identity->reduced_clock);
}

esp_err_t installation_power_mode(unsigned slot, goldshell_mode expected, goldshell_mode desired, const char *password)
{
    if (slot >= DISPATCH_LIMIT || strlen(password) >= sizeof(power_job.password)) return ESP_ERR_INVALID_ARG;
    lock();
    const uint32_t now = network_uptime();
    const device_runtime *device = &runtime[slot];
    const bool ready = !power_job.pending && !integration_busy(restart_job.command) && state.settings.device[slot].miner_ip[0] &&
        fresh(device, now) && device->view.outlet.on;
    const bool managed = (state.evaluation.active && state.evaluation.slot == slot) ||
        (preset_authorized(slot) && (preset_choice(slot) == PRESET_SOLAR ||
         (preset_choice(slot) == PRESET_EFFICIENCY && state.pinned_preset[slot])));
    if (!ready || (desired != GOLDSHELL_MODE_UNKNOWN &&
        (managed || !password[0] || expected == desired || device->view.power_mode.status != GOLDSHELL_MODE_READY ||
        (desired != GOLDSHELL_MODE_DEFAULT && desired != device->view.power_mode.alternate &&
         !(desired == GOLDSHELL_MODE_IDLE && device->view.power_mode.idle_supported)) ||
        device->view.power_mode.mode != expected || !device->power_checked || now - device->power_checked > MODE_FRESH_SECONDS ||
        !can_change_power_mode(slot, now)))) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    prepare_power_job(slot, desired == GOLDSHELL_MODE_UNKNOWN ? POWER_INSPECT : POWER_MANUAL, desired, password);
    unlock();
    return ESP_OK;
}

esp_err_t installation_preset_authorize(unsigned slot, const char *password)
{
    if (slot >= DISPATCH_LIMIT || !password[0] || strlen(password) >= GOLDSHELL_PASSWORD_BYTES) return ESP_ERR_INVALID_ARG;
    lock();
    const uint32_t now = network_uptime();
    if (power_job.pending || integration_busy(restart_job.command) || !mapping_allows_control(slot) || !fresh(&runtime[slot], now) ||
        !runtime[slot].view.outlet.on || !preset_observed(slot, now)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    prepare_power_job(slot, POWER_AUTHORIZE, GOLDSHELL_MODE_UNKNOWN, password);
    unlock();
    return ESP_OK;
}

esp_err_t installation_preset_revoke(unsigned slot)
{
    if (slot >= DISPATCH_LIMIT) return ESP_ERR_INVALID_ARG;
    lock();
    if (!state.settings.device[slot].shelly.ip[0]) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (power_job.pending && power_job.slot == slot && power_job.source == POWER_AUTHORIZE) power_job.canceled = true;
    const esp_err_t erased = preset_credentials_forget(slot);
    forget_preset_authorization(slot);
    const esp_err_t saved = save();
    if (saved == ESP_OK && erased == ESP_OK) record(slot, EVENT_PRESET_REVOKED);
    unlock();
    return saved == ESP_OK ? erased : saved;
}

esp_err_t installation_preset_choice(unsigned slot, uint16_t identity, preset_strategy choice)
{
    if (slot >= DISPATCH_LIMIT || choice > PRESET_EFFICIENCY || !identity) return ESP_ERR_INVALID_ARG;
    lock();
    const goldshell_mode winner = choice == PRESET_EFFICIENCY ? learned_winner(slot) : GOLDSHELL_MODE_UNKNOWN;
    if (identity != installation_equipment_identity(&state.settings.device[slot]) ||
        (state.evaluation.active && state.evaluation.slot == slot) ||
        (choice != PRESET_MANUAL && !preset_authorized(slot)) || (choice == PRESET_EFFICIENCY && !winner)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t previous = state.preset_choice[slot], prior_pin = state.pinned_preset[slot];
    state.preset_choice[slot] = choice;
    state.pinned_preset[slot] = winner;
    const esp_err_t result = save();
    if (result == ESP_OK) {
        if (power_job.pending && power_job.slot == slot && previous != choice) power_job.canceled = true;
        adaptive_preset_attempts &= ~(1u << slot);
        record(slot, EVENT_PRESET_STRATEGY);
    } else {
        state.preset_choice[slot] = previous;
        state.pinned_preset[slot] = prior_pin;
        dirty = true;
    }
    unlock();
    return result;
}

esp_err_t installation_preset_evaluate(unsigned slot, bool start)
{
    if (slot >= DISPATCH_LIMIT) return ESP_ERR_INVALID_ARG;
    lock();
    const uint32_t now = network_uptime();
    const bool eligible = start ? preset_choice(slot) == PRESET_MANUAL && !state.evaluation.active &&
        preset_authorized(slot) && preset_observed(slot, now) && fresh(&runtime[slot], now) &&
        can_change_power_mode(slot, now) && dispatch_preset_compare(&state.presets[slot].mode[PRESET_DEFAULT],
            &state.presets[slot].mode[PRESET_REDUCED]) == DISPATCH_PRESET_LEARNING :
        state.evaluation.active && state.evaluation.slot == slot;
    if (!eligible) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const preset_evaluation previous = state.evaluation;
    if (start) {
        state.evaluation.slot = slot;
        state.evaluation.restore = runtime[slot].view.power_mode.mode;
        state.evaluation.active = true;
        state.evaluation.restoring = state.evaluation.canceled = false;
    } else {
        state.evaluation.restoring = state.evaluation.canceled = true;
        if (!preset_authorized(slot)) state.evaluation.active = false;
    }
    const esp_err_t result = save();
    if (result == ESP_OK) {
        if (start) record(slot, EVENT_PRESET_EVAL_STARTED);
        else if (!state.evaluation.active) record(slot, EVENT_PRESET_EVAL_CANCELED);
    } else {
        state.evaluation = previous;
        dirty = true;
    }
    unlock();
    return result;
}

static uint32_t measured_preset_watts(unsigned slot, goldshell_mode mode)
{
    const dispatch_preset_totals *evidence = &state.presets[slot].mode[
        mode == GOLDSHELL_MODE_DEFAULT ? PRESET_DEFAULT : PRESET_REDUCED].totals;
    return evidence->seconds >= PRESET_LOAD_SECONDS ? (uint32_t)ceilf(evidence->peak_watts) : 0;
}

static bool bridge_preset_efficiency(unsigned slot)
{
    const dispatch_preset_evidence *mode = state.presets[slot].mode;
    if (mode[PRESET_DEFAULT].totals.seconds < PRESET_LOAD_SECONDS ||
        mode[PRESET_REDUCED].totals.seconds < PRESET_LOAD_SECONDS) return false;
    const double normal = dispatch_preset_efficiency(&mode[PRESET_DEFAULT].totals);
    const double reduced = dispatch_preset_efficiency(&mode[PRESET_REDUCED].totals);
    return reduced > 0 && reduced <= normal * 0.95;
}

static dispatch_solar_preset_decision solar_preset_choice(const dispatch_inputs *in,
    const dispatch_decision decisions[DISPATCH_LIMIT], bool preview, bool recheck)
{
    const dispatch_solar_preset_decision keep = {.slot = DISPATCH_LIMIT};
    if ((power_job.pending && !recheck) ||
        (!preview && !state.enabled) || in->now < next_energy_change) return keep;

    dispatch_solar_preset_slot slots[DISPATCH_LIMIT] = {0};
    dispatch_inputs available = *in;
    if (recheck && power_job.source == POWER_SOLAR_RESTORE) {
        const device_runtime *device = &runtime[power_job.slot];
        const uint32_t draw = (uint32_t)ceil(device->view.outlet.watts);
        if (device->preset_reserved_watts > draw) {
            const uint32_t own = device->preset_reserved_watts - draw;
            if (own <= available.solar.committed_watts) available.solar.committed_watts -= own;
        }
    }
    const dispatch_reason permission = dispatch_paid_permission(in);
    const bool bridging = permission == DISPATCH_DAYLIGHT_BRIDGE;
    const bool paid = in->solar.configured && dispatch_paid_covering(permission);
    if (!in->solar.bridge.enabled && !bridging && dispatch_solar_deficit(&in->solar)) return keep;
    if (recheck && (power_job.source == POWER_BRIDGE_LOWER) != bridging) return keep;
    if (bridging && ((!preview && !bridge_active) || (!recheck && in->now < bridge_preset_after))) return keep;
    const uint32_t observed_watts = paid ? solar.higher_watts : in->solar.production;
    const bool confirmed_deficit = in->grid_source ? grid.consecutive && grid.watts > 0 && grid.previous_watts > 0 :
        (uint64_t)observed_watts < (uint64_t)in->solar.running_watts + in->solar.reserve;
    const bool shortfall = confirmed_deficit &&
        !in->solar.shed_wait_seconds && !in->solar.shed_needs_reading;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        if (recheck && i != power_job.slot) continue;
        if (preset_choice(i) != PRESET_SOLAR) continue;
        if (bridging && !recheck && (bridge_preset_attempts & (1u << i))) continue;
        const device_runtime *device = &runtime[i];
        dispatch_decision applicable = decisions[i];
        if (applicable.action == DISPATCH_OFF && applicable.reason == DISPATCH_SOLAR_STOP)
            applicable.action = DISPATCH_KEEP;
        if (!preset_authorized(i) || !preset_observed(i, in->now) || !device->view.outlet.power_known ||
            in->now < device->preset_next_attempt || power_mode_block(i, in, applicable, preview)) continue;
        const goldshell_mode_reading *mode = &device->view.power_mode;
        const bool reduced = mode->mode != GOLDSHELL_MODE_DEFAULT;
        if (!bridging && !recheck && !reduced &&
            (adaptive_preset_attempts & (1u << i))) continue;
        if (reduced && in->now - device->preset_mode_since < PRESET_SOLAR_DWELL) continue;
        slots[i] = (dispatch_solar_preset_slot){.ready = true, .reduced = reduced,
            .draw = (uint32_t)ceil(device->view.outlet.watts), .default_watts = measured_preset_watts(i, GOLDSHELL_MODE_DEFAULT),
            .reduced_watts = measured_preset_watts(i, mode->alternate),
            .bridge_efficient = bridging && bridge_preset_efficiency(i)};
    }
    return dispatch_solar_choose_preset(&available, decisions, slots, shortfall);
}

static bool preset_power_fits(unsigned slot, goldshell_mode target, const dispatch_inputs *in)
{
    const device_runtime *device = &runtime[slot];
    if (!device->view.outlet.power_known || in->now < next_energy_change) return false;
    const uint32_t draw = (uint32_t)ceil(device->view.outlet.watts);
    uint32_t watts = measured_preset_watts(slot, target);
    if (!watts) {
        watts = in->solar.start_watts[slot];
        if (!watts) return false;
    }
    if (watts <= draw) return true;
    const uint32_t extra = watts - draw;
    if (in->solar.configured && in->solar.fresh && in->solar.load_known &&
        in->solar.sustained && dispatch_solar_headroom(&in->solar) >=
        (int64_t)extra + (in->grid_source ? in->surplus.start_reserve : 50)) return true;
    return dispatch_paid_permission(in) == DISPATCH_WINDOW_OPEN && dispatch_paid_room(in, extra);
}

static goldshell_mode evaluation_target(unsigned slot)
{
    if (state.evaluation.restoring) return state.evaluation.restore;
    const miner_preset_evidence *history = &state.presets[slot];
    const bool normal = dispatch_preset_window_count(&history->mode[PRESET_DEFAULT]) < DISPATCH_PRESET_WINDOWS;
    const bool reduced = dispatch_preset_window_count(&history->mode[PRESET_REDUCED]) < DISPATCH_PRESET_WINDOWS;
    if (normal && reduced) return runtime[slot].view.power_mode.mode;
    if (normal) return GOLDSHELL_MODE_DEFAULT;
    return reduced ? history->identity.alternate : state.evaluation.restore;
}

static void consider_preset_job(uint32_t now)
{
    if (integration_busy(restart_job.command)) return;
    if (!state.enabled || power_job.pending) return;
    const time_t utc = network_clock();
    const dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    if (in.solar.fresh && in.solar.load_known && in.solar.production >= (uint64_t)in.solar.running_watts + in.solar.reserve) {
        adaptive_preset_attempts = 0;
    }
    const dispatch_solar_preset_decision solar_choice = solar_preset_choice(&in, decisions, false, false);
    unsigned slot = solar_choice.slot;
    goldshell_mode target = GOLDSHELL_MODE_UNKNOWN;
    power_source source = POWER_INSPECT;
    if (solar_choice.action != DISPATCH_PRESET_KEEP) {
        target = solar_choice.action == DISPATCH_PRESET_RESTORE ? GOLDSHELL_MODE_DEFAULT : runtime[slot].view.power_mode.alternate;
        source = solar_choice.action == DISPATCH_PRESET_BRIDGE_LOWER ? POWER_BRIDGE_LOWER :
            solar_choice.action == DISPATCH_PRESET_LOWER ? POWER_SOLAR_LOWER : POWER_SOLAR_RESTORE;
    } else if (state.evaluation.active) {
        slot = state.evaluation.slot;
        if (!preset_observed(slot, now)) return;
        if (!state.evaluation.restoring &&
            dispatch_preset_window_count(&state.presets[slot].mode[PRESET_DEFAULT]) >= DISPATCH_PRESET_WINDOWS &&
            dispatch_preset_window_count(&state.presets[slot].mode[PRESET_REDUCED]) >= DISPATCH_PRESET_WINDOWS) {
            state.evaluation.restoring = true;
            dirty = true;
        }
        target = evaluation_target(slot);
        if (runtime[slot].view.power_mode.mode == target && state.evaluation.restoring) {
            state.evaluation.active = false;
            dirty = true;
            record(slot, state.evaluation.canceled ? EVENT_PRESET_EVAL_CANCELED : EVENT_PRESET_EVAL_FINISHED);
            return;
        }
        source = POWER_EVALUATE;
    } else {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            if (preset_choice(i) != PRESET_EFFICIENCY || !preset_authorized(i) || !preset_observed(i, now) ||
                !state.pinned_preset[i] ||
                state.pinned_preset[i] == runtime[i].view.power_mode.mode || now < runtime[i].preset_next_attempt ||
                power_mode_block(i, &in, decisions[i], false) || !preset_power_fits(i, state.pinned_preset[i], &in)) continue;
            slot = i;
            target = state.pinned_preset[i];
            source = POWER_EFFICIENCY;
            break;
        }
    }
    if (!target || slot == DISPATCH_LIMIT || runtime[slot].view.power_mode.mode == target || !preset_authorized(slot)) return;
    if (source == POWER_EVALUATE && (now < runtime[slot].preset_next_attempt ||
        power_mode_block(slot, &in, decisions[slot], false) || !preset_power_fits(slot, target, &in))) return;

    char password[GOLDSHELL_PASSWORD_BYTES] = {0};
    char binding[PRESET_BINDING_BYTES];
    preset_binding(slot, binding);
    if (!preset_credentials_read(slot, binding, password)) {
        forget_preset_authorization(slot);
        record(slot, EVENT_PRESET_AUTH_FAILED);
        return;
    }
    prepare_power_job(slot, source, target, password);
    explicit_bzero(password, sizeof(password));
    if (source == POWER_SOLAR_LOWER) {
        adaptive_preset_attempts |= 1u << slot;
        solar_preset_shed_until = now + MODE_VERIFY_SECONDS;
    } else if (source == POWER_BRIDGE_LOWER) {
        bridge_preset_attempts |= 1u << slot;
        bridge_preset_after = now + MODE_VERIFY_SECONDS + MODE_SETTLE_SECONDS;
    } else if (source == POWER_SOLAR_RESTORE) {
        device_runtime *device = &runtime[slot];
        device->preset_reserved_watts = (uint32_t)ceil(device->view.outlet.watts) + solar_choice.reserve_watts;
        device->preset_reserved_until = now + MODE_VERIFY_SECONDS;
    }
}

static bool managed_power_allowed(void)
{
    const unsigned slot = power_job.slot;
    if (power_job.canceled) return false;
    if (power_job.source == POWER_AUTHORIZE) return mapping_allows_control(slot);
    const uint32_t now = network_uptime();
    if (power_job.source == POWER_MANUAL) return can_change_power_mode(slot, now);
    if (!state.enabled || !preset_authorized(slot)) return false;
    const time_t utc = network_clock();
    const dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    if (power_job.source == POWER_SOLAR_LOWER || power_job.source == POWER_SOLAR_RESTORE ||
        power_job.source == POWER_BRIDGE_LOWER) {
        const dispatch_solar_preset_decision decision = solar_preset_choice(&in, decisions, false, true);
        if (decision.slot != slot) return false;
        if (power_job.source == POWER_BRIDGE_LOWER) return decision.action == DISPATCH_PRESET_BRIDGE_LOWER;
        return power_job.source == POWER_SOLAR_LOWER ?
            decision.action == DISPATCH_PRESET_LOWER && now < solar_preset_shed_until : decision.action == DISPATCH_PRESET_RESTORE;
    }
    if (power_mode_block(slot, &in, decisions[slot], false) || !preset_power_fits(slot, power_job.desired, &in)) return false;
    if (power_job.source == POWER_EVALUATE) return state.evaluation.active && state.evaluation.slot == slot &&
        evaluation_target(slot) == power_job.desired;
    return power_job.source == POWER_EFFICIENCY && preset_choice(slot) == PRESET_EFFICIENCY &&
        state.pinned_preset[slot] == power_job.desired;
}

static bool power_write_allowed(void)
{
    lock();
    const bool allowed = power_job.revision == generation && power_job.epoch == network_epoch() && network_online() &&
        managed_power_allowed();
    unlock();
    return allowed;
}

static bool preset_identity_matches(const preset_identity *identity, const goldshell_mode_reading *reading)
{
    return identity->alternate == reading->alternate && identity->boards == reading->board_count &&
        identity->default_clock == reading->default_clock && identity->reduced_clock == reading->alternate_clock &&
        !strcmp(identity->model, reading->model) && !strcmp(identity->firmware, reading->firmware) &&
        !strcmp(identity->hardware, reading->hardware);
}

static void confirm_preset_sample(unsigned slot, const goldshell_mode_reading *reading, uint32_t now, uint32_t epoch)
{
    device_runtime *device = &runtime[slot];
    miner_preset_evidence *history = &state.presets[slot];
    const preset_identity *known = &history->identity;
    if (known->alternate && ((reading->model && strcmp(known->model, reading->model)) ||
        (reading->firmware[0] && strcmp(known->firmware, reading->firmware)) ||
        (reading->hardware[0] && strcmp(known->hardware, reading->hardware)) ||
        (reading->status == GOLDSHELL_MODE_UNSUPPORTED && !reading->model && reading->advertised_model[0]))) {
        forget_preset_authorization(slot);
        clear_preset_evidence(slot);
        reset_preset_sample(device);
    }
    if (!hashing_preset(reading) || !mapping_allows_control(slot)) {
        reset_preset_sample(device);
        return;
    }
    if (!preset_identity_matches(&history->identity, reading)) {
        if (history->identity.alternate) forget_preset_authorization(slot);
        clear_preset_evidence(slot);
        history->identity = (preset_identity){.alternate = reading->alternate, .boards = reading->board_count,
            .default_clock = reading->default_clock, .reduced_clock = reading->alternate_clock};
        snprintf(history->identity.model, sizeof(history->identity.model), "%s", reading->model);
        strcpy(history->identity.firmware, reading->firmware);
        strcpy(history->identity.hardware, reading->hardware);
        reset_preset_sample(device);
        dirty = true;
        return;
    }
    if (device->view.power_mode.mode != reading->mode || device->power_probe_epoch != epoch ||
        !device->power_checked || now - device->power_checked > PRESET_CONFIRM_SECONDS ||
        now - device->preset_sample.at > OBSERVATION_TTL) {
        reset_preset_sample(device);
        return;
    }
    dispatch_preset_totals *period = &device->preset_sample.pending;
    if (!period->seconds) return;
    dispatch_preset_evidence *measured = &history->mode[reading->mode == GOLDSHELL_MODE_DEFAULT ? PRESET_DEFAULT : PRESET_REDUCED];
    dispatch_preset_merge(&measured->totals, period);
    dispatch_preset_merge(&device->preset_sample.window, period);
    if (device->preset_sample.window.seconds >= DISPATCH_PRESET_WINDOW_SECONDS) {
        dispatch_preset_record_window(measured, &device->preset_sample.window);
        device->preset_sample.window = (dispatch_preset_totals){0};
    }
    const uint32_t day = device->preset_sample.day;
    preset_daily_evidence *daily = &state.preset_days[day % DISPATCH_EVIDENCE_DAYS];
    if (day >= daily->day) {
        if (daily->day != day) *daily = (preset_daily_evidence){.day = day};
        dispatch_preset_merge(&daily->device[slot], period);
    }
    *period = (dispatch_preset_totals){0};
    dirty = true;
}

static event_kind preset_event(goldshell_mode mode)
{
    static const event_kind events[] = {
        [GOLDSHELL_MODE_DEFAULT] = EVENT_DEFAULT_MODE, [GOLDSHELL_MODE_LOW] = EVENT_LOW_MODE,
        [GOLDSHELL_MODE_BALANCE] = EVENT_BALANCE_MODE, [GOLDSHELL_MODE_IDLE] = EVENT_IDLE_MODE,
    };
    return mode > GOLDSHELL_MODE_UNKNOWN && mode <= GOLDSHELL_MODE_IDLE ? events[mode] : EVENT_POWER_MODE_UNCONFIRMED;
}

static void remember_power_mode(unsigned slot, goldshell_mode_reading reading, uint32_t now, uint32_t epoch)
{
    device_runtime *device = &runtime[slot];
    confirm_preset_sample(slot, &reading, now, epoch);
    if (reading.status == GOLDSHELL_MODE_READY && (reading.mode != device->view.power_mode.mode ||
        !device->preset_mode_since || !device->power_checked || device->power_probe_epoch != epoch)) device->preset_mode_since = now;
    device->view.power_mode = reading;
    device->power_checked = now;
    device->power_probe_epoch = epoch;
    uint32_t delay = MODE_FRESH_SECONDS;
    if (reading.status == GOLDSHELL_MODE_HASHRATE || reading.status == GOLDSHELL_MODE_CHANGED ||
        reading.status == GOLDSHELL_MODE_DEFERRED ||
        (reading.status == GOLDSHELL_MODE_UNCONFIRMED && reading.default_clock)) delay = 30;
    else if (reading.status == GOLDSHELL_MODE_OFFLINE) delay = 60;
    else if (hashing_preset(&reading)) delay = PRESET_PROBE_SECONDS;
    device->next_power_probe = now + delay;
}

static bool work_power_mode(void)
{
    lock();
    const uint32_t started = network_uptime();
    if (!power_job.pending || (power_job.verify_until && started < power_job.next_verify)) {
        unlock();
        return false;
    }
    const unsigned slot = power_job.slot;
    const uint32_t epoch = network_epoch();
    const uint32_t verify_until = power_job.verify_until;
    const goldshell_mode_reading expected = power_job.expected;
    const goldshell_mode desired = power_job.desired;
    const power_source source = power_job.source;
    char ip[16], password[GOLDSHELL_PASSWORD_BYTES];
    memcpy(ip, state.settings.device[slot].miner_ip, sizeof(ip));
    memcpy(password, power_job.password, sizeof(password));
    explicit_bzero(power_job.password, sizeof(power_job.password));
    const bool current = power_job.revision == generation && (verify_until || power_job.epoch == epoch) &&
        network_online() && fresh(&runtime[slot], started) && runtime[slot].view.outlet.on;
    unlock();

    goldshell_mode_reading result = {.status = GOLDSHELL_MODE_DEFERRED};
    if (verify_until) {
        // A retry only reads the miner. The write and its password have already been consumed.
        if (current && started <= verify_until) result = goldshell_mode_inspect(ip);
        const bool changed = (result.model && strcmp(result.model, expected.model)) ||
            (result.firmware[0] && strcmp(result.firmware, expected.firmware)) ||
            (result.hardware[0] && strcmp(result.hardware, expected.hardware)) ||
            (result.alternate && result.alternate != expected.alternate) ||
            (result.status == GOLDSHELL_MODE_READY && result.board_count && expected.board_count &&
             result.board_count != expected.board_count) ||
            (result.default_clock && result.default_clock != expected.default_clock) ||
            (result.alternate_clock && result.alternate_clock != expected.alternate_clock);
        if (changed) result.status = GOLDSHELL_MODE_CHANGED;
        else if (result.status != GOLDSHELL_MODE_READY || result.saved != desired || result.mode != desired)
            result.status = started < verify_until ? GOLDSHELL_MODE_VERIFYING : GOLDSHELL_MODE_UNCONFIRMED;
    } else if (current) {
        if (source == POWER_AUTHORIZE) {
            result = goldshell_mode_authorize(ip, password, &expected, power_write_allowed);
        } else {
            result = desired == GOLDSHELL_MODE_UNKNOWN ? goldshell_mode_inspect(ip) :
                goldshell_mode_change(ip, password, &expected, desired, power_write_allowed);
        }
    }
    lock();
    power_job.pending = false;
    if (source == POWER_AUTHORIZE) {
        if (power_job.revision == generation && !power_job.canceled) {
            bool stored = false;
            if (epoch == network_epoch() && network_online() && result.status == GOLDSHELL_MODE_READY &&
                preset_identity_matches(&state.presets[slot].identity, &result)) {
                char binding[PRESET_BINDING_BYTES];
                preset_binding(slot, binding);
                if (preset_credentials_store(slot, binding, password) == ESP_OK) {
                    state.preset_authorized |= 1u << slot;
                    stored = !preset_credentials_persistent() || save() == ESP_OK;
                    if (!stored) forget_preset_authorization(slot);
                }
            }
            runtime[slot].view.preset_auth_failed = !stored;
            record(slot, stored ? EVENT_PRESET_AUTHORIZED : EVENT_PRESET_AUTH_FAILED);
        }
        explicit_bzero(password, sizeof(password));
        unlock();
        return true;
    }
    explicit_bzero(password, sizeof(password));
    if (power_job.revision == generation) {
        device_runtime *device = &runtime[slot];
        const uint32_t now = network_uptime();
        if (epoch != network_epoch() || !network_online()) {
            if (verify_until) result.status = now < verify_until ? GOLDSHELL_MODE_VERIFYING : GOLDSHELL_MODE_UNCONFIRMED;
            else if (result.status == GOLDSHELL_MODE_READY || result.status == GOLDSHELL_MODE_DEFERRED)
                result.status = desired == GOLDSHELL_MODE_UNKNOWN ? GOLDSHELL_MODE_OFFLINE : GOLDSHELL_MODE_UNCONFIRMED;
        }
        if (result.status == GOLDSHELL_MODE_VERIFYING) {
            if (!verify_until) {
                power_job.verify_until = now + MODE_VERIFY_SECONDS;
                device->power_settle_until = power_job.verify_until;
                device->work = (dispatch_work_monitor){0};
                device->zero_since = 0;
            }
            const uint32_t next = now + MODE_RECHECK_SECONDS;
            power_job.next_verify = next < power_job.verify_until ? next : power_job.verify_until;
            power_job.pending = true;
        }
        // Keep showing the previously detected identity if the miner is restarting.
        if (verify_until && !result.model && !result.advertised_model[0]) {
            result.model = expected.model;
            memcpy(result.firmware, expected.firmware, sizeof(result.firmware));
            memcpy(result.hardware, expected.hardware, sizeof(result.hardware));
            result.experimental = expected.experimental;
            result.alternate = expected.alternate;
            result.idle_supported = expected.idle_supported;
        }
        remember_power_mode(slot, result, now, epoch);
        if (desired != GOLDSHELL_MODE_UNKNOWN && result.status == GOLDSHELL_MODE_READY) {
            if (result.mode != GOLDSHELL_MODE_IDLE) {
                const dispatch_profile previous = state.profile[slot];
                state.profile[slot] = (dispatch_profile){.startup_seconds = previous.startup_seconds, .startups = previous.startups};
                if (save() != ESP_OK) dirty = true;
            }
            device->power_settle_until = now + MODE_SETTLE_SECONDS;
            next_energy_change = now + GRID_SETTLE;
            device->work = (dispatch_work_monitor){0};
            device->zero_since = 0;
            record(slot, preset_event(result.mode));
            if (source >= POWER_SOLAR_LOWER) {
                static const event_kind applied[] = {EVENT_PRESET_SOLAR_LOWER, EVENT_PRESET_SOLAR_RESTORE,
                    EVENT_PRESET_EVAL_APPLIED, EVENT_PRESET_EFFICIENT, EVENT_PRESET_BRIDGE_LOWER};
                record(slot, applied[source - POWER_SOLAR_LOWER]);
            }
        } else if (desired != GOLDSHELL_MODE_UNKNOWN && result.status == GOLDSHELL_MODE_UNCONFIRMED) {
            record(slot, EVENT_POWER_MODE_UNCONFIRMED);
        }
        if (source >= POWER_SOLAR_LOWER && result.status != GOLDSHELL_MODE_READY && result.status != GOLDSHELL_MODE_VERIFYING) {
            device->preset_next_attempt = now + PRESET_RETRY_SECONDS;
            device->preset_reserved_until = 0;
            if (source == POWER_SOLAR_LOWER) solar_preset_shed_until = 0;
            if (result.status == GOLDSHELL_MODE_AUTH) {
                forget_preset_authorization(slot);
                device->view.preset_auth_failed = true;
                record(slot, EVENT_PRESET_AUTH_FAILED);
            }
        }
    }
    unlock();
    return true;
}

// Inspect one due address after control work. Rotate among the assigned slots.
static bool detect_power_mode(void)
{
    static unsigned next_slot;
    if (!network_online()) return false;
    lock();
    if (power_job.pending) {
        unlock();
        return false;
    }
    const uint32_t now = network_uptime(), epoch = network_epoch(), revision = generation;
    unsigned slot = DISPATCH_LIMIT;
    for (unsigned offset = 0; offset < DISPATCH_LIMIT; ++offset) {
        const unsigned i = (next_slot + offset) % DISPATCH_LIMIT;
        const device_runtime *device = &runtime[i];
        if (state.settings.device[i].miner_ip[0] && fresh(device, now) && device->view.outlet.on &&
            (device->power_probe_epoch != epoch || now >= device->next_power_probe)) {
            slot = i;
            break;
        }
    }
    if (slot == DISPATCH_LIMIT) {
        unlock();
        return false;
    }
    next_slot = (slot + 1) % DISPATCH_LIMIT;
    char ip[16];
    memcpy(ip, state.settings.device[slot].miner_ip, sizeof(ip));
    unlock();

    const goldshell_mode_reading reading = goldshell_mode_inspect(ip);
    lock();
    if (revision == generation && epoch == network_epoch() && network_online()) {
        const goldshell_mode_reading *previous = &runtime[slot].view.power_mode;
        if (previous->status == GOLDSHELL_MODE_READY && reading.status == GOLDSHELL_MODE_READY &&
            previous->mode != reading.mode) record(slot, preset_event(reading.mode));
        remember_power_mode(slot, reading, network_uptime(), epoch);
    }
    unlock();
    return true;
}

static void update_work_exceptions(uint32_t now)
{
    if (!state.enabled) {
        for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
            runtime[i].work = (dispatch_work_monitor){0};
            runtime[i].recovery_exhausted = false;
        }
        return;
    }
    const time_t utc = network_clock();
    const dispatch_inputs in = inputs(now, utc);
    dispatch_decision decisions[DISPATCH_LIMIT];
    decide(&in, decisions, utc);
    uint32_t day = 0;
    local_time(utc, &day, NULL);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        device_runtime *device = &runtime[i];
        const slot_coordination *slot = &state.coordination[i];
        const dispatch_outlet *outlet = &in.outlet[i];
        const bool expected = !testing_outlet(i) && !storage_error && in.clock_valid && !in.paused && !miner_work_paused(i, now) &&
            !dispatch_quiet_active(&in) && state.settings.device[i].miner_ip[0] && mapping_allows_control(i) &&
            !((in.manual_hold_mask | in.recovery_hold_mask | state.hold_mask) & (1u << i)) &&
            outlet->fresh && outlet->on && now - outlet->since >= DISPATCH_MIN_ON && now >= device->power_settle_until &&
            decisions[i].action != DISPATCH_OFF;
        const cgminer_sample *miner = &device->view.miner;
        const bool reporting = device->view.miner_polled && now - device->view.miner_time <= OBSERVATION_TTL &&
            miner->responding && miner->activity_known && miner->rate_window_seconds && miner->mhs > 0;
        const dispatch_work evidence = !expected ? DISPATCH_WORK_IDLE :
            reporting ? DISPATCH_WORK_PRESENT : DISPATCH_WORK_MISSING;
        const dispatch_work_change change = dispatch_work_update(&device->work, evidence, now);
        const bool exhausted = slot->recovery && day && slot->recovery_day == day && device->work.active;
        if (exhausted && !device->recovery_exhausted) record(i, EVENT_RECOVERY_EXHAUSTED);
        else if (change == DISPATCH_WORK_LOST) record(i, EVENT_WORK_LOST);
        else if (change == DISPATCH_WORK_RESTORED) record(i, EVENT_WORK_RESTORED);
        device->recovery_exhausted = exhausted;
    }
}

static bool deliver_notification(uint32_t now, uint32_t boot)
{
    if (!network_online()) return false;
    lock();
    const notification_job job = notification_due(&notifications, now);
    if (!job.sequence) {
        unlock();
        return false;
    }
    notification_target target = state.notifications;
    unlock();
    const notification_result result = notification_post(&target, &job, boot);
    explicit_bzero(&target, sizeof(target));
    lock();
    notification_finish(&notifications, job.sequence, result, network_uptime());
    unlock();
    return true;
}

static uint32_t probe_clock(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t retry_delay(uint8_t *failures, bool healthy, uint32_t normal)
{
    if (healthy) {
        *failures = 0;
        return normal;
    }
    if (*failures < 3) ++*failures;
    return normal << *failures;
}

static bool poll_equipment(bool miner)
{
    lock();
    if (!network_online()) {
        unlock();
        return false;
    }
    const uint32_t now = probe_clock();
    unsigned slot = DISPATCH_LIMIT;
    uint32_t oldest = 0;
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        const association *device = &state.settings.device[i];
        const device_runtime *d = &runtime[i];
        const uint32_t due = miner ? d->next_miner : d->next_probe;
        if (!device->shelly.ip[0] || (miner && (!device->miner_ip[0] || !fresh(d, network_uptime()) ||
            (!d->view.outlet.on && d->view.miner_polled))) || (due && (int32_t)(now - due) < 0)) continue;
        if (slot == DISPATCH_LIMIT || (!due && oldest) ||
            (due && oldest && (int32_t)(due - oldest) < 0)) {
            slot = i;
            oldest = due;
        }
    }
    if (slot == DISPATCH_LIMIT) {
        unlock();
        return false;
    }
    association target = state.settings.device[slot];
    const uint32_t revision = generation, connection = network_epoch();
    ha_endpoint ha = {0};
    home_meter_mapping meter = {0};
    mqtt_endpoint *mqtt = NULL;
    const outlet_kind kind = outlet_identity(target.shelly.identity);
    if (!miner && kind == OUTLET_HA) {
        ha = state.home.endpoint;
        meter = state.home_meter[slot];
    }
    if (!miner && kind == OUTLET_MQTT) {
        mqtt = malloc(sizeof(*mqtt));
        if (mqtt) *mqtt = state.mqtt;
    }
    unlock();

    cgminer_sample stats = {0};
    shelly_sample outlet = {0};
    shelly_result result = SHELLY_NETWORK;
    if (miner) stats = cgminer_summary(target.miner_ip);
    else if (kind == OUTLET_HA && !strcmp(ha.ip, target.shelly.ip))
        result = ha_mapped_switch_status(&ha, target.shelly.identity, meter.power, meter.energy, &outlet);
    else if (kind != OUTLET_MQTT || mqtt)
        result = outlet_status(&target.shelly, &ha, mqtt, &outlet);
    if (mqtt) {
        explicit_bzero(mqtt, sizeof(*mqtt));
        free(mqtt);
    }
    explicit_bzero(&ha, sizeof(ha));
    explicit_bzero(&target, sizeof(target));

    lock();
    if (revision == generation && connection == network_epoch() && network_online()) {
        const uint32_t finished = network_uptime();
        device_runtime *d = &runtime[slot];
        if (miner) {
            const bool current = fresh(d, finished);
            observation(slot, current ? SHELLY_OK : SHELLY_NETWORK, &d->view.outlet, &stats, false, finished);
            const uint32_t interval = testing_outlet(slot) || d->startup_tracking ? 5000 : 15000;
            const uint32_t backoff = retry_delay(&d->miner_failures, stats.responding, 5000);
            d->next_miner = probe_clock() + (stats.responding ? interval : backoff);
        } else {
            observation(slot, result, &outlet, NULL, true, finished);
            const uint32_t interval = retry_delay(&d->outlet_failures, result == SHELLY_OK, POLL_SECONDS * 1000);
            d->next_probe = result == SHELLY_OK ? now + interval :
                probe_clock() + (result == SHELLY_AUTH ? 60000 : interval);
        }
    }
    unlock();
    return true;
}

static void worker(void *arg)
{
    uint32_t next_log = 0, next_work_check = 0, next_notification = 0, next_detection = 0, next_telemetry = 0;
    const uint32_t boot = esp_random();
    uint32_t poll_epoch = UINT32_MAX;
    unsigned background = 0;
    for (;;) {
        const uint32_t now = network_uptime();
        lock();
        const time_t utc = network_clock();
        reconcile_budget_date(utc);
        expire_pause(utc);
        if (dirty && now >= next_checkpoint) {
            save();
        }
        unlock();

        const uint32_t epoch = network_epoch();
        if (epoch != poll_epoch && network_online()) {
            poll_epoch = epoch;
            lock();
            unsigned paired = 0;
            for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) paired += state.settings.device[i].shelly.ip[0] != 0;
            unsigned order = 0;
            for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
                if (!state.settings.device[i].shelly.ip[0]) continue;
                runtime[i].next_probe = probe_clock() + order++ * (5000 / paired);
                runtime[i].next_miner = 0;
                runtime[i].outlet_failures = runtime[i].miner_failures = 0;
            }
            unlock();
        }
        grid_tick();
        lock();
        update_solar_shed_timer(network_uptime());
        update_daylight_bridge(network_uptime());
        power_test_tick(network_uptime());
        if (power_test.phase == POWER_TEST_STOPPING && !power_test.retry_off) power_test_tick(network_uptime());
        consider_preset_job(network_uptime());
        bool commanded = false;
        if (state.enabled && network_online() && !storage_error) {
            const uint32_t due = network_uptime();
            const time_t clock = network_clock();
            const dispatch_inputs in = inputs(due, clock);
            dispatch_decision decisions[DISPATCH_LIMIT];
            decide(&in, decisions, clock);
            for (unsigned i = 0; i < DISPATCH_LIMIT && !commanded; ++i)
                commanded = try_command(i, due, DISPATCH_OFF, &in, decisions[i]);
            for (unsigned i = 0; i < DISPATCH_LIMIT && !commanded; ++i)
                commanded = try_command(i, due, DISPATCH_ON, &in, decisions[i]);
        }
        const uint32_t checked_at = network_uptime();
        if (checked_at >= next_work_check) {
            update_work_exceptions(checked_at);
            next_work_check = checked_at + POLL_SECONDS;
        }
        unlock();
        lock();
        const uint32_t telemetry_at = network_uptime();
        if (telemetry_at >= next_telemetry) {
            telemetry_tick(telemetry_at);
            next_telemetry = telemetry_at + 10;
        }
        timeline_flush_events();
        unlock();
        if (!commanded && !poll_equipment(false) && !poll_equipment(true)) {
            switch (background++ % 8) {
                case 0: solar_tick(); break;
                case 1: gx_tick(); break;
                case 2: home_tick(); break;
                case 3: mqtt_probe_tick(); break;
                case 4: if (!restart_tick()) work_power_mode(); break;
                case 5: mining_tick(); break;
                case 6:
                    if (network_uptime() >= next_detection && detect_power_mode())
                        next_detection = network_uptime() + 15;
                    else discovery_tick();
                    break;
                case 7:
                    if (network_uptime() >= next_notification) {
                        const bool attempted = deliver_notification(network_uptime(), boot);
                        next_notification = network_uptime() + (attempted ? POLL_SECONDS : 1);
                    }
                    break;
            }
        }
        lock();
        worker_stack = uxTaskGetStackHighWaterMark(NULL);
        unlock();
        if (now >= next_log) {
            const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            ESP_LOGI("dispatch", "resources uptime=%lu wifi=%d free=%u minimum=%u largest=%u worker_stack=%lu",
                     (unsigned long)now, network_online(),
                     (unsigned)heap_caps_get_free_size(caps),
                     (unsigned)heap_caps_get_minimum_free_size(caps),
                     (unsigned)heap_caps_get_largest_free_block(caps),
                     (unsigned long)worker_stack);
            next_log = now + 30;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void migrate_legacy(void)
{
    legacy_installation *old = calloc(1, sizeof(*old));
    if (!old) return;
    size_t size = sizeof(*old);
    if (nvs_get_blob(nvs, "state", old, &size) != ESP_OK || !old->operating.current_format ||
        !((old->version == LEGACY_VERSION && size == sizeof(*old)) ||
          (old->version == HOME_METER_VERSION && size == offsetof(legacy_installation, profile_revision)) ||
          (old->version == PREVIOUS_VERSION && size == offsetof(legacy_installation, home)))) goto done;

    memcpy(&state.pause_until, &old->pause_until, sizeof(old->pause_until));
    memcpy(&state.pause_unclocked, &old->pause_unclocked, sizeof(old->pause_unclocked));
    memcpy(&state.hold_mask, &old->hold_mask, sizeof(old->hold_mask));
    memcpy(&state.hold_on, &old->hold_on, sizeof(old->hold_on));
    memcpy(&state.enabled, &old->enabled, sizeof(old->enabled));
    memcpy(&state.timezone, &old->timezone, sizeof(old->timezone));
    memcpy(&state.quiet, &old->quiet, sizeof(old->quiet));
    memcpy(&state.tariff, &old->tariff, sizeof(old->tariff));
    memcpy(&state.highest_rate_day, &old->highest_rate_day, sizeof(old->highest_rate_day));
    memcpy(&state.profile, &old->profile, sizeof(old->profile));
    memcpy(&state.days, &old->days, sizeof(old->days));
    memcpy(&state.coordination, &old->coordination, sizeof(old->coordination));
    memcpy(&state.mining, &old->mining, sizeof(old->mining));
    memcpy(&state.budget_date_pending, &old->budget_date_pending, sizeof(old->budget_date_pending));
    memcpy(&state.solar, &old->solar, sizeof(old->solar));
    memcpy(&state.grid, &old->grid, sizeof(old->grid));
    memcpy(&state.first_run, &old->first_run, sizeof(old->first_run));
    memcpy(&state.pause_indefinite, &old->pause_indefinite, sizeof(old->pause_indefinite));
    memcpy(&state.solar_days, &old->solar_days, sizeof(old->solar_days));
    memcpy(&state.plan_ready, &old->plan_ready, sizeof(old->plan_ready));
    memcpy(&state.efficiency, &old->efficiency, sizeof(old->efficiency));
    memcpy(&state.equipment_name, &old->equipment_name, sizeof(old->equipment_name));
    memcpy(&state.weekend, &old->weekend, sizeof(old->weekend));
    memcpy(&state.notifications, &old->notifications, sizeof(old->notifications));
    memcpy(&state.presets, &old->presets, sizeof(old->presets));
    memcpy(&state.reserved_preset, &old->reserved_preset, sizeof(old->reserved_preset));
    memcpy(&state.preset_authorized, &old->preset_authorized, sizeof(old->preset_authorized));
    memcpy(&state.pinned_preset, &old->pinned_preset, sizeof(old->pinned_preset));
    memcpy(&state.evaluation, &old->evaluation, sizeof(old->evaluation));
    memcpy(&state.daylight, &old->daylight, sizeof(old->daylight));
    memcpy(&state.bridge_started, &old->bridge_started, sizeof(old->bridge_started));
    memcpy(&state.power_test, &old->power_test, sizeof(old->power_test));
    memcpy(&state.test_armed, &old->test_armed, sizeof(old->test_armed));
    memcpy(&state.reserved_policy, &old->reserved_policy, sizeof(old->reserved_policy));
    memcpy(&state.operating, &old->operating, sizeof(old->operating));
    memcpy(&state.preset_choice, &old->preset_choice, sizeof(old->preset_choice));
    memcpy(&state.home, &old->home, sizeof(old->home));
    memcpy(&state.mqtt, &old->mqtt, sizeof(old->mqtt));
    memcpy(&state.home_meter, &old->home_meter, sizeof(old->home_meter));
    memcpy(&state.profile_revision, &old->profile_revision, sizeof(old->profile_revision));
    memcpy(&state.grid_profile_revision, &old->grid_profile_revision, sizeof(old->grid_profile_revision));

    state.attempt_counts = old->attempt_counts;
    state.settings.policy = old->settings.policy;
    state.settings.utc_offset_minutes = old->settings.utc_offset_minutes;
    memcpy(state.settings.device, old->settings.device, sizeof(old->settings.device));
    memcpy(state.budget.anchor, old->budget.anchor, sizeof(old->budget.anchor));
    state.budget.used_nano_cents = old->budget.used_nano_cents;
    state.budget.day = old->budget.day;
    state.budget.gap_mask = old->budget.gap_mask;
    state.follow.reserve = old->follow.reserve;
    state.follow.shed_margin = old->follow.shed_margin;
    memcpy(state.follow.start_watts, old->follow.start_watts, sizeof(old->follow.start_watts));
    state.follow.restart_seconds = old->follow.restart_seconds;
    state.follow.shed_delay_seconds = old->follow.shed_delay_seconds;
    for (unsigned i = 0; i < DISPATCH_EVIDENCE_DAYS; ++i) {
        state.preset_days[i].day = old->preset_days[i].day;
        memcpy(state.preset_days[i].device, old->preset_days[i].device, sizeof(old->preset_days[i].device));
    }
done:
    explicit_bzero(old, sizeof(*old));
    free(old);
}

void installation_start(void)
{
    timeline_start();
    ESP_ERROR_CHECK(nvs_open("dispatch_op", NVS_READWRITE, &nvs));
    preset_credentials_start(nvs);
    state = (installation_store){
        .version = INSTALLATION_VERSION,
        .settings.policy.rate_millicents_per_kwh = 12000,
        .quiet = default_quiet,
        .solar = default_solar,
        .grid = default_grid,
        .follow = default_follow,
        .operating.current_format = true,
    };
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) state.coordination[i].priority = 1;
    if (!operations_load(&state, sizeof(state))) {
        migrate_legacy();
        if (save() != ESP_OK) {
            dirty = true;
            ESP_LOGE("dispatch", "Could not initialize the operations journal");
        }
    }
    const uint64_t revision = operating_profile_revision();
    const uint64_t grid_revision = grid_profile_revision(state.grid.target.identity);
    if (state.profile_revision != revision || state.grid_profile_revision != grid_revision) {
        energy_preview();
        state.profile_revision = revision;
        if (state.grid_profile_revision != grid_revision) {
            state.grid.direction_verified = false;
            state.grid_profile_revision = grid_revision;
        }
        state.bridge_started = 0;
        dirty = true;
    }
    state.timezone[sizeof(state.timezone) - 1] = 0;
    active_timezone = timezone_find(state.timezone);
    for (unsigned i = 0; i < DISPATCH_LIMIT; ++i) {
        device_runtime *d = &runtime[i];
        d->tries = (state.attempt_counts >> (2 * i)) & 3u;
        if (d->tries) {
            d->pending = state.hold_on & (1u << i) ? DISPATCH_ON : DISPATCH_OFF;
            d->next_command = RETRY_SECONDS;
        }
    }
    if (state.test_armed > 0 && state.test_armed <= DISPATCH_LIMIT) {
        power_test.slot = state.test_armed - 1;
        stop_power_test(POWER_TEST_FAILED, "The controller restarted. The outlet was returned to off without saving a measurement.");
    }
    next_checkpoint = CHECKPOINT_SECONDS;
    mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(mutex ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(worker, "dispatch_worker", WORKER_STACK, NULL, 4, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_INSTALLATION_H
#define DISPATCH_INSTALLATION_H

#include "native_grid.h"
#include "cgminer.h"
#include "dispatch_policy.h"
#include "dispatch_daylight.h"
#include "dispatch_evidence.h"
#include "dispatch_timezone.h"
#include "esp_err.h"
#include "outlet.h"
#include "victron_gx.h"
#include "mining_path.h"
#include "ecu_r.h"
#include "notifications.h"
#include "goldshell_mode.h"

typedef enum {
    COMMAND_IDLE, COMMAND_QUEUED, COMMAND_VERIFYING, COMMAND_CONFIRMED, COMMAND_UNCONFIRMED, COMMAND_CANCELED,
} integration_command;

typedef struct {
    char power[HA_ENTITY_BYTES], energy[HA_ENTITY_BYTES];
} home_meter_mapping;

typedef struct {
    ha_endpoint endpoint;
    char battery[HA_ENTITY_BYTES], temperature[HA_ENTITY_BYTES], actuator[HA_ENTITY_BYTES];
} home_settings;

typedef struct {
    char ip[16], battery[HA_ENTITY_BYTES], temperature[HA_ENTITY_BYTES], actuator[HA_ENTITY_BYTES];
    uint16_t port;
    ha_control_state control;
    double battery_percent, temperature_c;
    bool battery_known, temperature_known, actuator_known;
    integration_command command;
    shelly_result result;
} home_view;

typedef enum { OVERRIDE_NONE, OVERRIDE_HOLD, OVERRIDE_PREFER } override_mode;
typedef struct {
    uint64_t override_until, recovery_until;
    uint32_t recovery_day;
    uint8_t priority, mode;
    bool recovery;
} slot_coordination;

enum { HISTORY_SIZE = 24, OBSERVATION_TTL = 30 };
enum { EQUIPMENT_NAME_BYTES = 49 };
enum { EVENT_NO_REASON = UINT8_MAX };
enum { RUN_MAPPING = 1, RUN_POWER = 2, RUN_WORK = 4 };

typedef struct {
    shelly_target shelly;
    char miner_ip[16];
} association;

typedef struct {
    dispatch_policy policy;
    int16_t utc_offset_minutes;
    association device[DISPATCH_LIMIT];
} installation_settings;

typedef struct {
    char ip[16], id[13];
    // A configured source with port zero uses the saved REST bridge.
    uint16_t port, night_start, night_end;
} solar_settings;

typedef struct {
    uint16_t reserve, shed_margin, start_watts[DISPATCH_LIMIT];
    uint8_t reserved;
    uint16_t restart_seconds, shed_delay_seconds;
} follow_settings;

enum { FOLLOW_RESTART_DEFAULT = 600, FOLLOW_SHED_DELAY_DEFAULT = 300, FOLLOW_TIMER_LIMIT_MINUTES = 120 };
enum { BRIDGE_DEFAULT_MINUTES = 20, BRIDGE_MIN_MINUTES = 5, BRIDGE_MAX_MINUTES = 60 };
typedef struct {
    int32_t latitude_e4, longitude_e4;
    uint16_t minutes;
    bool enabled;
} daylight_settings;

typedef struct {
    dispatch_operating_plan plan;
    daylight_settings daylight;
} operating_settings;

enum { OPERATING_SCENARIOS = 7 };
typedef struct {
    dispatch_decision decision[DISPATCH_LIMIT];
    dispatch_solar_preset_decision preset;
    uint8_t on_mask;
    bool applicable;
} operating_scenario;

typedef struct {
    shelly_target target;
    uint16_t reserve;
    native_grid_config native;
    bool reverse, direction_verified;
} grid_settings;

typedef struct {
    solar_settings solar;
    follow_settings follow;
    dispatch_generation generation;
    dispatch_reason paid_permission;
    dispatch_gap_forecast gap_budget, gap_budget_after;
    daylight_settings daylight;
    dispatch_daylight_kind daylight_kind;
    uint16_t sunrise_minute, sunset_minute;
    uint32_t solar_covered_watts;
    grid_settings grid;
    ecu_reading production;
    int32_t grid_watts;
    uint32_t solar_age, grid_age;
    ecu_result solar_result;
    shelly_result grid_result;
    bool solar_sampled, solar_fresh, solar_night, grid_sampled, grid_fresh, grid_attempted;
    bool grid_source;
    uint32_t fronius_solar_watts;
    bool fronius_solar_known;
    int32_t gx_target, gx_requested;
    bool gx_target_known;
    integration_command gx_command;
    shelly_result gx_result;
} energy_view;

typedef struct {
    uint64_t energy_mwh, savings_nano_cents;
} solar_daily_evidence;

enum { PRESET_DEFAULT, PRESET_REDUCED, PRESET_COUNT };
typedef enum { PRESET_MANUAL, PRESET_SOLAR, PRESET_EFFICIENCY } preset_strategy;
typedef enum {
    PRESET_NO_REQUEST, PRESET_LOWER_SOLAR, PRESET_RESTORE_SOLAR, PRESET_EVALUATE, PRESET_APPLY_EFFICIENCY,
    PRESET_LOWER_BRIDGE,
} preset_request;
typedef struct {
    uint32_t day;
    dispatch_preset_totals device[DISPATCH_LIMIT];
} preset_daily_evidence;

enum { POWER_TEST_SECONDS = 240 };
typedef enum {
    POWER_TEST_IDLE, POWER_TEST_QUEUED, POWER_TEST_STARTING, POWER_TEST_WARMING,
    POWER_TEST_MEASURING, POWER_TEST_STOPPING, POWER_TEST_COMPLETE, POWER_TEST_CANCELED, POWER_TEST_FAILED,
} power_test_phase;

typedef struct {
    power_test_phase phase;
    dispatch_power_test_result measured;
    uint16_t seconds, peak;
    unsigned stable_seconds;
    const char *detail, *blocked;
} power_test_view;

typedef struct {
    shelly_sample outlet;
    cgminer_sample miner;
    uint32_t outlet_time, miner_time;
    uint16_t learned_startup_watts;
    dispatch_profile profile;
    dispatch_efficiency efficiency;
    dispatch_preset_evidence presets[PRESET_COUNT];
    goldshell_mode measured_alternate;
    goldshell_mode preset_target;
    preset_request preset_request;
    bool preset_authorized, preset_evaluable, preset_evaluating, preset_restoring, preset_auth_failed;
    char name[EQUIPMENT_NAME_BYTES];
    goldshell_mode_reading power_mode;
    bool power_pending, power_current, restart_ready;
    integration_command restart_command;
    goldshell_mode_status restart_result;
    const char *power_block;
    power_test_view power_test;
    shelly_result outlet_result;
    dispatch_decision decision;
    bool observed, miner_polled, command_pending;
    miner_destination destination;
    slot_coordination coordination;
    uint32_t override_seconds, recovery_seconds;
    uint32_t running_seconds, solar_restart_seconds;
    uint8_t first_run;
    bool stalled, work_missing, recovery_exhausted;
    const char *recovery_status;
} device_view;

typedef enum {
    EVENT_OUTLET_ON, EVENT_OUTLET_OFF, EVENT_OUTLET_LOST, EVENT_MINER_LOST,
    EVENT_MINER_RESPONDING, EVENT_COMMAND_ON, EVENT_COMMAND_OFF, EVENT_COMMAND_ERROR,
    EVENT_CONFIRMED, EVENT_COMMAND_HOLD, EVENT_COUNTER_GAP, EVENT_CONFIGURED,
    EVENT_PREVIEW, EVENT_ENABLED, EVENT_PAUSED, EVENT_RESUMED, EVENT_STORAGE_ERROR,
    EVENT_DESTINATION_CHANGED, EVENT_DESTINATION_MATCH, EVENT_RECOVERY_ATTEMPT,
    EVENT_RECOVERY_ACK, EVENT_RECOVERY_ERROR, EVENT_COORDINATION, EVENT_PATH_CONFIGURED, EVENT_BUDGET_DATE,
    EVENT_METER_RECOVERED, EVENT_ACCOUNTING_CLEARED,
    EVENT_MAPPING_CONFIRMED, EVENT_FIRST_POWER, EVENT_FIRST_WORK,
    EVENT_RECOVERY_OFF_OBSERVED, EVENT_WORK_LOST, EVENT_WORK_RESTORED, EVENT_RECOVERY_EXHAUSTED,
    EVENT_DEFAULT_MODE, EVENT_LOW_MODE, EVENT_POWER_MODE_UNCONFIRMED, EVENT_BALANCE_MODE, EVENT_IDLE_MODE,
    EVENT_PRESET_AUTHORIZED, EVENT_PRESET_AUTH_FAILED, EVENT_PRESET_REVOKED, EVENT_PRESET_STRATEGY,
    EVENT_PRESET_EVAL_STARTED, EVENT_PRESET_EVAL_FINISHED, EVENT_PRESET_EVAL_CANCELED,
    EVENT_PRESET_SOLAR_LOWER, EVENT_PRESET_SOLAR_RESTORE, EVENT_PRESET_EVAL_APPLIED, EVENT_PRESET_EFFICIENT,
    EVENT_DAYLIGHT_BRIDGE_STARTED, EVENT_DAYLIGHT_BRIDGE_REARMED, EVENT_DAYLIGHT_BRIDGE_ENDED,
    EVENT_POWER_TEST_STARTED, EVENT_POWER_TEST_COMPLETE, EVENT_POWER_TEST_CANCELED, EVENT_POWER_TEST_FAILED,
    EVENT_PRESET_BRIDGE_LOWER, EVENT_MINER_RESTART, EVENT_MINER_RESTART_CONFIRMED,
} event_kind;

typedef enum {
    NEXT_NONE, NEXT_PAUSE_END, NEXT_WINDOW_OPEN, NEXT_WINDOW_CLOSE, NEXT_QUIET_START,
    NEXT_QUIET_END, NEXT_RATE, NEXT_BUDGET, NEXT_OVERRIDE, NEXT_RECOVERY, NEXT_DWELL,
    NEXT_STAGGER, NEXT_TIME_CHANGE,
    NEXT_SOLAR_RESTART, NEXT_SOLAR_SHED, NEXT_WEEKEND_PLAN, NEXT_WEEKDAY_PLAN,
    NEXT_SUNRISE, NEXT_SUNSET, NEXT_DAYLIGHT_BRIDGE_END,
    NEXT_PAID_OPEN, NEXT_PAID_CLOSE, NEXT_SOURCE_GRACE_END,
} operation_change;

typedef struct {
    uint32_t seconds, day, minute;
    operation_change kind;
    uint8_t slot;
} operation_next;

typedef struct {
    uint32_t uptime, local_day;
    uint16_t local_minute;
    uint8_t device, kind, action, reason, local_second;
    bool previous_equipment;
} history_event;

typedef struct {
    installation_settings settings;
    char timezone[TIMEZONE_NAME_BYTES];
    home_view home;
    home_meter_mapping home_meter[DISPATCH_LIMIT];
    char mqtt_ip[16], mqtt_tls_name[96];
    uint16_t mqtt_port;
    bool mqtt_probe_pending, mqtt_probe_attempted, mqtt_probe_on;
    shelly_result mqtt_probe_result;
    dispatch_quiet_hours quiet;
    dispatch_tariff tariff;
    dispatch_weekend_plan weekend;
    bool active_weekend;
    device_view device[DISPATCH_LIMIT];
    history_event history[HISTORY_SIZE];
    dispatch_daily_evidence days[DISPATCH_EVIDENCE_DAYS];
    solar_daily_evidence solar_days[DISPATCH_EVIDENCE_DAYS];
    preset_daily_evidence preset_days[DISPATCH_EVIDENCE_DAYS];
    dispatch_operating_plan operating;
    uint8_t preset_choice[DISPATCH_LIMIT];
    bool preset_credentials_persistent;
    unsigned history_count;
    uint32_t now, pause_seconds, day, local_day, worker_stack;
    uint64_t used_nano_cents;
    uint32_t active_rate;
    unsigned active_rate_window;
    mining_settings mining;
    node_reading node;
    datum_reading datum;
    energy_view energy;
    operation_next next;
    struct {
        char ip[16];
        uint16_t port;
        uint32_t sent, failed, dropped;
        unsigned pending;
        notification_state test;
    } notifications;
    uint32_t node_age, datum_age;
    bool node_sampled, datum_sampled, node_seen, datum_seen, rpc_configured;
    bool enabled, clock_valid, accounting_complete, paused, storage_error, quiet_active;
    bool rate_known, highest_rate_used, time_synchronized, meter_gap, pause_indefinite, plan_ready;
} installation_view;

void installation_start(void);
bool installation_boot_healthy(void);
#ifdef DISPATCH_PREVIEW_ONLY
void installation_checkpoint(void);
#endif
void installation_read(installation_view *view);
uint16_t installation_equipment_identity(const association *device);
esp_err_t installation_power_test(unsigned slot, uint16_t identity, bool start, const char **conflict);
esp_err_t installation_configure(unsigned index, const association *device, bool keep_password);
esp_err_t installation_set_plan(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                               const dispatch_tariff *tariff, const dispatch_weekend_plan *weekend,
                               const dispatch_timezone *timezone, const operating_settings *operating);
void installation_preview_plan(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                               const dispatch_tariff *tariff, const operating_settings *operating,
                               operating_scenario cases[OPERATING_SCENARIOS]);
esp_err_t installation_import(const dispatch_policy *policy, const dispatch_quiet_hours *quiet,
                              const dispatch_tariff *tariff, const dispatch_weekend_plan *weekend,
                              const dispatch_timezone *timezone,
                              int offset, const association devices[DISPATCH_LIMIT], const operating_settings *operating);
esp_err_t installation_mining(const mining_settings *settings);
esp_err_t installation_notifications(const notification_target *target);
esp_err_t installation_notification_test(void);
esp_err_t installation_miner_restart(unsigned slot, uint16_t identity, const char *password);
esp_err_t installation_power_mode(unsigned slot, goldshell_mode expected, goldshell_mode desired, const char *password);
esp_err_t installation_preset_authorize(unsigned slot, const char *password);
esp_err_t installation_preset_revoke(unsigned slot);
esp_err_t installation_preset_choice(unsigned slot, uint16_t identity, preset_strategy choice);
esp_err_t installation_preset_evaluate(unsigned slot, bool start);
esp_err_t installation_solar(const solar_settings *settings);
esp_err_t installation_follow(const follow_settings *settings);
esp_err_t installation_power_policy(const follow_settings *settings, uint16_t grid_reserve);
esp_err_t installation_startup_draw(unsigned slot, uint16_t identity, uint16_t watts);
bool installation_grid_identity(const char *identity);
esp_err_t installation_mqtt_probe(const char *identity);
esp_err_t installation_mqtt(const mqtt_endpoint *endpoint, bool keep_credentials);
esp_err_t installation_ha(const home_settings *settings, bool keep_token);
esp_err_t installation_ha_meter(unsigned slot, uint16_t identity, const home_meter_mapping *mapping);
esp_err_t installation_ha_action(const char *entity, double value, const char *option);
esp_err_t installation_gx_target(const char *ip, int32_t expected, int32_t watts);
esp_err_t installation_grid(const grid_settings *settings, bool keep_password, bool keep_reserve);
esp_err_t installation_coordination(unsigned slot, unsigned priority, bool recovery);
esp_err_t installation_rename(unsigned slot, const char *identity, const char *name);
esp_err_t installation_override(unsigned slot, override_mode mode, unsigned seconds);
esp_err_t installation_control(bool enable);
esp_err_t installation_accounting(bool clear);
esp_err_t installation_pause(unsigned seconds, bool indefinite);
esp_err_t installation_confirm_mapping(unsigned slot, const association *expected);
const char *installation_event(event_kind kind);
const char *installation_next_label(operation_change kind);
#endif

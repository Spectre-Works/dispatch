// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "shelly_http.h"
#include "integration_profile.h"
#include "local_http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *request, *auth, *body, *challenge;
    int code;
} exchange;

static exchange script[4];
static unsigned count, cursor;
static char response[JSON_RECORD_BYTES + 1];

char *local_http_body(void) { return response; }

const json_record *local_http_json(void)
{
    return json_parse_shared(response);
}

static void prepare(const exchange *items, unsigned length)
{
    assert(cursor == count && length <= sizeof(script) / sizeof(*script));
    memcpy(script, items, length * sizeof(*items));
    count = length;
    cursor = 0;
}

static int respond(const char *request, const char *auth, char *answer, size_t capacity,
                   char *challenge, size_t challenge_capacity)
{
    assert(cursor < count);
    const exchange *item = &script[cursor++];
    assert(!strcmp(request, item->request) && !strcmp(auth, item->auth));
    assert(strlen(item->body) < capacity);
    strcpy(answer, item->body);
    if (challenge) {
        assert(strlen(item->challenge) < challenge_capacity);
        strcpy(challenge, item->challenge);
    }
    return item->code;
}

int64_t esp_timer_get_time(void) { return 100; }

int shelly_get_until(const char *ip, const char *path, const char *credentials,
                     char *answer, size_t capacity, int64_t deadline)
{
    assert(!strcmp(ip, "192.168.1.45") && deadline > 100);
    return respond(path, credentials, answer, capacity, NULL, 0);
}

int shelly_http_until(const char *ip, const char *body, const char *authorization,
                      char *answer, size_t capacity, char *challenge, size_t challenge_capacity, int64_t deadline)
{
    assert(!strcmp(ip, "192.168.1.45") && deadline > 100);
    return respond(body, authorization, answer, capacity, challenge, challenge_capacity);
}

bool shelly_digest(const shelly_target *target, const char *challenge,
                   char authorization[SHELLY_AUTH_BYTES])
{
    if (strcmp(challenge, target->identity) || strcmp(target->password, "test-password")) return false;
    strcpy(authorization, "fixture-digest");
    return true;
}

static bool paused(void) { return false; }

static void gen1_outlet(void)
{
    const char identity[] = "{\"type\":\"SHSW-25\",\"mac\":\"001122AABBCC\",\"auth\":true,\"longid\":1}";
    const char *credential = "operator:synthetic:secret";
    const char outlet[] = "{\"relays\":[{\"ison\":true,\"has_timer\":false},"
        "{\"ison\":false,\"has_timer\":false}],\"meters\":[{\"power\":9,\"total\":99,\"is_valid\":true},"
        "{\"power\":0,\"total\":3624,\"is_valid\":true}]}";
    shelly_target relay = {.ip = "192.168.1.45"};
    prepare((exchange[]){{"/shelly", "", identity, NULL, 200}}, 1);
    assert(shelly_identify(&relay) && !strcmp(relay.identity, "shellyswitch25-001122aabbcc"));
    assert(shelly_valid_credentials(relay.identity, credential) && !shelly_valid_credentials(relay.identity, "bad"));
    assert(!shelly_valid_relay_channel(relay.identity, 2));

    shelly_sample sample;
    prepare((exchange[]){{"/shelly", "", identity, NULL, 200}}, 1);
    assert(shelly_status_channel(&relay, 1, &sample) == SHELLY_AUTH);
    strcpy(relay.password, credential);
    prepare((exchange[]){{"/shelly", "", identity, NULL, 200}, {"/status", credential, outlet, NULL, 200}}, 2);
    assert(shelly_status_channel(&relay, 1, &sample) == SHELLY_OK);
    assert(!sample.on && sample.power_known && sample.watts == 0);
    assert(sample.energy_known && sample.total_mwh == 60400);

    prepare((exchange[]){{"/shelly", "", identity, NULL, 200}, {"/status", credential, outlet, NULL, 200}}, 2);
    assert(shelly_set_channel(&relay, 1, true, paused) == SHELLY_DEFERRED);
    prepare((exchange[]){
        {"/shelly", "", identity, NULL, 200}, {"/status", credential, outlet, NULL, 200},
        {"/relay/1?turn=on&timer=240", credential, "", NULL, 0},
    }, 3);
    assert(shelly_timed_on_channel(&relay, 1, 240, NULL) == SHELLY_NETWORK);
    prepare((exchange[]){
        {"/shelly", "", identity, NULL, 200},
        {"/status", credential, "{\"relays\":[{}, {\"ison\":true,\"has_timer\":true,"
            "\"timer_duration\":240}],\"meters\":[{}, {\"is_valid\":false,\"power\":44}]}", NULL, 200},
    }, 2);
    assert(shelly_status_channel(&relay, 1, &sample) == SHELLY_OK);
    assert(sample.on && sample.timer_active && sample.flip_back_timer && sample.timer_seconds == 240);
    assert(!sample.power_known && !sample.energy_known);
    prepare((exchange[]){{"/shelly", "", identity, NULL, 200}}, 1);
    strcpy(relay.identity, "shellyswitch25-001122aabbcd");
    assert(shelly_set_channel(&relay, 1, false, NULL) == SHELLY_IDENTITY);
}

static void rpc_outlet(void)
{
    const char read[] = "{\"id\":1,\"method\":\"Switch.GetStatus\",\"params\":{\"id\":2}}";
    const char off[] = "{\"id\":1,\"method\":\"Switch.Set\",\"params\":{\"id\":2,\"on\":false}}";
    const char state[] = "{\"id\":1,\"src\":\"shellypro4pm-001122aabbcd\",\"result\":{\"id\":2,\"output\":true,"
        "\"apower\":52.2,\"aenergy\":{\"total\":18.45}}}";
    shelly_target relay = {.ip = "192.168.1.45", .identity = "shellypro4pm-001122aabbcd", .password = "test-password"};
    prepare((exchange[]){{read, "", state, "", 200}}, 1);
    shelly_sample sample;
    assert(shelly_status_channel(&relay, 2, &sample) == SHELLY_OK && sample.on && sample.total_mwh == 18450);
    prepare((exchange[]){{read, "", state, "", 200}}, 1);
    assert(shelly_set_channel(&relay, 2, false, paused) == SHELLY_DEFERRED);
    prepare((exchange[]){
        {read, "", "", relay.identity, 401},
        {off, "fixture-digest", "{\"id\":1,\"src\":\"shellypro4pm-001122aabbcd\",\"result\":{\"was_on\":true}}", "", 200},
    }, 2);
    assert(shelly_set_channel(&relay, 2, false, NULL) == SHELLY_OK);
    prepare((exchange[]){{read, "", "", "another-device", 401}}, 1);
    assert(shelly_set_channel(&relay, 2, false, NULL) == SHELLY_AUTH);
}

static void grid_meters(void)
{
    shelly_target meter = {.ip = "192.168.1.45", .identity = "shellyem3-aabbcc"};
    const char identity[] = "{\"type\":\"SHEM-3\",\"mac\":\"001122AABBCC\",\"auth\":false,\"longid\":0}";
    prepare((exchange[]){
        {"/shelly", "", identity, NULL, 200},
        {"/status", "", "{\"emeters\":[{\"is_valid\":true,\"power\":-1100.8},"
            "{\"is_valid\":true,\"power\":300},{\"is_valid\":true,\"power\":79}]}", NULL, 200},
    }, 2);
    int32_t watts;
    assert(shelly_grid_power_channel(&meter, 0, &watts) == SHELLY_OK && watts == -721);
    prepare((exchange[]){
        {"/shelly", "", identity, NULL, 200},
        {"/status", "", "{\"emeters\":[{\"is_valid\":true,\"power\":-1100.8},"
            "{\"is_valid\":false,\"power\":300},{\"is_valid\":true,\"power\":79}]}", NULL, 200},
    }, 2);
    assert(shelly_grid_power_channel(&meter, 0, &watts) == SHELLY_PROTOCOL && watts == -721);
    strcpy(meter.identity, "shellyproem-001122aabbcc");
    const char read[] = "{\"id\":1,\"method\":\"EM1.GetStatus\",\"params\":{\"id\":1}}";
    prepare((exchange[]){{read, "", "{\"id\":1,\"src\":\"shellyproem-001122aabbcc\","
        "\"result\":{\"id\":1,\"act_power\":81.2}}", "", 200}}, 1);
    assert(shelly_grid_power_channel(&meter, 1, &watts) == SHELLY_OK && watts == 82);
    prepare((exchange[]){{read, "", "{\"id\":1,\"src\":\"shellyproem-001122aabbcc\","
        "\"result\":{\"id\":1,\"act_power\":81.2,\"errors\":[\"power_meter_failure\"]}}", "", 200}}, 1);
    assert(shelly_grid_power_channel(&meter, 1, &watts) == SHELLY_PROTOCOL);
    assert(!shelly_valid_grid_identity(meter.identity, 2));
    strcpy(meter.identity, "shellypro3em-001122aabbcc");
    assert(!shelly_valid_relay_channel(meter.identity, 0));
    assert(shelly_valid_relay_channel(meter.identity, 100));
    assert(!shelly_valid_relay_channel(meter.identity, 200));
}

static void user_models(void)
{
    integration_profile profile = {.gen1 = *(const profile_shelly_gen1 *)integration_profile_get(PROFILE_SHELLY_GEN1)};
    profile.gen1.models[7] = (profile_shelly_model){"FIXTURE-1", "shellytest1-", 1};
    assert(integration_profile_activate(PROFILE_SHELLY_GEN1, &profile));
    const char gen1[] = "{\"type\":\"FIXTURE-1\",\"mac\":\"001122AABBCC\",\"auth\":false,\"longid\":1}";
    shelly_target target = {.ip = "192.168.1.45"};
    prepare((exchange[]){{"/shelly", "", gen1, NULL, 200}}, 1);
    assert(shelly_identify(&target) && !strcmp(target.identity, "shellytest1-001122aabbcc"));
    prepare((exchange[]){{"/shelly", "", gen1, NULL, 200},
        {"/status", "", "{\"relays\":[{\"ison\":true,\"has_timer\":false}]}", NULL, 200}}, 2);
    shelly_sample sample;
    assert(shelly_status_channel(&target, 0, &sample) == SHELLY_OK && sample.on);

    profile = (integration_profile){.rpc = *(const profile_shelly_rpc *)integration_profile_get(PROFILE_SHELLY_RPC)};
    profile.rpc.models[0] = (profile_rpc_model){"shellytest2-", 4, 8, PROFILE_RPC_EM1};
    strcpy(profile.rpc.single_grid, "gridW");
    assert(integration_profile_activate(PROFILE_SHELLY_RPC, &profile));
    strcpy(target.identity, "shellytest2-001122aabbcc");
    assert(shelly_valid_relay_channel(target.identity, 8) && !shelly_valid_relay_channel(target.identity, 9));
    const char read[] = "{\"id\":1,\"method\":\"EM1.GetStatus\",\"params\":{\"id\":0}}";
    prepare((exchange[]){{read, "", "{\"id\":1,\"src\":\"shellytest2-001122aabbcc\","
        "\"result\":{\"id\":0,\"gridW\":-140}}", "", 200}}, 1);
    int32_t watts;
    assert(shelly_grid_power_channel(&target, 0, &watts) == SHELLY_OK && watts == -140);
}

int main(void)
{
    gen1_outlet();
    rpc_outlet();
    grid_meters();
    user_models();
    assert(cursor == count);
    puts("Shelly adapter fixture checks passed");
}

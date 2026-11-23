// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "aes/esp_aes.h"
#include "cgminer.h"
#include "goldshell_mode.h"
#include "local_http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *setting =
    "{\"select\":%d,\"manual\":false,\"tempcontrol\":true,\"version\":\"v1.2\",\"algoname\":\"blake2b(SC)\","
    "\"powerplans\":[{\"algo\":\"blake2bsha3(HNS)\",\"mode\":[{\"level\":2},{\"level\":0}]},"
    "{\"algo\":\"blake2b(SC)\",\"mode\":[{\"level\":0,\"info\":\"800 MHz 10100 V 80 RPM 80 RPM\"},"
    "{\"level\":2,\"info\":\"650 MHz 9530 V 80 RPM 80 RPM\"},{\"level\":3}]}]}";
static char body[640];
static char fixture_token[484];
static unsigned writes, allow_checks, logins, restart_writes;
static int restart_response = 204;
static int selection, active_selection;
static unsigned default_clock = 800, low_clock = 650, boards = 2;
static double default_mhs, low_mhs;
static const char *expected_model = "HS Lite";
static const char *model = "Goldshell-HSLITE";
static const char *firmware = "2.2.5";
static const char *hardware = "fixture";
static const char *algorithms;
static unsigned web_mode = 2;
static bool idle_fixture, reports_hashing;

cgminer_board_sample cgminer_boards(const char *ip, unsigned expected_boards)
{
    assert(!strcmp(ip, "192.168.77.21") && (!expected_boards || expected_boards == boards));
    return (cgminer_board_sample){.count = boards, .clock = active_selection == 1 ? low_clock : default_clock,
        .mhs = active_selection == 1 ? low_mhs : default_mhs};
}

cgminer_sample cgminer_summary(const char *ip)
{
    assert(!strcmp(ip, "192.168.77.21") && idle_fixture);
    return (cgminer_sample){.activity_known = true, .rate_window_seconds = 20, .mhs = reports_hashing ? 100 : 0};
}
static bool old_firmware, deny_auth, login_offline, ignore_write, settle_later, block_before_write;

void esp_aes_init(esp_aes_context *ctx) { ctx->initialized = 1; }
void esp_aes_free(esp_aes_context *ctx) { ctx->initialized = 0; }

int esp_aes_setkey(esp_aes_context *ctx, const unsigned char *key, unsigned keybits)
{
    assert(ctx->initialized && keybits == 128 && key[0]);
    return 0;
}

int esp_aes_crypt_cbc(esp_aes_context *ctx, int mode, size_t length, unsigned char iv[16],
                     const unsigned char *input, unsigned char *output)
{
    const unsigned char zero[16] = {0};
    assert(ctx->initialized && mode == ESP_AES_ENCRYPT && length == 16 && !memcmp(iv, zero, 16));
    assert(!memcmp(input, "fixture", 7) && !memcmp(input + 7, zero, 9));
    memset(output, 0xab, length);
    return 0;
}

char *local_http_body(void) { return body; }

int local_http_response(const char *ip, unsigned port, const char *header, size_t header_bytes,
                        const char *payload, size_t payload_bytes, bool prefix)
{
    assert(!strcmp(ip, "192.168.77.21") && port == 80 && strlen(header) == header_bytes && !prefix);
    if (!strncmp(header, "GET /mcb/status ", 16)) {
        snprintf(body, sizeof(body), "{\"model\":\"%s\",\"firmware\":\"%s\",\"hardware\":\"%s\"}",
            model, old_firmware ? "2.2.1" : firmware, hardware);
        return 200;
    }
    if (!strncmp(header, "GET /mcb/algosetting ", 21)) {
        if (!algorithms) return 404;
        snprintf(body, sizeof(body), "%s", algorithms);
        return 200;
    }
    static const char devs[] = "GET /mcb/cgminer?cgminercmd=devs ";
    if (!strncmp(header, devs, sizeof(devs) - 1)) {
        assert(idle_fixture);
        snprintf(body, sizeof(body), "{\"status\":%u,\"data\":[]}", web_mode);
        return 200;
    }
    if (!strncmp(header, "GET /mcb/setting ", 17)) {
        snprintf(body, sizeof(body), setting, selection);
        return 200;
    }
    if (!strncmp(header, "GET /user/login?", 16)) {
        assert(strstr(header, "username=admin&password=abababababababababababababababab&cipher=true "));
        ++logins;
        if (login_offline) { strcpy(body, "{}"); return 503; }
        if (deny_auth) strcpy(body, "{}");
        else snprintf(body, sizeof(body), "{\"JWT Token\":\"%s\"}", fixture_token);
        return deny_auth ? 401 : 200;
    }
    const char *authorization = strstr(header, "Authorization: Bearer ");
    assert(authorization && !strncmp(authorization + 22, fixture_token, sizeof(fixture_token) - 1) &&
           !strncmp(authorization + 22 + sizeof(fixture_token) - 1, "\r\n", 2));
    if (!strncmp(header, "PUT /mcb/restart ", 17)) {
        assert(!payload && !payload_bytes && strstr(header, "\r\nContent-Length: 0\r\n\r\n"));
        ++restart_writes;
        body[0] = 0;
        return restart_response;
    }
    assert(!strncmp(header, "PUT /mcb/setting ", 17));
    assert(payload == body && strlen(payload) == payload_bytes);
    char expected[512];
    snprintf(expected, sizeof(expected), setting, 1 - active_selection);
    assert(!strcmp(payload, expected));
    if (!ignore_write) {
        selection = 1 - active_selection;
        if (!settle_later) {
            active_selection = selection;
            if (idle_fixture) web_mode = selection == 1 ? 6 : 2;
        }
    }
    ++writes;
    strcpy(body, "{}");
    return 200;
}

static bool allowed(void)
{
    return !block_before_write || ++allow_checks == 1;
}

static goldshell_mode_reading change_mode(const char *ip, goldshell_mode expected, goldshell_mode desired)
{
    goldshell_mode_reading previous = {.model = expected_model, .mode = expected, .alternate_clock = low_clock};
    strcpy(previous.firmware, firmware);
    return goldshell_mode_change(ip, "fixture", &previous, desired, allowed);
}

static void check_restart(const char *ip)
{
    model = "Goldshell-KDBox";
    firmware = "2.2.4";
    hardware = "fixture";
    const goldshell_mode_reading kd = goldshell_mode_inspect(ip);
    assert(kd.status == GOLDSHELL_MODE_UNSUPPORTED && kd.restart_supported);
    assert(goldshell_restart_inspect(ip).status == GOLDSHELL_MODE_READY);
    assert(goldshell_restart(ip, "fixture", &kd, allowed) == GOLDSHELL_MODE_ACKNOWLEDGED);
    assert(restart_writes == 1);

    model = "Goldshell-HSBox";
    firmware = "2.2.6";
    const goldshell_mode_reading hs = goldshell_restart_inspect(ip);
    assert(hs.status == GOLDSHELL_MODE_READY && hs.restart_supported);
    assert(goldshell_restart(ip, "fixture", &kd, allowed) == GOLDSHELL_MODE_CHANGED);
    hardware = "new-board";
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_CHANGED);
    hardware = "fixture";

    block_before_write = true;
    allow_checks = 0;
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_DEFERRED);
    assert(allow_checks == 2 && restart_writes == 1);
    block_before_write = false;
    deny_auth = true;
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_AUTH);
    assert(restart_writes == 1);
    deny_auth = false;
    restart_response = 0;
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_UNCONFIRMED);
    assert(restart_writes == 2);
    restart_response = 403;
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_AUTH);
    assert(restart_writes == 3);

    model = "Goldshell-HSLITE";
    firmware = "2.2.5";
    assert(goldshell_restart_inspect(ip).restart_supported);
    firmware = "2.2.4";
    assert(goldshell_restart_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);
    model = "Goldshell-HSBoxII";
    firmware = "2.2.6";
    assert(goldshell_restart_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);
    assert(goldshell_restart(ip, "fixture", &hs, allowed) == GOLDSHELL_MODE_UNSUPPORTED);
    assert(restart_writes == 3);
}

int main(void)
{
    const char *ip = "192.168.77.21";
    memset(fixture_token, 'A', sizeof(fixture_token) - 1);
    const goldshell_mode_reading baseline = goldshell_mode_inspect(ip);
    assert(baseline.mode == GOLDSHELL_MODE_DEFAULT);
    assert(goldshell_mode_authorize(ip, "fixture", &baseline, allowed).status == GOLDSHELL_MODE_READY);
    assert(!writes && logins == 1);
    goldshell_mode_reading other = baseline;
    ++other.alternate_clock;
    assert(goldshell_mode_authorize(ip, "fixture", &other, allowed).status == GOLDSHELL_MODE_CHANGED);
    assert(!writes && logins == 1);
    login_offline = true;
    assert(goldshell_mode_authorize(ip, "fixture", &baseline, allowed).status == GOLDSHELL_MODE_OFFLINE);
    assert(!writes && logins == 2);
    login_offline = false;
    active_selection = 1;
    const goldshell_mode_reading mismatch = goldshell_mode_inspect(ip);
    assert(mismatch.mode == GOLDSHELL_MODE_LOW && mismatch.saved == GOLDSHELL_MODE_DEFAULT);
    active_selection = 0;
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).mode ==
           GOLDSHELL_MODE_LOW);
    assert(writes == 1 && selection == 1);
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
           GOLDSHELL_MODE_CHANGED && writes == 1);
    deny_auth = true;
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).status ==
           GOLDSHELL_MODE_AUTH && writes == 1);
    deny_auth = false;
    block_before_write = true;
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).status ==
           GOLDSHELL_MODE_DEFERRED && writes == 1);
    block_before_write = false;
    ignore_write = true;
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).status ==
           GOLDSHELL_MODE_VERIFYING && writes == 2);
    ignore_write = false;
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).mode ==
           GOLDSHELL_MODE_DEFAULT && selection == 0 && writes == 3);
    old_firmware = true;
    const goldshell_mode_reading unsupported = goldshell_mode_inspect(ip);
    assert(unsupported.status == GOLDSHELL_MODE_UNSUPPORTED && !strcmp(unsupported.firmware, "2.2.1"));
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
           GOLDSHELL_MODE_UNSUPPORTED && writes == 3);
    old_firmware = false;
    char older_setting[480];
    strcpy(older_setting, setting);
    strstr(older_setting, "v1.2")[3] = '1';
    const char *latest_setting = setting;
    const goldshell_mode_reading previous = goldshell_mode_inspect(ip);
    setting = older_setting;
    firmware = "2.2.3";
    assert(goldshell_mode_inspect(ip).experimental && goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_DEFAULT);
    firmware = "2.2.4";
    const goldshell_mode_reading experimental = goldshell_mode_inspect(ip);
    assert(experimental.experimental && experimental.mode == GOLDSHELL_MODE_DEFAULT);
    assert(goldshell_mode_authorize(ip, "fixture", &experimental, allowed).status == GOLDSHELL_MODE_READY);
    assert(goldshell_mode_change(ip, "fixture", &previous, GOLDSHELL_MODE_LOW, allowed).status ==
        GOLDSHELL_MODE_CHANGED && writes == 3);
    setting = latest_setting;
    model = "Goldshell-SCBoxⅡ";
    boards = 1;
    firmware = "2.2.2";
    default_clock = 690; low_clock = 525;
    setting = "{\"select\":%d,\"manual\":false,\"tempcontrol\":true,\"version\":\"v1.3\","
        "\"powerplans\":[{\"level\":0,\"info\":\"690 MHz 42 V 50 RPM 41 RPM\",\"voltage\":42.00001},"
        "{\"level\":2,\"info\":\"525 MHz 61 V 40 RPM 59 RPM\",\"voltage\":61.00001},{\"level\":3}]}";
    goldshell_mode_reading sc = goldshell_mode_inspect(ip);
    assert(sc.mode == GOLDSHELL_MODE_DEFAULT && sc.experimental && !strcmp(sc.model, "SC BOX II"));
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
        GOLDSHELL_MODE_CHANGED && writes == 3);
    model = "Goldshell-SCBOX\\u2161";
    expected_model = "SC BOX II";
    assert(goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_DEFAULT);
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).mode ==
        GOLDSHELL_MODE_LOW && writes == 4);
    model = "Goldshell-HSBoxII";
    expected_model = "HS BOX II";
    firmware = "2.2.6";
    default_clock = 850; low_clock = 638;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"algoname\":\"blake2b(SC)\","
        "\"powerplans\":[{\"algo\":\"blake2b(SC)\",\"mode\":[{\"level\":0,\"info\":\"850 MHz 35 V\",\"extra\":\"PV 33\"},"
        "{\"level\":2,\"info\":\"638 MHz 54 V\",\"extra\":\"PV 52\"}]}]}";
    assert(goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_LOW);
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).mode ==
        GOLDSHELL_MODE_DEFAULT && writes == 5);
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"algoname\":\"blake2bsha3(HNS)\","
        "\"powerplans\":[{\"algo\":\"blake2b(SC)\",\"mode\":[{\"level\":0,\"info\":\"850 MHz 10000 V\"},"
        "{\"level\":2,\"info\":\"638 MHz 9300 V\"}]}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_ALGORITHM);
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
        GOLDSHELL_MODE_ALGORITHM && writes == 5);
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"algoname\":\"blake2b(SC)\","
        "\"powerplans\":[{\"algo\":\"blake2b(SC)\",\"mode\":[{\"level\":0,\"info\":\"850 MHz 10000 V\"},"
        "{\"level\":2,\"info\":\"650 MHz 9300 V\"}]}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);
    model = "Goldshell-SC6SE";
    assert(!strcmp(goldshell_mode_inspect(ip).model, "SC6 SE"));
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
        GOLDSHELL_MODE_UNSUPPORTED && writes == 5);

    model = "Goldshell-KD5Pro";
    expected_model = "KD5 Pro";
    firmware = "2.2.4";
    boards = 3;
    default_clock = 825;
    low_clock = 700;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"algoname\":\"blake2b(SC)\","
        "\"powerplans\":[{\"algo\":\"blake2s(KDA)\",\"mode\":[{\"level\":0}]},"
        "{\"algo\":\"blake2b(SC)\",\"mode\":[{\"level\":0,\"info\":\"825 MHz 12300 V 70 RPM 70 RPM\"},"
        "{\"level\":1,\"info\":\"700 MHz 11700 V 50 RPM 50 RPM\"},{\"level\":3}]}]}";
    const goldshell_mode_reading kd = goldshell_mode_inspect(ip);
    assert(kd.status == GOLDSHELL_MODE_READY && kd.mode == GOLDSHELL_MODE_DEFAULT &&
        kd.alternate == GOLDSHELL_MODE_BALANCE && kd.experimental);
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).status ==
        GOLDSHELL_MODE_UNSUPPORTED && writes == 5);
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_BALANCE).mode ==
        GOLDSHELL_MODE_BALANCE && writes == 6);
    assert(change_mode(ip, GOLDSHELL_MODE_BALANCE, GOLDSHELL_MODE_DEFAULT).mode ==
        GOLDSHELL_MODE_DEFAULT && writes == 7);
    strcpy(older_setting, setting);
    strstr(older_setting, "v1.2")[3] = '1';
    setting = older_setting;
    firmware = "2.2.3";
    assert(goldshell_mode_inspect(ip).alternate == GOLDSHELL_MODE_BALANCE);
    settle_later = true;
    const goldshell_mode_reading accepted = change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_BALANCE);
    assert(accepted.status == GOLDSHELL_MODE_VERIFYING && accepted.saved == GOLDSHELL_MODE_BALANCE && writes == 8);
    assert(goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_DEFAULT);
    active_selection = selection;
    const goldshell_mode_reading confirmed = goldshell_mode_inspect(ip);
    assert(confirmed.mode == GOLDSHELL_MODE_BALANCE && confirmed.saved == GOLDSHELL_MODE_BALANCE && writes == 8);
    model = "Goldshell-New-Model";
    const goldshell_mode_reading unknown = goldshell_mode_inspect(ip);
    assert(unknown.status == GOLDSHELL_MODE_UNSUPPORTED && !unknown.model &&
        !strcmp(unknown.advertised_model, "New-Model"));
    model = "Goldshell-Invalid\\\"Name";
    assert(!goldshell_mode_inspect(ip).advertised_model[0]);

    model = "Goldshell-SC5Pro";
    expected_model = "SC5 Pro";
    firmware = "2.2.0";
    boards = 4;
    default_clock = 840;
    low_clock = 670; // Synthetic value. The adapter reads the actual factory entry.
    selection = active_selection = 0;
    settle_later = false;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"powerplans\":["
        "{\"level\":0,\"info\":\"840 MHz 12090 V 70 RPM 70 RPM\"},"
        "{\"level\":2,\"info\":\"670 MHz 11000 V 50 RPM 50 RPM\"}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);
    hardware = "30.50.SA";
    const goldshell_mode_reading sc5 = goldshell_mode_inspect(ip);
    assert(sc5.status == GOLDSHELL_MODE_READY && sc5.mode == GOLDSHELL_MODE_DEFAULT &&
        sc5.alternate_clock == 670 && sc5.experimental);
    strcpy(older_setting, setting);
    strstr(older_setting, "670 MHz")[2] = '1';
    const char *sc5_setting = setting;
    setting = older_setting;
    assert(goldshell_mode_change(ip, "fixture", &sc5, GOLDSHELL_MODE_LOW, allowed).status ==
        GOLDSHELL_MODE_CHANGED && writes == 8);
    setting = sc5_setting;
    assert(change_mode(ip, GOLDSHELL_MODE_DEFAULT, GOLDSHELL_MODE_LOW).mode == GOLDSHELL_MODE_LOW && writes == 9);
    assert(change_mode(ip, GOLDSHELL_MODE_LOW, GOLDSHELL_MODE_DEFAULT).mode == GOLDSHELL_MODE_DEFAULT && writes == 10);

    model = "Goldshell-ARI31";
    firmware = "2.2.3";
    default_clock = 525;
    low_clock = 400; // Synthetic reduced clock for settings discovery.
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"powerplans\":["
        "{\"level\":0,\"info\":\"525 MHz 12600 V\"},{\"level\":1,\"info\":\"400 MHz 11000 V\"}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_ALGORITHM);
    const char *sc_algorithms = "{\"algo_select\":7,\"algos\":[{\"name\":\"blake2b(SC)\",\"id\":7},"
        "{\"name\":\"blake2bsha3(HNS)\",\"id\":0}]}";
    algorithms = sc_algorithms;
    const goldshell_mode_reading ari = goldshell_mode_inspect(ip);
    assert(ari.status == GOLDSHELL_MODE_READY && ari.mode == GOLDSHELL_MODE_DEFAULT &&
        ari.alternate == GOLDSHELL_MODE_BALANCE && ari.experimental);
    algorithms = "{\"algo_select\":0,\"algos\":[{\"name\":\"blake2b(SC)\",\"id\":7},"
        "{\"name\":\"blake2bsha3(HNS)\",\"id\":0}]}";
    assert(goldshell_mode_change(ip, "fixture", &ari, GOLDSHELL_MODE_BALANCE, allowed).status ==
        GOLDSHELL_MODE_ALGORITHM && writes == 10);
    algorithms = "{\"algo_select\":7,\"algos\":[{\"name\":\"blake2b(SC)\",\"id\":7},"
        "{\"name\":\"blake2b(SC)\",\"id\":7}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_ALGORITHM);
    algorithms = sc_algorithms;
    const goldshell_mode_reading balance = goldshell_mode_change(ip, "fixture", &ari, GOLDSHELL_MODE_BALANCE, allowed);
    assert(balance.status == GOLDSHELL_MODE_READY && balance.mode == GOLDSHELL_MODE_BALANCE && writes == 11);
    assert(goldshell_mode_change(ip, "fixture", &balance, GOLDSHELL_MODE_DEFAULT, allowed).mode == GOLDSHELL_MODE_DEFAULT && writes == 12);

    strcpy(older_setting, setting);
    strstr(older_setting, "\"level\":1")[8] = '2';
    setting = older_setting;
    const goldshell_mode_reading low = goldshell_mode_inspect(ip);
    assert(low.status == GOLDSHELL_MODE_READY && low.alternate == GOLDSHELL_MODE_LOW);
    assert(goldshell_mode_change(ip, "fixture", &ari, GOLDSHELL_MODE_BALANCE, allowed).status ==
        GOLDSHELL_MODE_CHANGED && writes == 12);
    assert(goldshell_mode_change(ip, "fixture", &low, GOLDSHELL_MODE_LOW, allowed).mode == GOLDSHELL_MODE_LOW && writes == 13);
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"powerplans\":["
        "{\"level\":0,\"info\":\"525 MHz 12600 V\"},{\"level\":1,\"info\":\"450 MHz 11250 V\"},"
        "{\"level\":2,\"info\":\"400 MHz 11000 V\"}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);

    idle_fixture = true;
    model = "Goldshell-SCBox";
    firmware = "2.2.5";
    default_clock = low_clock = 725;
    boards = 1;
    selection = active_selection = 0;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v2.1\",\"powerplans\":["
        "{\"level\":0,\"info\":\"725 MHz 0.41 V 70 RPM 70 RPM\"},"
        "{\"level\":3,\"info\":\"0 MHz 0 V 70 RPM 70 RPM\"}]}";
    const goldshell_mode_reading scbox = goldshell_mode_inspect(ip);
    assert(scbox.status == GOLDSHELL_MODE_READY && scbox.mode == GOLDSHELL_MODE_DEFAULT &&
        scbox.alternate == GOLDSHELL_MODE_UNKNOWN && scbox.idle_supported);
    assert(goldshell_mode_change(ip, "fixture", &scbox, GOLDSHELL_MODE_LOW, allowed).status ==
        GOLDSHELL_MODE_UNSUPPORTED && writes == 13);
    const goldshell_mode_reading idle = goldshell_mode_change(ip, "fixture", &scbox, GOLDSHELL_MODE_IDLE, allowed);
    assert(idle.status == GOLDSHELL_MODE_READY && idle.mode == GOLDSHELL_MODE_IDLE && idle.saved == GOLDSHELL_MODE_IDLE && writes == 14);
    reports_hashing = true;
    assert(goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_DEFAULT);
    assert(goldshell_mode_change(ip, "fixture", &idle, GOLDSHELL_MODE_DEFAULT, allowed).status == GOLDSHELL_MODE_CHANGED && writes == 14);
    reports_hashing = false;
    web_mode = 2;
    assert(goldshell_mode_inspect(ip).mode == GOLDSHELL_MODE_DEFAULT);
    web_mode = 6;
    assert(goldshell_mode_change(ip, "fixture", &idle, GOLDSHELL_MODE_DEFAULT, allowed).mode == GOLDSHELL_MODE_DEFAULT && writes == 15);

    idle_fixture = false;
    model = "Goldshell-SC5Pro2";
    firmware = "2.2.0";
    default_clock = 750; // Synthetic clocks and topology for the live qualification path.
    low_clock = 520;
    boards = 4;
    default_mhs = 14e6;
    low_mhs = 10e6;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"powerplans\":["
        "{\"level\":0,\"info\":\"750 MHz 12000 V\"},{\"level\":2,\"info\":\"520 MHz 10000 V\"}]}";
    const goldshell_mode_reading pro2 = goldshell_mode_inspect(ip);
    assert(pro2.status == GOLDSHELL_MODE_READY && pro2.mode == GOLDSHELL_MODE_DEFAULT && pro2.alternate == GOLDSHELL_MODE_LOW &&
        pro2.default_clock == 750 && pro2.board_count == 4 && pro2.experimental);
    model = "Goldshell SC5 Pro-II";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_READY);
    model = "Goldshell-SC5-Pro \\u2161";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_READY);
    model = "Goldshell-SC5-Pro XII";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_UNSUPPORTED);
    model = "Goldshell-SC5Pro2";
    default_mhs = 10e6;
    const goldshell_mode_reading weak = goldshell_mode_inspect(ip);
    assert(weak.status == GOLDSHELL_MODE_HASHRATE && weak.mode == GOLDSHELL_MODE_DEFAULT);
    assert(goldshell_mode_change(ip, "fixture", &pro2, GOLDSHELL_MODE_LOW, allowed).status == GOLDSHELL_MODE_HASHRATE && writes == 15);
    default_mhs = 14e6;
    boards = 3;
    assert(goldshell_mode_change(ip, "fixture", &pro2, GOLDSHELL_MODE_LOW, allowed).status == GOLDSHELL_MODE_CHANGED && writes == 15);
    boards = 4;
    hardware = "NEW.REV";
    assert(goldshell_mode_change(ip, "fixture", &pro2, GOLDSHELL_MODE_LOW, allowed).status == GOLDSHELL_MODE_CHANGED && writes == 15);
    hardware = "30.50.SA";
    const goldshell_mode_reading pro2_low = goldshell_mode_change(ip, "fixture", &pro2, GOLDSHELL_MODE_LOW, allowed);
    assert(pro2_low.status == GOLDSHELL_MODE_READY && pro2_low.mode == GOLDSHELL_MODE_LOW && writes == 16);
    assert(goldshell_mode_change(ip, "fixture", &pro2_low, GOLDSHELL_MODE_DEFAULT, allowed).mode == GOLDSHELL_MODE_DEFAULT && writes == 17);

    model = "Goldshell-CAEU14";
    algorithms = NULL;
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_ALGORITHM);
    algorithms = sc_algorithms;
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_READY);

    idle_fixture = true;
    model = "Goldshell-HS3-SE";
    firmware = "2.2.4";
    default_clock = low_clock = 750;
    setting = "{\"select\":%d,\"manual\":false,\"version\":\"v1.2\",\"powerplans\":["
        "{\"level\":0,\"info\":\"750 MHz 0.41 V 100 RPM 100 RPM\"},"
        "{\"level\":3,\"info\":\"0 MHz 0 V 100 RPM 100 RPM\"}]}";
    algorithms = "{\"algo_select\":0,\"algos\":[{\"name\":\"blake2bsha3(HNS)\",\"id\":0},"
        "{\"name\":\"blake2b(SC)\",\"id\":1}]}";
    assert(goldshell_mode_inspect(ip).status == GOLDSHELL_MODE_ALGORITHM);
    algorithms = "{\"algo_select\":1,\"algos\":[{\"name\":\"blake2bsha3(HNS)\",\"id\":0},"
        "{\"name\":\"blake2b(SC)\",\"id\":1}]}";
    const goldshell_mode_reading hs3se = goldshell_mode_inspect(ip);
    assert(hs3se.status == GOLDSHELL_MODE_READY && hs3se.idle_supported && hs3se.board_count == 4);
    const goldshell_mode_reading hs3se_idle = goldshell_mode_change(ip, "fixture", &hs3se, GOLDSHELL_MODE_IDLE, allowed);
    assert(hs3se_idle.mode == GOLDSHELL_MODE_IDLE && hs3se_idle.status == GOLDSHELL_MODE_READY && writes == 18);
    assert(goldshell_mode_change(ip, "fixture", &hs3se_idle, GOLDSHELL_MODE_DEFAULT, allowed).mode == GOLDSHELL_MODE_DEFAULT && writes == 19);
    check_restart(ip);
    puts("DISPATCH Goldshell preset, readback, and restart fixtures passed");
}

/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the INI parser's robustness: BOM handling, long network
 * fields, and an endless diet of malformed documents.  The parser must never
 * crash and must always report success, whatever the SD card contains.
 */
#include <stdio.h>
#include <string.h>

#include "mosu_config.h"
#include "test_util.h"

#ifndef MOSU_SOURCE_DIR
#define MOSU_SOURCE_DIR "."
#endif

#define FUZZ_DOCS 512
#define FUZZ_MUTATIONS 256
#define FUZZ_BUF 17000

static uint32_t s_rng = 0x4D4F5355u; /* "MOSU" */

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static size_t read_file(const char *path, char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    size_t len = fread(buf, 1, cap - 1, file);
    fclose(file);
    buf[len] = '\0';
    return len;
}

static void shipped_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/sd_card/mosu.cfg", MOSU_SOURCE_DIR);
}

static void test_shipped_config(void)
{
    char path[512];
    shipped_path(path, sizeof(path));

    mosu_config_t cfg;
    mosu_config_report_t report;
    mosu_config_defaults(&cfg);

    int rc = mosu_config_load(&cfg, path, &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(report.warnings == 0);
    EXPECT(report.lines > 50);
    EXPECT(report.applied > 40);
    EXPECT_STR(cfg.device.name, "MosuTime");
    EXPECT(cfg.display.brightness == 55);
    EXPECT(cfg.display.screen_on_ms == 11250);
    EXPECT_STR(cfg.clock.timezone, "Europe/Bucharest");
    EXPECT(cfg.clock.sync_hours == 12);
    EXPECT(cfg.clock.sync_per_day == 0);
    EXPECT(cfg.wifi_count == 1);
    EXPECT_STR(cfg.wifi[0].ssid, "Acasa");
    EXPECT_STR(cfg.wifi[0].pass, "parola");
    EXPECT(cfg.announce.volume == 85);
}

static void test_bom(void)
{
    static const char bom_ini[] =
        "\xEF\xBB\xBF[device]\n"
        "name = Bom\n"
        "[display]\n"
        "brightness = 33\n";

    mosu_config_t cfg;
    mosu_config_report_t report;
    mosu_config_defaults(&cfg);

    int rc = mosu_config_parse(&cfg, bom_ini, sizeof(bom_ini) - 1, &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(report.warnings == 0);
    EXPECT_STR(cfg.device.name, "Bom");
    EXPECT(cfg.display.brightness == 33);

    /* A BOM in the middle of the document is just data, not a doc marker. */
    static const char mid_ini[] =
        "[device]\n"
        "name = \xEF\xBB\xBFx\n";
    mosu_config_defaults(&cfg);
    rc = mosu_config_parse(&cfg, mid_ini, sizeof(mid_ini) - 1, &report);
    EXPECT(rc == 0);
    EXPECT_STR(cfg.device.name, "\xEF\xBB\xBFx");
}

static void test_long_network_fields(void)
{
    char ssid[41];
    char pass[71];
    memset(ssid, 'S', sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    memset(pass, 'P', sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';

    char ini[256];
    snprintf(ini, sizeof(ini),
             "[wifi]\nnetwork1_ssid = %s\nnetwork1_pass = %s\n", ssid, pass);

    mosu_config_t cfg;
    mosu_config_report_t report;
    mosu_config_defaults(&cfg);

    int rc = mosu_config_parse(&cfg, ini, strlen(ini), &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(report.warnings == 2);
    EXPECT(strlen(cfg.wifi[0].ssid) == MOSU_CFG_SSID_MAX - 1);
    EXPECT(strlen(cfg.wifi[0].pass) == MOSU_CFG_PASS_MAX - 1);
    EXPECT(cfg.wifi_count == 1);

    /* Exactly one byte less than the field size still fits, silently. */
    char ok_ssid[MOSU_CFG_SSID_MAX];
    char ok_pass[MOSU_CFG_PASS_MAX];
    memset(ok_ssid, 'a', sizeof(ok_ssid) - 1);
    ok_ssid[sizeof(ok_ssid) - 1] = '\0';
    memset(ok_pass, 'b', sizeof(ok_pass) - 1);
    ok_pass[sizeof(ok_pass) - 1] = '\0';

    snprintf(ini, sizeof(ini),
             "[wifi]\nnetwork1_ssid = %s\nnetwork1_pass = %s\n", ok_ssid,
             ok_pass);
    mosu_config_defaults(&cfg);
    rc = mosu_config_parse(&cfg, ini, strlen(ini), &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(report.warnings == 0);
    EXPECT(strlen(cfg.wifi[0].ssid) == MOSU_CFG_SSID_MAX - 1);
    EXPECT(strlen(cfg.wifi[0].pass) == MOSU_CFG_PASS_MAX - 1);
}

static void test_fuzz_random(void)
{
    static char doc[1024];
    int bad = 0;

    for (int i = 0; i < FUZZ_DOCS; i++) {
        size_t len = (size_t)(rnd() % sizeof(doc));
        for (size_t j = 0; j < len; j++) {
            doc[j] = (char)(rnd() & 0xFFu);
        }

        mosu_config_t cfg;
        mosu_config_report_t report;
        mosu_config_defaults(&cfg);
        if (mosu_config_parse(&cfg, doc, len, &report) != 0) {
            bad++;
        }
    }

    EXPECT(bad == 0);
}

static void test_fuzz_mutations(void)
{
    static char original[FUZZ_BUF];
    static char mutated[FUZZ_BUF];
    char path[512];
    shipped_path(path, sizeof(path));

    size_t base_len = read_file(path, original, sizeof(original));
    EXPECT(base_len > 0);

    if (base_len == 0) {
        return;
    }

    int bad = 0;
    for (int i = 0; i < FUZZ_MUTATIONS; i++) {
        memcpy(mutated, original, base_len);

        unsigned changes = 1u + rnd() % 8u;
        for (unsigned c = 0; c < changes; c++) {
            size_t pos = (size_t)(rnd() % base_len);
            mutated[pos] = (char)(rnd() & 0xFFu);
        }

        size_t len = base_len;
        if ((rnd() & 3u) == 0) {
            len = (size_t)(rnd() % (base_len + 1u));
        }

        mosu_config_t cfg;
        mosu_config_report_t report;
        mosu_config_defaults(&cfg);
        if (mosu_config_parse(&cfg, mutated, len, &report) != 0) {
            bad++;
        }
    }

    EXPECT(bad == 0);

    /* The shipped file with a BOM prepended must stay warning-free too. */
    static char with_bom[FUZZ_BUF];
    with_bom[0] = (char)0xEF;
    with_bom[1] = (char)0xBB;
    with_bom[2] = (char)0xBF;
    memcpy(with_bom + 3, original, base_len);

    mosu_config_t cfg;
    mosu_config_report_t report;
    mosu_config_defaults(&cfg);
    int rc = mosu_config_parse(&cfg, with_bom, base_len + 3, &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(report.warnings == 0);
    EXPECT_STR(cfg.device.name, "MosuTime");
}

static void test_sync_per_day(void)
{
    mosu_config_t cfg;
    mosu_config_report_t report;

    static const char per_day[] = "[clock]\nsync_per_day = 5\n";
    mosu_config_defaults(&cfg);
    int rc = mosu_config_parse(&cfg, per_day, sizeof(per_day) - 1, &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT(cfg.clock.sync_per_day == 5);
    EXPECT(cfg.clock.sync_hours == 12);

    static const char clamped[] = "[clock]\nsync_per_day = 99\n";
    mosu_config_defaults(&cfg);
    rc = mosu_config_parse(&cfg, clamped, sizeof(clamped) - 1, &report);
    EXPECT(rc == 0);
    EXPECT(cfg.clock.sync_per_day == 24);

    static const char disabled[] = "[clock]\nsync_per_day = 0\n";
    mosu_config_defaults(&cfg);
    rc = mosu_config_parse(&cfg, disabled, sizeof(disabled) - 1, &report);
    EXPECT(rc == 0);
    EXPECT(cfg.clock.sync_per_day == 0);
}

void test_config_extra(void)
{
    test_shipped_config();
    test_bom();
    test_long_network_fields();
    test_sync_per_day();
    test_fuzz_random();
    test_fuzz_mutations();
}

// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#ifndef DISPATCH_JSON_RECORD_H
#define DISPATCH_JSON_RECORD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t from, to, next;
} json_token;

// Allow four full Goldshell hashboard records within the existing byte limit.
enum { JSON_RECORD_TOKENS = 416, JSON_RECORD_BYTES = 4096 };
typedef struct {
    const char *source;
    json_token tokens[JSON_RECORD_TOKENS];
    unsigned used;
} json_record;

bool json_parse(json_record *doc, const char *text);
// Serial adapter scratch. Parsing another record invalidates the previous one.
const json_record *json_parse_shared(const char *text);
unsigned json_field(const json_record *doc, unsigned object, const char *key);
unsigned json_first(const json_record *doc, unsigned array);
bool json_equals(const json_record *doc, unsigned token, const char *literal);
bool json_number(const json_record *doc, unsigned token, double *value);
#endif

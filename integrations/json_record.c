// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
#include "json_record.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    json_record *doc;
    const char *p;
} parser;

static void spaces(parser *cursor)
{
    cursor->p += strspn(cursor->p, " \r\n\t");
}

static bool digits(parser *cursor)
{
    size_t n = strspn(cursor->p, "0123456789");
    cursor->p += n;
    return n != 0;
}

static bool number(parser *cursor)
{
    if (*cursor->p == '-') {
        ++cursor->p;
    }
    if (*cursor->p == '0') {
        ++cursor->p;
    } else if (!digits(cursor)) {
        return false;
    }
    if (*cursor->p == '.') {
        ++cursor->p;
        if (!digits(cursor)) {
            return false;
        }
    }
    if (*cursor->p == 'e' || *cursor->p == 'E') {
        ++cursor->p;
        if (*cursor->p == '+' || *cursor->p == '-') {
            ++cursor->p;
        }
        return digits(cursor);
    }
    return true;
}

static bool string(parser *cursor)
{
    ++cursor->p;
    while (*cursor->p && *cursor->p != '"') {
        const unsigned char ch = (unsigned char)*cursor->p++;
        if (ch < 0x20) {
            return false;
        }
        if (ch == '\\') {
            if (*cursor->p == 'u') {
                ++cursor->p;
                for (unsigned i = 0; i < 4; ++i) {
                    if (!isxdigit((unsigned char)*cursor->p)) {
                        return false;
                    }
                    ++cursor->p;
                }
            } else if (*cursor->p && strchr("\"\\/bfnrt", *cursor->p)) {
                ++cursor->p;
            } else {
                return false;
            }
        }
    }
    if (*cursor->p != '"') {
        return false;
    }
    ++cursor->p;
    return true;
}

static bool value(parser *cursor, unsigned depth);

static bool container(parser *cursor, unsigned depth)
{
    bool object = *cursor->p++ == '{';
    const char close = object ? '}' : ']';
    spaces(cursor);
    if (*cursor->p != close) {
        for (;;) {
            if (object) {
                if (*cursor->p != '"' || !value(cursor, depth + 1) || *cursor->p != ':') {
                    return false;
                }
                ++cursor->p;
            }
            if (!value(cursor, depth + 1)) {
                return false;
            }
            if (*cursor->p != ',') {
                break;
            }
            ++cursor->p;
            spaces(cursor);
        }
    }
    if (*cursor->p != close) {
        return false;
    }
    ++cursor->p;
    return true;
}

static bool literal(parser *cursor, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(cursor->p, word, n) != 0) {
        return false;
    }
    cursor->p += n;
    return true;
}

static bool value(parser *cursor, unsigned depth)
{
    spaces(cursor);
    if (depth > 8 || cursor->doc->used >= JSON_RECORD_TOKENS) {
        return false;
    }
    json_token *token = &cursor->doc->tokens[cursor->doc->used++];
    token->from = cursor->p - cursor->doc->source;
    bool ok;
    switch (*cursor->p) {
    case '{': case '[': ok = container(cursor, depth); break;
    case '"': ok = string(cursor); break;
    case 't': ok = literal(cursor, "true"); break;
    case 'f': ok = literal(cursor, "false"); break;
    case 'n': ok = literal(cursor, "null"); break;
    default: ok = number(cursor); break;
    }
    token->to = cursor->p - cursor->doc->source;
    token->next = cursor->doc->used;
    spaces(cursor);
    return ok;
}

bool json_parse(json_record *doc, const char *text)
{
    doc->source = text;
    doc->used = 1;
    parser cursor = {doc, text};
    return strlen(text) <= JSON_RECORD_BYTES &&
        value(&cursor, 0) && !*cursor.p;
}

const json_record *json_parse_shared(const char *text)
{
    static json_record record;
    return json_parse(&record, text) ? &record : NULL;
}

bool json_equals(const json_record *doc, unsigned index, const char *literal_text)
{
    if (!index || index >= doc->used) {
        return false;
    }
    const json_token item = doc->tokens[index];
    const char *start = doc->source + item.from;
    size_t size = item.to - item.from;
    return size == strlen(literal_text) && memcmp(start, literal_text, size) == 0;
}

unsigned json_field(const json_record *doc, unsigned object, const char *key)
{
    if (!object || object >= doc->used || doc->source[doc->tokens[object].from] != '{') {
        return 0;
    }
    unsigned match = 0;
    const size_t size = strlen(key);
    for (unsigned i = object + 1; i < doc->tokens[object].next; ) {
        const json_token item = doc->tokens[i];
        if ((size_t)(item.to - item.from) == size + 2 &&
            memcmp(doc->source + item.from + 1, key, size) == 0) {
            if (match) {
                return 0;
            }
            match = i + 1;
        }
        i = doc->tokens[i + 1].next;
    }
    return match;
}

unsigned json_first(const json_record *doc, unsigned array)
{
    return array && array < doc->used && doc->source[doc->tokens[array].from] == '[' &&
        doc->tokens[array].next > array + 1 ? array + 1 : 0;
}

bool json_number(const json_record *doc, unsigned index, double *value_out)
{
    if (!index || index >= doc->used) {
        return false;
    }
    const json_token token = doc->tokens[index];
    const char *start = doc->source + token.from;
    if (*start != '-' && (*start < '0' || *start > '9')) {
        return false;
    }
    char *end;
    double value_number = strtod(start, &end);
    if (end != doc->source + token.to || !isfinite(value_number)) {
        return false;
    }
    *value_out = value_number;
    return true;
}

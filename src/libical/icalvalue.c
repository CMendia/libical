/*======================================================================
 FILE: icalvalue.c
 CREATOR: eric 02 May 1999

 SPDX-FileCopyrightText: 2000, Eric Busboom <eric@civicknowledge.com>

 SPDX-License-Identifier: LGPL-2.1-only OR MPL-2.0

  Contributions from:
     Graham Davison <g.m.davison@computer.org>
======================================================================*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "icalvalue.h"
#include "icalvalueimpl.h"
#include "icalerror.h"
#include "icalmemory.h"
#include "icaltime.h"

#include <ctype.h>
#include <locale.h>
#include <stdlib.h>

#define TMP_BUF_SIZE 1024

LIBICAL_ICAL_EXPORT struct icalvalue_impl *icalvalue_new_impl(icalvalue_kind kind)
{
    struct icalvalue_impl *v;

    if (!icalvalue_kind_is_valid(kind))
        return NULL;

    if ((v = (struct icalvalue_impl *)icalmemory_new_buffer(sizeof(struct icalvalue_impl))) == 0) {
        icalerror_set_errno(ICAL_NEWFAILED_ERROR);
        return 0;
    }

    strcpy(v->id, "val");

    v->kind = kind;
    v->size = 0;
    v->parent = 0;
    v->x_value = 0;
    memset(&(v->data), 0, sizeof(v->data));

    return v;
}

icalvalue *icalvalue_new(icalvalue_kind kind)
{
    return (icalvalue *)icalvalue_new_impl(kind);
}

icalvalue *icalvalue_clone(const icalvalue *old)
{
    struct icalvalue_impl *clone;

    clone = icalvalue_new_impl(old->kind);

    if (clone == 0) {
        return 0;
    }

    strcpy(clone->id, old->id);
    clone->kind = old->kind;
    clone->size = old->size;

    switch (clone->kind) {
    case ICAL_ATTACH_VALUE:
    case ICAL_BINARY_VALUE: {
        /* Hmm.  We just ref the attach value, which may not be the right
             * thing to do.  We cannot quite copy the data, anyways, since we
             * don't know how long it is.
             */
        clone->data.v_attach = old->data.v_attach;
        if (clone->data.v_attach)
            icalattach_ref(clone->data.v_attach);

        break;
    }
    case ICAL_QUERY_VALUE:
    case ICAL_STRING_VALUE:
    case ICAL_TEXT_VALUE:
    case ICAL_CALADDRESS_VALUE:
    case ICAL_UID_VALUE:
    case ICAL_XMLREFERENCE_VALUE:
    case ICAL_URI_VALUE: {
        if (old->data.v_string != 0) {
            clone->data.v_string = icalmemory_strdup(old->data.v_string);

            if (clone->data.v_string == 0) {
                clone->parent = 0;
                icalvalue_free(clone);
                return 0;
            }
        }
        break;
    }
    case ICAL_ACTION_VALUE: {
        clone->data = old->data;

        if (old->data.v_enum == ICAL_ACTION_X) {
            //preserve the custom action string
            if (old->x_value != 0) {
                clone->x_value = icalmemory_strdup(old->x_value);

                if (clone->x_value == 0) {
                    clone->parent = 0;
                    icalvalue_free(clone);
                    return 0;
                }
            }
        }
        break;
    }
    case ICAL_RECUR_VALUE: {
        if (old->data.v_recur != 0) {
            clone->data.v_recur = icalrecurrencetype_clone(old->data.v_recur);
            if (clone->data.v_recur == 0) {
                icalvalue_free(clone);
                return 0;
            }
        }
        break;
    }

    case ICAL_X_VALUE: {
        if (old->x_value != 0) {
            clone->x_value = icalmemory_strdup(old->x_value);

            if (clone->x_value == 0) {
                clone->parent = 0;
                icalvalue_free(clone);
                return 0;
            }
        }

        break;
    }

    default: {
        /* all of the other types are stored as values, not
               pointers, so we can just copy the whole structure. */

        clone->data = old->data;
    }
    }

    return clone;
}

static char *icalmemory_strdup_and_dequote(const char *str)
{
    const char *p;
    char *out = (char *)icalmemory_new_buffer(sizeof(char) * strlen(str) + 1);
    char *pout;
    int wroteNull = 0;

    if (out == 0) {
        return 0;
    }

    pout = out;

    /* Stop the loop when encountering a terminator in the source string
       or if a null has been written to the destination. This prevents
       reading past the end of the source string if the last character
       is a backslash. */
    for (p = str; !wroteNull && *p != 0; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 0: {
                wroteNull = 1; //stops iteration so p isn't incremented past the end of str
                *pout = '\0';
                break;
            }
            case 'n':
            case 'N': {
                *pout = '\n';
                break;
            }
            case 't':
            case 'T': {
                *pout = '\t';
                break;
            }
            case 'r':
            case 'R': {
                *pout = '\r';
                break;
            }
            case 'b':
            case 'B': {
                *pout = '\b';
                break;
            }
            case 'f':
            case 'F': {
                *pout = '\f';
                break;
            }
            case ';':
            case ',':
            case '"':
            case '\\': {
                *pout = *p;
                break;
            }
            default: {
                *pout = ' ';
            }
            }
        } else {
            *pout = *p;
        }

        pout++;
    }

    *pout = '\0';

    return out;
}

/*
 * Returns a quoted copy of a string
 * @todo This is not RFC5545 compliant.
 * The RFC only allows:
 * TSAFE-CHAR = %x20-21 / %x23-2B / %x2D-39 / %x3C-5B / %x5D-7E / NON-US-ASCII
 * As such, \t\r\b\f are not allowed, not even escaped
 */
static char *icalmemory_strdup_and_quote(const icalvalue *value, const char *unquoted_str)
{
    /* oss-fuzz sets the cpu timeout at 60 seconds.
     * In order to meet that requirement we'd need to set MAX_ITERATIONS to (1024 * 128) approximately.
     * We don't feel safe setting MAX_ITERATIONS that low.
     */
    static const size_t MAX_ITERATIONS = (1024 * 1024 * 10);
    char *str;
    char *str_p;
    const char *p;
    size_t buf_sz;
    size_t cnt = 0; //track iterations

    buf_sz = strlen(unquoted_str) + 1;

    str_p = str = (char *)icalmemory_new_buffer(buf_sz);

    if (str_p == 0) {
        return 0;
    }

    for (p = unquoted_str; *p != 0 && cnt < MAX_ITERATIONS; p++, cnt++) {
        switch (*p) {
        case '\n': {
            icalmemory_append_string(&str, &str_p, &buf_sz, "\\n");
            break;
        }

            /*issue74: \t is not escaped, but embedded literally.*/
        case '\t': {
            icalmemory_append_string(&str, &str_p, &buf_sz, "\t");
            break;
        }

            /*issue74: \r, \b and \f are not whitespace and are trashed.*/
        case '\r': {
            /*icalmemory_append_string(&str,&str_p,&buf_sz,"\\r"); */
            break;
        }
        case '\b': {
            /*icalmemory_append_string(&str,&str_p,&buf_sz,"\\b"); */
            break;
        }
        case '\f': {
            /*icalmemory_append_string(&str,&str_p,&buf_sz,"\\f"); */
            break;
        }

        case ';':
        case ',':
            /* unescaped COMMA is allowed in CATEGORIES property as its
               considered a list delimiter here, see:
               https://tools.ietf.org/html/rfc5545#section-3.8.1.2 */
            if ((icalproperty_isa(value->parent) == ICAL_CATEGORIES_PROPERTY) ||
                (icalproperty_isa(value->parent) == ICAL_RESOURCES_PROPERTY) ||
                (icalproperty_isa(value->parent) == ICAL_POLLPROPERTIES_PROPERTY) ||
                (icalproperty_isa(value->parent) == ICAL_LOCATIONTYPE_PROPERTY) ||
                ((icalproperty_isa(value->parent) == ICAL_X_PROPERTY) &&
                 icalvalue_isa(value) != ICAL_TEXT_VALUE)) {
                icalmemory_append_char(&str, &str_p, &buf_sz, *p);
                break;
            }
            _fallthrough();
            /*issue74, we don't escape double quotes
        case '"':
*/
        case '\\': {
            icalmemory_append_char(&str, &str_p, &buf_sz, '\\');
            icalmemory_append_char(&str, &str_p, &buf_sz, *p);
            break;
        }

        default: {
            icalmemory_append_char(&str, &str_p, &buf_sz, *p);
        }
        }
    }

    /* Assume the last character is not a '\0' and add one. We could
       check *str_p != 0, but that would be an uninitialized memory
       read. */

    icalmemory_append_char(&str, &str_p, &buf_sz, '\0');
    return str;
}

/*
 * FIXME
 *
 * This is a bad API, as it forces callers to specify their own X type.
 * This function should take care of this by itself.
 */
static icalvalue *icalvalue_new_enum(icalvalue_kind kind, int x_type, const char *str)
{
    int e = icalproperty_kind_and_string_to_enum((int)kind, str);
    struct icalvalue_impl *value;

    if (e != 0 && icalproperty_enum_belongs_to_property(icalproperty_value_kind_to_kind(kind), e)) {
        value = icalvalue_new_impl(kind);
        value->data.v_enum = e;
    } else {
        /* Make it an X value */
        value = icalvalue_new_impl(kind);
        value->data.v_enum = x_type;
        icalvalue_set_x(value, str);
    }

    return value;
}

/**
 * Extracts a simple floating point number as a substring.
 * The decimal separator (if any) of the double has to be '.'
 * The code is locale *independent* and does *not* change the locale.
 * It should be thread safe.
 */
static bool simple_str_to_doublestr(const char *from, char *result, int result_len, char **to)
{
    char *start = NULL, *end = NULL, *cur = (char *)from;

    struct lconv *loc_data = localeconv();
    int i = 0, len;
    double dtest;

    /*sanity checks */
    if (!from || !result) {
        return true;
    }

    /*skip the white spaces at the beginning */
    while (*cur && isspace((int)*cur))
        cur++;

    start = cur;
    /* copy the part that looks like a double into result.
     * during the copy, we give ourselves a chance to convert the '.'
     * into the decimal separator of the current locale.
     */
    while (*cur && (isdigit((int)*cur) || *cur == '.' || *cur == '+' || *cur == '-')) {
        ++cur;
    }
    end = cur;
    len = (int)(ptrdiff_t)(end - start);
    if (len + 1 >= result_len) {
        /* huh hoh, number is too big. truncate it */
        len = result_len - 1;
    }

    /* copy the float number string into result, and take
     * care to have the (optional) decimal separator be the one
     * of the current locale.
     */
    for (i = 0; i < len; ++i) {
        if (start[i] == '.' &&
            loc_data && loc_data->decimal_point && loc_data->decimal_point[0] && loc_data->decimal_point[0] != '.') {
            /*replace '.' by the digit separator of the current locale */
            result[i] = loc_data->decimal_point[0];
        } else {
            result[i] = start[i];
        }
    }
    if (to) {
        *to = end;
    }

    /* now try to convert to a floating point number, to check for validity only */
    if (sscanf(result, "%lf", &dtest) != 1) {
        return true;
    }
    return false;
}

static void free_icalvalue_attach_data(char *data, void *user_data)
{
    _unused(user_data);
    free(data);
}

static icalvalue *icalvalue_new_from_string_with_error(icalvalue_kind kind,
                                                       const char *str, icalproperty **error)
{
    struct icalvalue_impl *value = 0;

    icalerror_check_arg_rz(str != 0, "str");

    if (error != 0) {
        *error = 0;
    }

    switch (kind) {
    case ICAL_ATTACH_VALUE: {
        icalattach *attach;

        attach = icalattach_new_from_url(str);
        if (!attach)
            break;

        value = icalvalue_new_attach(attach);
        icalattach_unref(attach);
        break;
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
    case ICAL_BINARY_VALUE: {
        icalattach *attach;

        char *dupStr = strdup(str); // will be freed later on during unref
        if (dupStr) {
            attach = icalattach_new_from_data(dupStr, free_icalvalue_attach_data, 0);
            if (!attach) {
                free(dupStr);
                break;
            }

            value = icalvalue_new_attach(attach);
            icalattach_unref(attach);
        }
        break;
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    case ICAL_BOOLEAN_VALUE: {
        if (!strcmp(str, "TRUE")) {
            value = icalvalue_new_boolean(1);
        } else if (!strcmp(str, "FALSE")) {
            value = icalvalue_new_boolean(0);
        } else if (error != 0) {
            char temp[TMP_BUF_SIZE];
            icalparameter *errParam;

            snprintf(temp, sizeof(temp),
                     "Could not parse %s as a %s property",
                     str, icalvalue_kind_to_string(kind));
            errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_VALUEPARSEERROR);
            *error = icalproperty_vanew_xlicerror(temp, errParam, (void *)0);
        }
        break;
    }

    case ICAL_TRANSP_VALUE:
        value = icalvalue_new_enum(kind, (int)ICAL_TRANSP_X, str);
        break;
    case ICAL_METHOD_VALUE:
        value = icalvalue_new_enum(kind, (int)ICAL_METHOD_X, str);
        break;
    case ICAL_STATUS_VALUE:
        value = icalvalue_new_enum(kind, (int)ICAL_STATUS_X, str);
        break;
    case ICAL_ACTION_VALUE:
        value = icalvalue_new_enum(kind, (int)ICAL_ACTION_X, str);
        break;

    case ICAL_QUERY_VALUE:
        value = icalvalue_new_query(str);
        break;

    case ICAL_CLASS_VALUE:
        value = icalvalue_new_enum(kind, (int)ICAL_CLASS_X, str);
        break;
    case ICAL_CMD_VALUE:
        value = icalvalue_new_enum(kind, ICAL_CMD_X, str);
        break;
    case ICAL_QUERYLEVEL_VALUE:
        value = icalvalue_new_enum(kind, ICAL_QUERYLEVEL_X, str);
        break;
    case ICAL_CARLEVEL_VALUE:
        value = icalvalue_new_enum(kind, ICAL_CARLEVEL_X, str);
        break;
    case ICAL_BUSYTYPE_VALUE:
        value = icalvalue_new_enum(kind, ICAL_BUSYTYPE_X, str);
        break;
    case ICAL_PROXIMITY_VALUE:
        value = icalvalue_new_enum(kind, ICAL_PROXIMITY_X, str);
        break;
    case ICAL_POLLMODE_VALUE:
        value = icalvalue_new_enum(kind, ICAL_POLLMODE_X, str);
        break;
    case ICAL_POLLCOMPLETION_VALUE:
        value = icalvalue_new_enum(kind, ICAL_POLLCOMPLETION_X, str);
        break;

    case ICAL_PARTICIPANTTYPE_VALUE:
        value = icalvalue_new_enum(kind, ICAL_PARTICIPANTTYPE_X, str);
        break;

    case ICAL_RESOURCETYPE_VALUE:
        value = icalvalue_new_enum(kind, ICAL_RESOURCETYPE_X, str);
        break;

    case ICAL_INTEGER_VALUE:
        value = icalvalue_new_integer(atoi(str));
        break;

    case ICAL_FLOAT_VALUE:
        value = icalvalue_new_float((float)atof(str));
        break;

    case ICAL_UTCOFFSET_VALUE: {
        int t, utcoffset, hours, minutes, seconds;

        /* treat the UTCOFSET string as a decimal number, disassemble its digits
               and reconstruct it as sections */
        t = strtol(str, 0, 10);
        /* add phantom seconds field */
        if (strlen(str) < 7) {
            t *= 100;
        }
        hours = (t / 10000);
        minutes = (t - hours * 10000) / 100;
        seconds = (t - hours * 10000 - minutes * 100);
        utcoffset = hours * 3600 + minutes * 60 + seconds;

        value = icalvalue_new_utcoffset(utcoffset);

        break;
    }

    case ICAL_TEXT_VALUE: {
        char *dequoted_str = icalmemory_strdup_and_dequote(str);

        value = icalvalue_new_text(dequoted_str);
        icalmemory_free_buffer(dequoted_str);
        break;
    }

    case ICAL_STRING_VALUE:
        value = icalvalue_new_string(str);
        break;

    case ICAL_CALADDRESS_VALUE:
        value = icalvalue_new_caladdress(str);
        break;

    case ICAL_URI_VALUE:
        value = icalvalue_new_uri(str);
        break;

    case ICAL_GEO_VALUE: {
        char *cur = NULL;
        struct icalgeotype geo;
        memset(geo.lat, 0, ICAL_GEO_LEN);
        memset(geo.lon, 0, ICAL_GEO_LEN);

        if (simple_str_to_doublestr(str, geo.lat, ICAL_GEO_LEN, &cur)) {
            goto geo_parsing_error;
        }
        /* skip white spaces */
        while (cur && isspace((int)*cur)) {
            ++cur;
        }

        /*there is a ';' between the latitude and longitude parts */
        if (!cur || *cur != ';') {
            goto geo_parsing_error;
        }

        ++cur;

        /* skip white spaces */
        while (cur && isspace((int)*cur)) {
            ++cur;
        }

        if (simple_str_to_doublestr(cur, geo.lon, ICAL_GEO_LEN, &cur)) {
            goto geo_parsing_error;
        }
        value = icalvalue_new_geo(geo);
        break;

    geo_parsing_error:
        if (error != 0) {
            char temp[TMP_BUF_SIZE];
            icalparameter *errParam;

            snprintf(temp, sizeof(temp),
                     "Could not parse %s as a %s property",
                     str, icalvalue_kind_to_string(kind));
            errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_VALUEPARSEERROR);
            *error = icalproperty_vanew_xlicerror(temp, errParam, (void *)0);
        }
    } break;

    case ICAL_RECUR_VALUE: {
        struct icalrecurrencetype *rt;

        rt = icalrecurrencetype_new_from_string(str);
        if (rt) {
            value = icalvalue_new_recur(rt);
            icalrecurrencetype_unref(rt);
        }
        break;
    }

    case ICAL_DATE_VALUE:
    case ICAL_DATETIME_VALUE: {
        struct icaltimetype tt;

        tt = icaltime_from_string(str);
        if (!icaltime_is_null_time(tt)) {
            value = icalvalue_new_impl(kind);
            value->data.v_time = tt;

            icalvalue_reset_kind(value);
        }
        break;
    }

    case ICAL_DATETIMEPERIOD_VALUE: {
        struct icaltimetype tt;
        struct icalperiodtype p;

        tt = icaltime_from_string(str);

        if (!icaltime_is_null_time(tt)) {
            value = icalvalue_new_datetime(tt);
            break;
        }

        p = icalperiodtype_from_string(str);
        if (!icalperiodtype_is_null_period(p)) {
            value = icalvalue_new_period(p);
        }

        break;
    }

    case ICAL_DURATION_VALUE: {
        struct icaldurationtype dur = icaldurationtype_from_string(str);

        if (!icaldurationtype_is_bad_duration(dur)) { /* failed to parse */
            value = icalvalue_new_duration(dur);
        }

        break;
    }

    case ICAL_PERIOD_VALUE: {
        struct icalperiodtype p;

        p = icalperiodtype_from_string(str);

        if (!icalperiodtype_is_null_period(p)) {
            value = icalvalue_new_period(p);
        }
        break;
    }

    case ICAL_TRIGGER_VALUE: {
        struct icaltriggertype tr = icaltriggertype_from_string(str);

        if (!icaltriggertype_is_bad_trigger(tr)) {
            value = icalvalue_new_trigger(tr);
        }
        break;
    }

    case ICAL_REQUESTSTATUS_VALUE: {
        struct icalreqstattype rst = icalreqstattype_from_string(str);

        if (rst.code != ICAL_UNKNOWN_STATUS) {
            value = icalvalue_new_requeststatus(rst);
        }
        break;
    }

    case ICAL_UID_VALUE: {
        char *dequoted_str = icalmemory_strdup_and_dequote(str);

        value = icalvalue_new_uid(dequoted_str);
        icalmemory_free_buffer(dequoted_str);
        break;
    }

    case ICAL_XMLREFERENCE_VALUE:
        value = icalvalue_new_xmlreference(str);
        break;

    case ICAL_X_VALUE: {
        char *dequoted_str = icalmemory_strdup_and_dequote(str);

        value = icalvalue_new_x(dequoted_str);
        icalmemory_free_buffer(dequoted_str);
    } break;

    default: {
        char temp[TMP_BUF_SIZE];
        icalparameter *errParam;

        if (error != 0) {
            snprintf(temp, TMP_BUF_SIZE, "Unknown type for \'%s\'", str);

            errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_VALUEPARSEERROR);
            *error = icalproperty_vanew_xlicerror(temp, errParam, (void *)0);
        }

        snprintf(temp, TMP_BUF_SIZE,
                 "icalvalue_new_from_string got an unknown value type (%s) for \'%s\'",
                 icalvalue_kind_to_string(kind), str);
        icalerror_warn(temp);
        value = 0;
    }
    }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
    if (error != 0 && *error == 0 && value == 0) {
        char temp[TMP_BUF_SIZE];
        icalparameter *errParam;

        snprintf(temp, TMP_BUF_SIZE, "Failed to parse value: \'%s\'", str);

        errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_VALUEPARSEERROR);
        *error = icalproperty_vanew_xlicerror(temp, errParam, (void *)0);
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    return value;
}

icalvalue *icalvalue_new_from_string(icalvalue_kind kind, const char *str)
{
    return icalvalue_new_from_string_with_error(kind, str, (icalproperty **)0);
}

void icalvalue_free(icalvalue *v)
{
    icalerror_check_arg_rv((v != 0), "value");

    if (v->parent != 0) {
        return;
    }

    if (v->x_value != 0) {
        icalmemory_free_buffer(v->x_value);
    }

    switch (v->kind) {
    case ICAL_BINARY_VALUE:
    case ICAL_ATTACH_VALUE: {
        if (v->data.v_attach) {
            icalattach_unref(v->data.v_attach);
            v->data.v_attach = NULL;
        }

        break;
    }
    case ICAL_TEXT_VALUE:
        _fallthrough();

    case ICAL_CALADDRESS_VALUE:
        _fallthrough();

    case ICAL_URI_VALUE:
        _fallthrough();

    case ICAL_STRING_VALUE:
        _fallthrough();

    case ICAL_QUERY_VALUE: {
        _fallthrough();
    case ICAL_UID_VALUE:
        _fallthrough();
    case ICAL_XMLREFERENCE_VALUE:
        if (v->data.v_string != 0) {
            icalmemory_free_buffer((void *)v->data.v_string);
            v->data.v_string = 0;
        }
        break;
    }
    case ICAL_RECUR_VALUE: {
        if (v->data.v_recur != 0) {
            icalrecurrencetype_unref(v->data.v_recur);
            v->data.v_recur = NULL;
        }
        break;
    }

    default: {
        /* Nothing to do */
    }
    }

    v->kind = ICAL_NO_VALUE;
    v->size = 0;
    v->parent = 0;
    memset(&(v->data), 0, sizeof(v->data));
    v->id[0] = 'X';
    icalmemory_free_buffer(v);
}

bool icalvalue_is_valid(const icalvalue *value)
{
    if (value == 0) {
        return false;
    }

    return true;
}

static char *icalvalue_binary_as_ical_string_r(const icalvalue *value)
{
    char *str;

    icalerror_check_arg_rz((value != 0), "value");

    str = (char *)icalmemory_new_buffer(60);
    snprintf(str, 60, "icalvalue_binary_as_ical_string is not implemented yet");

    return str;
}

static char *icalvalue_boolean_as_ical_string_r(const icalvalue *value)
{
    int data;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");
    str = (char *)icalmemory_new_buffer(6);

    data = icalvalue_get_integer(value);

    strcpy(str, data ? "TRUE" : "FALSE");

    return str;
}

#define MAX_INT_DIGITS 12 /* Enough for 2^32 + sign */

static char *icalvalue_int_as_ical_string_r(const icalvalue *value)
{
    int data;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");
    str = (char *)icalmemory_new_buffer(MAX_INT_DIGITS);

    data = icalvalue_get_integer(value);

    snprintf(str, MAX_INT_DIGITS, "%d", data);

    return str;
}

static char *icalvalue_utcoffset_as_ical_string_r(const icalvalue *value)
{
    int data, h, m, s;
    char sign;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");

    str = (char *)icalmemory_new_buffer(9);
    data = icalvalue_get_utcoffset(value);

    if (abs(data) == data) {
        sign = '+';
    } else {
        sign = '-';
    }

    h = data / 3600;
    m = (data - (h * 3600)) / 60;
    s = (data - (h * 3600) - (m * 60));

    h = MIN(abs(h), 23);
    m = MIN(abs(m), 59);
    s = MIN(abs(s), 59);
    if (s != 0) {
        snprintf(str, 9, "%c%02d%02d%02d", sign, h, m, s);
    } else {
        snprintf(str, 9, "%c%02d%02d", sign, h, m);
    }

    return str;
}

static char *icalvalue_string_as_ical_string_r(const icalvalue *value)
{
    const char *data;
    char *str = 0;

    icalerror_check_arg_rz((value != 0), "value");
    data = value->data.v_string;

    str = (char *)icalmemory_new_buffer(strlen(data) + 1);

    strcpy(str, data);

    return str;
}

static char *icalvalue_recur_as_ical_string_r(const icalvalue *value)
{
    struct icalrecurrencetype *recur = value->data.v_recur;

    return icalrecurrencetype_as_string_r(recur);
}

static char *icalvalue_text_as_ical_string_r(const icalvalue *value)
{
    return icalmemory_strdup_and_quote(value, value->data.v_string);
}

static char *icalvalue_attach_as_ical_string_r(const icalvalue *value)
{
    icalattach *a;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");

    a = icalvalue_get_attach(value);

    if (icalattach_get_is_url(a)) {
        const char *url;

        url = icalattach_get_url(a);
        str = icalmemory_new_buffer(strlen(url) + 1);
        strcpy(str, url);
        return str;
    } else {
        const char *data = 0;

        data = (const char *)icalattach_get_data(a);
        str = icalmemory_new_buffer(strlen(data) + 1);
        strcpy(str, data);
        return str;
    }
}

static char *icalvalue_duration_as_ical_string_r(const icalvalue *value)
{
    struct icaldurationtype data;

    icalerror_check_arg_rz((value != 0), "value");
    data = icalvalue_get_duration(value);

    return icaldurationtype_as_ical_string_r(data);
}

static void print_time_to_string(char *str, const struct icaltimetype *data)
{ /* this function is a candidate for a library-wide external function
           except it isn't used any place outside of icalvalue.c.
           see print_date_to_string() and print_datetime_to_string in icalvalue.h */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
    char temp[8];

    str[0] = '\0';
    if (data != 0) {
        if (icaltime_is_utc(*data)) {
            snprintf(temp, sizeof(temp), "%02d%02d%02dZ", data->hour, data->minute, data->second);
            strncat(str, temp, 7);
        } else {
            snprintf(temp, sizeof(temp), "%02d%02d%02d", data->hour, data->minute, data->second);
            strncat(str, temp, 6);
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

void print_date_to_string(char *str, const struct icaltimetype *data)
{
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
    char temp[9];

    str[0] = '\0';

    if (data != 0) {
        snprintf(temp, sizeof(temp), "%04d%02d%02d", data->year, data->month, data->day);
        strncat(str, temp, 8);
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

static char *icalvalue_date_as_ical_string_r(const icalvalue *value)
{
    struct icaltimetype data;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");
    data = icalvalue_get_date(value);

    str = (char *)icalmemory_new_buffer(9);

    str[0] = '\0';
    print_date_to_string(str, &data);

    return str;
}

void print_datetime_to_string(char *str, const struct icaltimetype *data)
{
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
    char temp[20] = {0};

    str[0] = '\0';
    if (data != 0) {
        print_date_to_string(str, data);
        if (!data->is_date) {
            strncat(str, "T", 19);
            temp[0] = '\0';
            print_time_to_string(temp, data);
            strncat(str, temp, 19);
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

static char *icalvalue_datetime_as_ical_string_r(const icalvalue *value)
{
    struct icaltimetype data;
    char *str;
    icalvalue_kind kind = icalvalue_isa(value);

    icalerror_check_arg_rz((value != 0), "value");

    if (!(kind == ICAL_DATE_VALUE || kind == ICAL_DATETIME_VALUE)) {
        icalerror_set_errno(ICAL_BADARG_ERROR);
        return 0;
    }

    data = icalvalue_get_datetime(value);

    str = (char *)icalmemory_new_buffer(20);

    str[0] = 0;
    print_datetime_to_string(str, &data);

    return str;
}

static char *icalvalue_float_as_ical_string_r(const icalvalue *value)
{
    float data;
    char *str;
    char *old_locale;

    icalerror_check_arg_rz((value != 0), "value");
    data = icalvalue_get_float(value);

    /* bypass current locale in order to make
       sure snprintf uses a '.' as a separator
       set locate to 'C' and keep old locale */
    old_locale = icalmemory_strdup(setlocale(LC_NUMERIC, NULL));
    (void)setlocale(LC_NUMERIC, "C");

    str = (char *)icalmemory_new_buffer(40);

    snprintf(str, 40, "%f", data);

    /* restore saved locale */
    (void)setlocale(LC_NUMERIC, old_locale);
    icalmemory_free_buffer(old_locale);

    return str;
}

static char *icalvalue_geo_as_ical_string_r(const icalvalue *value)
{
    struct icalgeotype data;
    char *str;

    icalerror_check_arg_rz((value != 0), "value");

    data = icalvalue_get_geo(value);
    str = (char *)icalmemory_new_buffer(80);
    snprintf(str, 80, "%s;%s", data.lat, data.lon);

    return str;
}

static char *icalvalue_datetimeperiod_as_ical_string_r(const icalvalue *value)
{
    struct icaldatetimeperiodtype dtp = icalvalue_get_datetimeperiod(value);

    icalerror_check_arg_rz((value != 0), "value");

    if (!icaltime_is_null_time(dtp.time)) {
        return icaltime_as_ical_string_r(dtp.time);
    } else {
        return icalperiodtype_as_ical_string_r(dtp.period);
    }
}

static char *icalvalue_period_as_ical_string_r(const icalvalue *value)
{
    struct icalperiodtype data;

    icalerror_check_arg_rz((value != 0), "value");
    data = icalvalue_get_period(value);

    return icalperiodtype_as_ical_string_r(data);
}

static char *icalvalue_trigger_as_ical_string_r(const icalvalue *value)
{
    struct icaltriggertype data;

    icalerror_check_arg_rz((value != 0), "value");
    data = icalvalue_get_trigger(value);

    if (!icaltime_is_null_time(data.time)) {
        return icaltime_as_ical_string_r(data.time);
    } else {
        return icaldurationtype_as_ical_string_r(data.duration);
    }
}

const char *icalvalue_as_ical_string(const icalvalue *value)
{
    char *buf;

    buf = icalvalue_as_ical_string_r(value);
    icalmemory_add_tmp_buffer(buf);
    return buf;
}

char *icalvalue_as_ical_string_r(const icalvalue *value)
{
    if (value == 0) {
        return 0;
    }

    switch (value->kind) {
    case ICAL_ATTACH_VALUE:
        return icalvalue_attach_as_ical_string_r(value);

    case ICAL_BINARY_VALUE:
        return icalvalue_binary_as_ical_string_r(value);

    case ICAL_BOOLEAN_VALUE:
        return icalvalue_boolean_as_ical_string_r(value);

    case ICAL_INTEGER_VALUE:
        return icalvalue_int_as_ical_string_r(value);

    case ICAL_UTCOFFSET_VALUE:
        return icalvalue_utcoffset_as_ical_string_r(value);

    case ICAL_TEXT_VALUE:
    case ICAL_UID_VALUE:
        return icalvalue_text_as_ical_string_r(value);

    case ICAL_QUERY_VALUE:
        return icalvalue_string_as_ical_string_r(value);

    case ICAL_STRING_VALUE:
    case ICAL_URI_VALUE:
    case ICAL_CALADDRESS_VALUE:
    case ICAL_XMLREFERENCE_VALUE:
        return icalvalue_string_as_ical_string_r(value);

    case ICAL_DATE_VALUE:
        return icalvalue_date_as_ical_string_r(value);
    case ICAL_DATETIME_VALUE:
        return icalvalue_datetime_as_ical_string_r(value);
    case ICAL_DURATION_VALUE:
        return icalvalue_duration_as_ical_string_r(value);

    case ICAL_PERIOD_VALUE:
        return icalvalue_period_as_ical_string_r(value);
    case ICAL_DATETIMEPERIOD_VALUE:
        return icalvalue_datetimeperiod_as_ical_string_r(value);

    case ICAL_FLOAT_VALUE:
        return icalvalue_float_as_ical_string_r(value);

    case ICAL_GEO_VALUE:
        return icalvalue_geo_as_ical_string_r(value);

    case ICAL_RECUR_VALUE:
        return icalvalue_recur_as_ical_string_r(value);

    case ICAL_TRIGGER_VALUE:
        return icalvalue_trigger_as_ical_string_r(value);

    case ICAL_REQUESTSTATUS_VALUE:
        return icalreqstattype_as_string_r(value->data.v_requeststatus);

    case ICAL_ACTION_VALUE:
    case ICAL_CMD_VALUE:
    case ICAL_QUERYLEVEL_VALUE:
    case ICAL_CARLEVEL_VALUE:
    case ICAL_METHOD_VALUE:
    case ICAL_STATUS_VALUE:
    case ICAL_TRANSP_VALUE:
    case ICAL_CLASS_VALUE:
    case ICAL_BUSYTYPE_VALUE:
    case ICAL_PROXIMITY_VALUE:
    case ICAL_POLLMODE_VALUE:
    case ICAL_POLLCOMPLETION_VALUE:
    case ICAL_PARTICIPANTTYPE_VALUE:
    case ICAL_RESOURCETYPE_VALUE:
        if (value->x_value != 0) {
            return icalmemory_strdup(value->x_value);
        }

        return icalproperty_enum_to_string_r(value->data.v_enum);

    case ICAL_X_VALUE:
        if (value->x_value != 0) {
            return icalmemory_strdup_and_quote(value, value->x_value);
        }
        _fallthrough();

    case ICAL_NO_VALUE:
        _fallthrough();

    default: {
        return 0;
    }
    }
}

icalvalue_kind icalvalue_isa(const icalvalue *value)
{
    if (value == 0) {
        return ICAL_NO_VALUE;
    }

    return value->kind;
}

bool icalvalue_isa_value(void *value)
{
    struct icalvalue_impl *impl = (struct icalvalue_impl *)value;

    icalerror_check_arg_rz((value != 0), "value");

    if (strcmp(impl->id, "val") == 0) {
        return true;
    } else {
        return false;
    }
}

static bool icalvalue_is_time(const icalvalue *a)
{
    icalvalue_kind kind = icalvalue_isa(a);

    if (kind == ICAL_DATETIME_VALUE || kind == ICAL_DATE_VALUE) {
        return true;
    }

    return false;
}

/*
 * In case of error, this function returns 0. This is partly bogus, as 0 is
 * not part of the returned enum.
 * FIXME We should probably add an error value to the enum.
 */
icalparameter_xliccomparetype icalvalue_compare(const icalvalue *a, const icalvalue *b)
{
    icalerror_check_arg_rz((a != 0), "a");
    icalerror_check_arg_rz((b != 0), "b");

    /* Not the same type; they can only be unequal */
    if (!(icalvalue_is_time(a) && icalvalue_is_time(b)) && icalvalue_isa(a) != icalvalue_isa(b)) {
        return ICAL_XLICCOMPARETYPE_NOTEQUAL;
    }

    switch (icalvalue_isa(a)) {
    case ICAL_ATTACH_VALUE: {
        if (icalattach_get_is_url(a->data.v_attach) &&
            icalattach_get_is_url(b->data.v_attach)) {
            if (strcasecmp(icalattach_get_url(a->data.v_attach),
                           icalattach_get_url(b->data.v_attach)) == 0) {
                return ICAL_XLICCOMPARETYPE_EQUAL;
            } else {
                return ICAL_XLICCOMPARETYPE_NOTEQUAL;
            }
        } else {
            if (a->data.v_attach == b->data.v_attach) {
                return ICAL_XLICCOMPARETYPE_EQUAL;
            } else {
                return ICAL_XLICCOMPARETYPE_NOTEQUAL;
            }
        }
    }
    case ICAL_BINARY_VALUE: {
        if (a->data.v_attach == b->data.v_attach) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_BOOLEAN_VALUE: {
        if (icalvalue_get_boolean(a) == icalvalue_get_boolean(b)) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_FLOAT_VALUE: {
        if (a->data.v_float > b->data.v_float) {
            return ICAL_XLICCOMPARETYPE_GREATER;
        } else if (a->data.v_float < b->data.v_float) {
            return ICAL_XLICCOMPARETYPE_LESS;
        } else {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        }
    }

    case ICAL_INTEGER_VALUE:
    case ICAL_UTCOFFSET_VALUE: {
        if (a->data.v_int > b->data.v_int) {
            return ICAL_XLICCOMPARETYPE_GREATER;
        } else if (a->data.v_int < b->data.v_int) {
            return ICAL_XLICCOMPARETYPE_LESS;
        } else {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        }
    }

    case ICAL_DURATION_VALUE: {
        int dur_a = icaldurationtype_as_int(a->data.v_duration);
        int dur_b = icaldurationtype_as_int(b->data.v_duration);

        if (dur_a > dur_b) {
            return ICAL_XLICCOMPARETYPE_GREATER;
        } else if (dur_a < dur_b) {
            return ICAL_XLICCOMPARETYPE_LESS;
        } else {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        }
    }

    case ICAL_TEXT_VALUE:
    case ICAL_URI_VALUE:
    case ICAL_CALADDRESS_VALUE:
    case ICAL_TRIGGER_VALUE:
    case ICAL_DATE_VALUE:
    case ICAL_DATETIME_VALUE:
    case ICAL_DATETIMEPERIOD_VALUE:
    case ICAL_QUERY_VALUE:
    case ICAL_UID_VALUE:
    case ICAL_XMLREFERENCE_VALUE:
    case ICAL_RECUR_VALUE: {
        int r;
        char *temp1, *temp2;

        temp1 = icalvalue_as_ical_string_r(a);
        if (temp1) {
            temp2 = icalvalue_as_ical_string_r(b);
            if (temp2) {
                r = strcmp(temp1, temp2);
            } else {
                icalmemory_free_buffer(temp1);
                return ICAL_XLICCOMPARETYPE_GREATER;
            }
        } else {
            return ICAL_XLICCOMPARETYPE_LESS;
        }
        icalmemory_free_buffer(temp1);
        icalmemory_free_buffer(temp2);

        if (r > 0) {
            return ICAL_XLICCOMPARETYPE_GREATER;
        } else if (r < 0) {
            return ICAL_XLICCOMPARETYPE_LESS;
        } else {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        }
    }

    case ICAL_METHOD_VALUE: {
        if (icalvalue_get_method(a) == icalvalue_get_method(b)) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_STATUS_VALUE: {
        if (icalvalue_get_status(a) == icalvalue_get_status(b)) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_TRANSP_VALUE: {
        if (icalvalue_get_transp(a) == icalvalue_get_transp(b)) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_ACTION_VALUE: {
        if (icalvalue_get_action(a) == icalvalue_get_action(b)) {
            return ICAL_XLICCOMPARETYPE_EQUAL;
        } else {
            return ICAL_XLICCOMPARETYPE_NOTEQUAL;
        }
    }

    case ICAL_PERIOD_VALUE:
    case ICAL_GEO_VALUE:
    case ICAL_NO_VALUE:
    default: {
        icalerror_warn("Comparison not implemented for value type");
        return 0;
    }
    }
}

/** Examine the value and possibly change the kind to agree with the
 *  value
 */

void icalvalue_reset_kind(icalvalue *value)
{
    if (value &&
        (value->kind == ICAL_DATETIME_VALUE || value->kind == ICAL_DATE_VALUE) &&
        !icaltime_is_null_time(value->data.v_time)) {
        if (icaltime_is_date(value->data.v_time)) {
            value->kind = ICAL_DATE_VALUE;
        } else {
            value->kind = ICAL_DATETIME_VALUE;
        }
    }
}

void icalvalue_set_parent(icalvalue *value, icalproperty *property)
{
    icalerror_check_arg_rv((value != 0), "value");

    value->parent = property;
}

icalproperty *icalvalue_get_parent(icalvalue *value)
{
    return value->parent;
}

bool icalvalue_encode_ical_string(const char *szText, char *szEncText, int nMaxBufferLen)
{
    char *ptr;
    icalvalue *value = 0;

    if ((szText == 0) || (szEncText == 0))
        return false;

    value = icalvalue_new_from_string(ICAL_STRING_VALUE, szText);

    if (value == 0)
        return false;

    ptr = icalvalue_text_as_ical_string_r(value);
    if (ptr == 0)
        return false;

    if ((int)strlen(ptr) >= nMaxBufferLen) {
        icalvalue_free(value);
        icalmemory_free_buffer(ptr);
        return false;
    }

    strcpy(szEncText, ptr);
    icalmemory_free_buffer(ptr);

    icalvalue_free((icalvalue *)value);

    return true;
}

bool icalvalue_decode_ical_string(const char *szText, char *szDecText, int nMaxBufferLen)
{
    char *str, *str_p;
    const char *p;
    size_t buf_sz;

    if ((szText == 0) || (szDecText == 0))
        return false;

    buf_sz = strlen(szText) + 1;
    str_p = str = (char *)icalmemory_new_buffer(buf_sz);

    if (str_p == 0) {
        return false;
    }

    for (p = szText; *p != 0; p++) {
        if (*p == '\\') {
            icalmemory_append_char(&str, &str_p, &buf_sz, *(p + 1));
            p++;
        } else {
            icalmemory_append_char(&str, &str_p, &buf_sz, *p);
        }

        if (str_p - str > nMaxBufferLen)
            break;
    }

    icalmemory_append_char(&str, &str_p, &buf_sz, '\0');

    if ((int)strlen(str) >= nMaxBufferLen) {
        icalmemory_free_buffer(str);
        return false;
    }

    strcpy(szDecText, str);

    icalmemory_free_buffer(str);
    return true;
}

/* The remaining interfaces are 'new', 'set' and 'get' for each of the value
   types */

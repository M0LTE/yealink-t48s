// SPDX-License-Identifier: AGPL-3.0-or-later
/* The time zones the phone knows by name: the Settings screen's list and the
 * web page's (webconfig/). */
#ifndef T48_ZONES_H
#define T48_ZONES_H

/* musl and a read-only image: no zone files, so the rules are here, in the
 * form the C library takes in TZ. A zone not listed can be set over SSH as
 * that kind of string (t48-config set system timezone 'XYZ-3'). */
static const struct { const char *name, *tz; } zones[] = {
    {"UTC", "UTC0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Atlantic/Reykjavik", "GMT0"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Nairobi", "EAT-3"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Shanghai", "CST-8"},
    {"Australia/Perth", "AWST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},
};
#define NZONES ((int)(sizeof zones / sizeof zones[0]))

#endif

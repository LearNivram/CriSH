/* regex.h - the regular expression engine shared by grep, sed, awk and [[ =~ ]].
 *
 * macOS ships a POSIX regex library, but it is the BSD one: no \+, no \|,
 * no \b, no \w inside a basic expression, and no -P at all.  Scripts written
 * against GNU grep and GNU sed lean on exactly those, so CriSH brings its own.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_REGEX_H
#define CRISH_REGEX_H

#include <stddef.h>

/* Syntax flavours. */
#define RX_BRE  0 /* POSIX basic, with the GNU extensions \+ \? \| \< \> \b \w */
#define RX_ERE  1 /* POSIX extended */
#define RX_PCRE 2 /* the Perl subset grep -P scripts actually use */

/* Compile flags. */
#define RX_ICASE    0x01 /* case insensitive */
#define RX_NEWLINE  0x02 /* . and [^...] do not match a newline */
#define RX_NOSUB    0x04 /* do not record group positions */
#define RX_MULTILINE 0x08 /* ^ and $ also match at embedded newlines */

#define RX_MAX_GROUPS 32

typedef struct {
	long start;
	long end;
} RxGroup;

typedef struct {
	long start;
	long end;
	int ngroups;
	RxGroup group[RX_MAX_GROUPS];
} RxMatch;

typedef struct Rx Rx;

/* Returns NULL and sets *err (a static string) on a bad pattern. */
Rx *rx_compile(const char *pattern, int mode, int flags, const char **err);
void rx_free(Rx *rx);

/* Leftmost-longest-ish search starting at offset `from`.
 * Returns 1 and fills m on a match, 0 otherwise. */
int rx_search(Rx *rx, const char *text, size_t len, size_t from, RxMatch *m);
/* The whole of text must match. */
int rx_match_full(Rx *rx, const char *text, size_t len);
int rx_ngroups(const Rx *rx);

/* expr-style anchored match: returns the number of characters matched, and
 * hands back the first group's text when the pattern has one. */
int rx_matches_anchored(const char *pattern, const char *text, char **group);

/* Convenience: does the pattern occur anywhere in a NUL terminated string? */
int rx_matches(const char *pattern, const char *text, int mode, int flags);

#endif /* CRISH_REGEX_H */

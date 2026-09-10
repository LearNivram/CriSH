/* color.c - themes, and the decision of whether to use colour at all.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "color.h"
#include "shell.h"

/* The escape body only, without the leading \033[ and trailing m, so that a
 * theme is readable and an override from CRISH_COLORS is the same shape. */
typedef struct {
	const char *name; /* how it is written in CRISH_COLORS */
	const char *bold; /* the default theme */
	const char *muted;
	const char *mono;
} RoleDef;

/* Kept in the order of the ColorRole enum. */
static const RoleDef roles[C_ROLE_COUNT] = {
	{ "reset",    "0",      "0",        "0"    },

	{ "cmd",      "1;32",   "38;5;71",  "1"    },
	{ "missing",  "1;31",   "38;5;167", "4"    },
	{ "builtin",  "1;36",   "38;5;73",  "1"    },
	{ "gnu",      "1;36",   "38;5;73",  "1"    },
	{ "function", "1;35",   "38;5;139", "1"    },
	{ "alias",    "1;35",   "38;5;139", "1"    },
	{ "keyword",  "1;34",   "38;5;110", "1"    },
	{ "string",   "33",     "38;5;180", ""     },
	{ "escape",   "1;33",   "38;5;186", "1"    },
	{ "var",      "36",     "38;5;109", ""     },
	{ "subst",    "1;36",   "38;5;109", "1"    },
	{ "operator", "35",     "38;5;175", "1"    },
	{ "redir",    "34",     "38;5;110", "1"    },
	{ "comment",  "90",     "38;5;242", "2"    },
	{ "flag",     "2",      "38;5;245", "2"    },
	{ "path",     "4",      "4",        "4"    },

	{ "dir",      "1;34",   "38;5;110", "1"    },
	{ "exec",     "1;32",   "38;5;71",  "1"    },
	{ "link",     "1;36",   "38;5;73",  "4"    },
	{ "fifo",     "33",     "38;5;180", ""     },
	{ "sock",     "1;35",   "38;5;139", ""     },
	{ "block",    "1;33",   "38;5;186", ""     },
	{ "char",     "1;33",   "38;5;186", ""     },
	{ "file",     "0",      "0",        "0"    },

	{ "error",    "1;31",   "38;5;167", "1"    },
	{ "warn",     "1;33",   "38;5;179", "1"    },
	{ "hint",     "90",     "38;5;242", "2"    },

	{ "match",    "1;31",   "1;38;5;167", "1;4" },
	{ "filename", "35",     "38;5;139", ""     },
	{ "lineno",   "32",     "38;5;71",  ""     },
	{ "sep",      "36",     "38;5;73",  "2"    },

	{ "user",     "1;32",   "38;5;71",  "1"    },
	{ "host",     "32",     "38;5;71",  ""     },
	{ "cwd",      "1;34",   "38;5;110", "1"    },
	{ "git",      "33",     "38;5;179", ""     },
	{ "ok",       "1;32",   "38;5;71",  "1"    },
	{ "fail",     "1;31",   "38;5;167", "1"    },
};

static const char *const theme_names[] = { "bold", "muted", "mono", NULL };

static char *current[C_ROLE_COUNT]; /* the full escape, or NULL for none */
static const char *theme = "bold";
static int enabled;
static int requested = COLOR_AUTO;

static void clear_current(void)
{
	int i;

	for (i = 0; i < C_ROLE_COUNT; i++) {
		free(current[i]);
		current[i] = NULL;
	}
}

static const char *theme_body(const RoleDef *r, const char *name)
{
	if (strcmp(name, "muted") == 0)
		return r->muted;
	if (strcmp(name, "mono") == 0)
		return r->mono;
	return r->bold;
}

static void build_theme(const char *name)
{
	int i;

	clear_current();
	for (i = 0; i < C_ROLE_COUNT; i++) {
		const char *body = theme_body(&roles[i], name);

		if (!body || !*body)
			continue;
		current[i] = xasprintf("\033[%sm", body);
	}
	/* reset is always the plain one, whatever a theme says */
	free(current[C_RESET]);
	current[C_RESET] = xstrdup("\033[0m");
}

const char *color_role_name(ColorRole role)
{
	if (role < 0 || role >= C_ROLE_COUNT)
		return NULL;
	return roles[role].name;
}

int color_role_by_name(const char *name)
{
	int i;

	for (i = 0; i < C_ROLE_COUNT; i++)
		if (strcmp(roles[i].name, name) == 0)
			return i;
	return -1;
}

void color_apply_overrides(const char *spec)
{
	char *copy, *save = NULL, *item;

	if (!spec || !*spec)
		return;
	copy = xstrdup(spec);
	for (item = strtok_r(copy, ":", &save); item; item = strtok_r(NULL, ":", &save)) {
		char *eq = strchr(item, '=');
		int role;

		if (!eq)
			continue;
		*eq = '\0';
		role = color_role_by_name(str_trim(item));
		if (role < 0)
			continue;
		free(current[role]);
		if (*(eq + 1))
			current[role] = xasprintf("\033[%sm", eq + 1);
		else
			current[role] = NULL;
	}
	free(copy);
}

int color_set_theme(const char *name)
{
	int i;

	for (i = 0; theme_names[i]; i++) {
		if (strcmp(theme_names[i], name) == 0) {
			theme = theme_names[i];
			build_theme(theme);
			color_apply_overrides(var_get("CRISH_COLORS"));
			return 1;
		}
	}
	return 0;
}

const char *color_theme_name(void)
{
	return theme;
}

const char *const *color_theme_names(size_t *count)
{
	size_t n = 0;

	while (theme_names[n])
		n++;
	if (count)
		*count = n;
	return theme_names;
}

/* The whole decision, in the order the plan lays out. */
static int decide(void)
{
	const char *term;

	if (requested == COLOR_NEVER)
		return 0;
	/* no-color.org: any value, even empty, means off */
	if (getenv("NO_COLOR"))
		return 0;
	if (requested == COLOR_ALWAYS)
		return 1;
	if (getenv("CLICOLOR_FORCE"))
		return 1;

	term = var_get("TERM");
	if (!term)
		term = getenv("TERM");
	if (!term || !*term || strcmp(term, "dumb") == 0)
		return 0;

	/* Colour is for a person looking at a terminal.  A pipe gets none, which
	 * is also why the test suite never sees an escape. */
	return isatty(1) || isatty(2);
}

void color_refresh(void)
{
	const char *want = var_get("CRISH_THEME");

	enabled = decide();
	if (want && *want && strcmp(want, theme) != 0) {
		if (!color_set_theme(want))
			build_theme(theme);
	} else {
		build_theme(theme);
		color_apply_overrides(var_get("CRISH_COLORS"));
	}
}

void color_init(int when)
{
	requested = when;
	color_refresh();
}

int color_enabled(void)
{
	return enabled;
}

const char *color(ColorRole role)
{
	if (!enabled || role < 0 || role >= C_ROLE_COUNT || !current[role])
		return "";
	return current[role];
}

const char *color_off(void)
{
	return enabled ? "\033[0m" : "";
}

void color_putn(Buf *buf, ColorRole role, const char *text, size_t len)
{
	const char *on = color(role);

	if (*on)
		buf_puts(buf, on);
	buf_put(buf, text, len);
	if (*on)
		buf_puts(buf, color_off());
}

void color_put(Buf *buf, ColorRole role, const char *text)
{
	color_putn(buf, role, text, strlen(text));
}

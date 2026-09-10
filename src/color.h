/* color.h - one place that decides what anything coloured looks like.
 *
 * Nothing in CriSH writes an escape sequence directly.  Everything asks for a
 * role, and gets either the escape for it or an empty string when colour is
 * off, so that turning colour off can never leave half a sequence behind.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_COLOR_H
#define CRISH_COLOR_H

#include <stddef.h>

#include "util.h"

typedef enum {
	C_RESET = 0,

	/* the line editor */
	C_CMD_OK,      /* a command name that resolves */
	C_CMD_MISSING, /* one that does not: the point of the whole feature */
	C_BUILTIN,
	C_GNU,
	C_FUNCTION,
	C_ALIAS,
	C_KEYWORD,
	C_STRING,
	C_ESCAPE,
	C_VAR,
	C_SUBST,
	C_OPERATOR,
	C_REDIR,
	C_COMMENT,
	C_FLAG,
	C_PATH_EXISTS,

	/* file kinds, shared by the completion list and ls */
	C_DIR,
	C_EXEC,
	C_LINK,
	C_FIFO,
	C_SOCK,
	C_BLOCK,
	C_CHAR,
	C_FILE,

	/* diagnostics */
	C_ERROR,
	C_WARN,
	C_HINT,

	/* grep */
	C_MATCH,
	C_FILENAME,
	C_LINENO,
	C_SEP,

	/* the prompt */
	C_PROMPT_USER,
	C_PROMPT_HOST,
	C_PROMPT_PATH,
	C_PROMPT_GIT,
	C_PROMPT_OK,
	C_PROMPT_FAIL,

	C_ROLE_COUNT
} ColorRole;

/* When colour applies at all. */
#define COLOR_AUTO   (-1)
#define COLOR_NEVER  0
#define COLOR_ALWAYS 1

/* Work out whether colour is on, from the flag, the environment and whether
 * the terminal is real.  Call once at startup; `when` comes from --color. */
void color_init(int when);
int color_enabled(void);
/* Recompute after something that could change the answer, such as an
 * assignment to TERM or a shopt. */
void color_refresh(void);

/* The escape for a role, or "" when colour is off.  Never NULL. */
const char *color(ColorRole role);
/* The escape for a role whatever the global decision says, for a tool that
 * was told --color=always on its own. */
const char *color_force(ColorRole role);
/* The reset escape, or "" when colour is off. */
const char *color_off(void);

/* Append `text` wrapped in `role` to buf; plain text when colour is off. */
void color_put(Buf *buf, ColorRole role, const char *text);
void color_putn(Buf *buf, ColorRole role, const char *text, size_t len);

/* Themes: "bold" (the default), "muted", "mono". */
int color_set_theme(const char *name);
const char *color_theme_name(void);
const char *const *color_theme_names(size_t *count);

/* Role names as they appear in CRISH_COLORS, for `theme --preview` and for
 * parsing.  Returns NULL for an unknown role. */
const char *color_role_name(ColorRole role);
int color_role_by_name(const char *name);

/* CRISH_COLORS="cmd=32:missing=1;31" style overrides, applied on top of the
 * current theme.  Called by color_init and whenever CRISH_COLORS changes. */
void color_apply_overrides(const char *spec);

#endif /* CRISH_COLOR_H */

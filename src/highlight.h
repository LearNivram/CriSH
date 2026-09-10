/* highlight.h - colouring a line while it is still being typed.
 *
 * This is deliberately not the real parser.  It has to cope with a line that
 * is half written, must never report an error, and must never run anything.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_HIGHLIGHT_H
#define CRISH_HIGHLIGHT_H

#include <stddef.h>

#include "color.h"

typedef struct {
	size_t start;
	size_t len;
	ColorRole role;
} Span;

/* Scan `line` and hand back the spans worth colouring, in order and not
 * overlapping.  The caller frees *out.  Returns how many there are. */
size_t highlight_line(const char *line, size_t len, Span **out);

/* The whole line with escapes in it, ready to print.  Caller frees. */
char *highlight_render(const char *line, size_t len);
/* Only the bytes in [from, to), but coloured as they would be in the whole
 * line, so a scrolled view reopens whatever colour was in force. */
char *highlight_render_range(const char *line, size_t len, size_t from, size_t to);

/* What a word in command position resolves to, which is the whole point:
 * C_CMD_MISSING when nothing by that name can be run. */
ColorRole highlight_command_role(const char *word);

/* Anything that could change the answers: a new function or alias, a changed
 * PATH, `hash -r`.  Cheap to call. */
void highlight_invalidate(void);

#endif /* CRISH_HIGHLIGHT_H */

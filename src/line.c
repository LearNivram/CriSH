/* line.c - the interactive line editor.
 *
 * Raw mode, emacs key bindings, history navigation, reverse search and
 * tab completion.  No readline, no terminfo, no dependencies.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "color.h"
#include "highlight.h"
#include "shell.h"

void complete_line(const char *line, size_t point, Vec *out, Vec *kinds,
		   size_t *replace_from);
ColorRole complete_kind_color(char kind);

static struct termios orig_termios;
static int raw_active;

void line_init(void)
{
	tcgetattr(0, &orig_termios);
}

void line_cleanup(void)
{
	if (raw_active) {
		tcsetattr(0, TCSAFLUSH, &orig_termios);
		raw_active = 0;
	}
}

static int enter_raw(void)
{
	struct termios raw;

	if (!isatty(0))
		return 0;
	if (tcgetattr(0, &orig_termios) < 0)
		return 0;
	raw = orig_termios;
	raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(unsigned long)(OPOST);
	raw.c_cflag |= CS8;
	raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSAFLUSH, &raw) < 0)
		return 0;
	raw_active = 1;
	return 1;
}

static void leave_raw(void)
{
	if (raw_active) {
		tcsetattr(0, TCSAFLUSH, &orig_termios);
		raw_active = 0;
	}
}

static int term_width(void)
{
	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

/* Display columns of a byte range, which is not the same as its length once
 * anyone types a character outside ASCII. */
static size_t columns_of(const char *s, size_t len)
{
	size_t i, cols = 0;

	for (i = 0; i < len; i++)
		if ((s[i] & 0xc0) != 0x80)
			cols++;
	return cols;
}

/* Printable width of a prompt, ignoring escape sequences. */
static size_t visible_width(const char *s)
{
	size_t w = 0;

	while (*s) {
		if (*s == '\x1b') {
			while (*s && *s != 'm')
				s++;
			if (*s)
				s++;
			continue;
		}
		if ((*s & 0xc0) != 0x80)
			w++;
		s++;
	}
	return w;
}

typedef struct {
	Buf buf;
	size_t point;
	const char *prompt;
	size_t prompt_width;
	size_t history_index;
	char *saved_current;
} Editor;

static void out(const char *s, size_t n)
{
	ssize_t ignored = write(1, s, n);

	(void)ignored;
}

static void outs(const char *s)
{
	out(s, strlen(s));
}

static void refresh(Editor *ed)
{
	Buf o;
	size_t width = (size_t)term_width();
	size_t start = 0;
	size_t shown;
	size_t cursor_col;
	const char *text = ed->buf.b ? ed->buf.b : "";
	size_t point_cols = columns_of(text, ed->point);
	size_t total_cols = columns_of(text, ed->buf.len);

	/* Scroll horizontally when prompt plus line no longer fit. */
	if (ed->prompt_width + total_cols >= width) {
		size_t cursor = ed->prompt_width + point_cols;

		if (cursor >= width - 1)
			start = ed->point - (ed->point > width ? width - 2 : ed->point);
	}

	buf_init(&o);
	buf_puts(&o, "\r\x1b[K");
	if (start == 0) {
		buf_puts(&o, ed->prompt);
		shown = ed->buf.len;
		while (ed->prompt_width + columns_of(text, shown) > width - 1 && shown)
			shown--;
		cursor_col = ed->prompt_width + point_cols;
	} else {
		shown = ed->buf.len;
		while (columns_of(text + start, shown - start) > width - 1 && shown > start)
			shown--;
		cursor_col = columns_of(text + start, ed->point - start);
	}
	{
		char *painted = sh.shopt.syntax_highlight && color_enabled()
					? highlight_render_range(text, ed->buf.len, start,
								 shown)
					: xstrndup(text + start, shown - start);

		buf_puts(&o, painted);
		free(painted);
	}
	buf_puts(&o, "\r");
	if (cursor_col)
		buf_printf(&o, "\x1b[%zuC", cursor_col);
	out(o.b, o.len);
	buf_free(&o);
}

static void insert_text(Editor *ed, const char *text, size_t n)
{
	buf_reserve(&ed->buf, n);
	memmove(ed->buf.b + ed->point + n, ed->buf.b + ed->point, ed->buf.len - ed->point);
	memcpy(ed->buf.b + ed->point, text, n);
	ed->buf.len += n;
	ed->point += n;
	ed->buf.b[ed->buf.len] = '\0';
}

static void delete_range(Editor *ed, size_t from, size_t to)
{
	if (from >= to || to > ed->buf.len)
		return;
	memmove(ed->buf.b + from, ed->buf.b + to, ed->buf.len - to);
	ed->buf.len -= to - from;
	ed->buf.b[ed->buf.len] = '\0';
	if (ed->point > ed->buf.len)
		ed->point = ed->buf.len;
	else if (ed->point > from && ed->point <= to)
		ed->point = from;
	else if (ed->point > to)
		ed->point -= to - from;
}

static size_t word_left(Editor *ed)
{
	size_t i = ed->point;

	while (i && isspace((unsigned char)ed->buf.b[i - 1]))
		i--;
	while (i && !isspace((unsigned char)ed->buf.b[i - 1]))
		i--;
	return i;
}

static size_t word_right(Editor *ed)
{
	size_t i = ed->point;

	while (i < ed->buf.len && isspace((unsigned char)ed->buf.b[i]))
		i++;
	while (i < ed->buf.len && !isspace((unsigned char)ed->buf.b[i]))
		i++;
	return i;
}

static void set_line(Editor *ed, const char *text)
{
	buf_reset(&ed->buf);
	buf_puts(&ed->buf, text ? text : "");
	ed->point = ed->buf.len;
}

static void history_prev(Editor *ed)
{
	size_t n = history_count();

	if (!n || ed->history_index == 0)
		return;
	if (ed->history_index == n) {
		free(ed->saved_current);
		ed->saved_current = xstrndup(ed->buf.b ? ed->buf.b : "", ed->buf.len);
	}
	ed->history_index--;
	set_line(ed, history_get(ed->history_index));
}

static void history_next(Editor *ed)
{
	size_t n = history_count();

	if (ed->history_index >= n)
		return;
	ed->history_index++;
	if (ed->history_index == n)
		set_line(ed, ed->saved_current ? ed->saved_current : "");
	else
		set_line(ed, history_get(ed->history_index));
}

/* Ctrl-R: incremental reverse search. */
static void reverse_search(Editor *ed)
{
	Buf needle;
	long found = -1;

	buf_init(&needle);
	for (;;) {
		char c;
		Buf line;

		buf_init(&line);
		buf_printf(&line, "\r\x1b[K(reverse-i-search)`%s': %s",
			   needle.b ? needle.b : "", found >= 0 ? history_get((size_t)found) : "");
		out(line.b, line.len);
		buf_free(&line);

		if (read(0, &c, 1) != 1)
			break;
		if (c == 7 || c == 27) { /* Ctrl-G or Escape */
			buf_free(&needle);
			refresh(ed);
			return;
		}
		if (c == '\r' || c == '\n') {
			if (found >= 0)
				set_line(ed, history_get((size_t)found));
			break;
		}
		if (c == 127 || c == 8) {
			if (needle.len)
				needle.b[--needle.len] = '\0';
		} else if (c == 18) { /* Ctrl-R again: keep searching further back */
			long start = found > 0 ? found - 1 : -1;
			long i;
			for (i = start; i >= 0; i--)
				if (strstr(history_get((size_t)i), needle.b ? needle.b : "")) {
					found = i;
					break;
				}
			continue;
		} else if (isprint((unsigned char)c)) {
			buf_putc(&needle, c);
		} else {
			continue;
		}
		{
			long i;
			found = -1;
			for (i = (long)history_count() - 1; i >= 0; i--) {
				if (strstr(history_get((size_t)i), needle.b ? needle.b : "")) {
					found = i;
					break;
				}
			}
		}
	}
	buf_free(&needle);
	if (found >= 0)
		set_line(ed, history_get((size_t)found));
	refresh(ed);
}

static void do_complete(Editor *ed)
{
	Vec matches, kinds;
	size_t replace_from = ed->point;

	vec_init(&matches);
	vec_init(&kinds);
	complete_line(ed->buf.b ? ed->buf.b : "", ed->point, &matches, &kinds,
		      &replace_from);
	if (!matches.len) {
		vec_free(&matches);
		vec_free(&kinds);
		return;
	}
	if (matches.len == 1) {
		delete_range(ed, replace_from, ed->point);
		ed->point = replace_from;
		insert_text(ed, matches.v[0], strlen(matches.v[0]));
		vec_free(&matches);
		vec_free(&kinds);
		refresh(ed);
		return;
	}
	/* insert the longest common prefix, then show the candidates */
	{
		size_t common = strlen(matches.v[0]);
		size_t i;

		for (i = 1; i < matches.len; i++) {
			size_t k = 0;
			while (k < common && matches.v[i][k] == matches.v[0][k])
				k++;
			common = k;
		}
		if (common > ed->point - replace_from) {
			delete_range(ed, replace_from, ed->point);
			ed->point = replace_from;
			insert_text(ed, matches.v[0], common);
			vec_free(&matches);
			vec_free(&kinds);
			refresh(ed);
			return;
		}
	}
	outs("\r\n");
	{
		size_t i;
		size_t width = (size_t)term_width();
		size_t longest = 0;
		size_t cols;
		Buf o;

		for (i = 0; i < matches.len; i++)
			if (strlen(matches.v[i]) > longest)
				longest = strlen(matches.v[i]);
		cols = width / (longest + 2);
		if (cols < 1)
			cols = 1;

		buf_init(&o);
		for (i = 0; i < matches.len; i++) {
			char kind = i < kinds.len ? kinds.v[i][0] : '-';
			size_t pad = longest + 2 - strlen(matches.v[i]);

			color_put(&o, complete_kind_color(kind), matches.v[i]);
			while (pad--)
				buf_putc(&o, ' ');
			if ((i + 1) % cols == 0) {
				buf_puts(&o, "\r\n");
				out(o.b, o.len);
				buf_reset(&o);
			}
		}
		if (matches.len % cols)
			buf_puts(&o, "\r\n");
		out(o.b, o.len);
		buf_free(&o);
	}
	vec_free(&matches);
	vec_free(&kinds);
	refresh(ed);
}

char *line_read(const char *prompt, const char *prompt2)
{
	Editor ed;
	char *result = NULL;

	(void)prompt2;

	if (!enter_raw()) {
		/* not a terminal: plain line input */
		Buf b;
		int c;

		fputs(prompt, stdout);
		fflush(stdout);
		buf_init(&b);
		while ((c = fgetc(stdin)) != EOF && c != '\n')
			buf_putc(&b, c);
		if (c == EOF && !b.len) {
			buf_free(&b);
			return NULL;
		}
		return buf_take(&b);
	}

	memset(&ed, 0, sizeof ed);
	buf_init(&ed.buf);
	ed.prompt = prompt;
	ed.prompt_width = visible_width(prompt);
	ed.history_index = history_count();
	refresh(&ed);

	for (;;) {
		char c;
		ssize_t n = read(0, &c, 1);

		if (n < 0 && errno == EINTR) {
			if (trap_pending) {
				leave_raw();
				trap_run_pending();
				enter_raw();
				refresh(&ed);
			}
			continue;
		}
		if (n <= 0) {
			if (!ed.buf.len) {
				leave_raw();
				buf_free(&ed.buf);
				free(ed.saved_current);
				return NULL;
			}
			break;
		}

		switch (c) {
		case '\r':
		case '\n':
			outs("\r\n");
			result = xstrndup(ed.buf.b ? ed.buf.b : "", ed.buf.len);
			leave_raw();
			buf_free(&ed.buf);
			free(ed.saved_current);
			return result;
		case 3: /* Ctrl-C */
			outs("^C\r\n");
			buf_reset(&ed.buf);
			ed.point = 0;
			leave_raw();
			buf_free(&ed.buf);
			free(ed.saved_current);
			return xstrdup("");
		case 4: /* Ctrl-D */
			if (!ed.buf.len) {
				leave_raw();
				buf_free(&ed.buf);
				free(ed.saved_current);
				return NULL;
			}
			delete_range(&ed, ed.point, ed.point + 1);
			break;
		case 1: /* Ctrl-A */
			ed.point = 0;
			break;
		case 5: /* Ctrl-E */
			ed.point = ed.buf.len;
			break;
		case 2: /* Ctrl-B */
			if (ed.point)
				ed.point--;
			break;
		case 6: /* Ctrl-F */
			if (ed.point < ed.buf.len)
				ed.point++;
			break;
		case 11: /* Ctrl-K */
			delete_range(&ed, ed.point, ed.buf.len);
			break;
		case 21: /* Ctrl-U */
			delete_range(&ed, 0, ed.point);
			break;
		case 23: { /* Ctrl-W */
			size_t from = word_left(&ed);
			delete_range(&ed, from, ed.point);
			break;
		}
		case 12: /* Ctrl-L */
			outs("\x1b[H\x1b[2J");
			break;
		case 16: /* Ctrl-P */
			history_prev(&ed);
			break;
		case 14: /* Ctrl-N */
			history_next(&ed);
			break;
		case 18: /* Ctrl-R */
			reverse_search(&ed);
			continue;
		case '\t':
			do_complete(&ed);
			continue;
		case 127:
		case 8:
			if (ed.point)
				delete_range(&ed, ed.point - 1, ed.point);
			break;
		case 27: { /* escape sequences */
			char seq[3];

			if (read(0, &seq[0], 1) != 1)
				break;
			if (seq[0] == 'b') {
				ed.point = word_left(&ed);
				break;
			}
			if (seq[0] == 'f') {
				ed.point = word_right(&ed);
				break;
			}
			if (seq[0] == 'd') {
				delete_range(&ed, ed.point, word_right(&ed));
				break;
			}
			if (read(0, &seq[1], 1) != 1)
				break;
			if (seq[0] == '[') {
				if (seq[1] >= '0' && seq[1] <= '9') {
					if (read(0, &seq[2], 1) != 1)
						break;
					if (seq[2] == '~') {
						if (seq[1] == '3')
							delete_range(&ed, ed.point, ed.point + 1);
						else if (seq[1] == '1' || seq[1] == '7')
							ed.point = 0;
						else if (seq[1] == '4' || seq[1] == '8')
							ed.point = ed.buf.len;
					}
					break;
				}
				switch (seq[1]) {
				case 'A': history_prev(&ed); break;
				case 'B': history_next(&ed); break;
				case 'C':
					if (ed.point < ed.buf.len)
						ed.point++;
					break;
				case 'D':
					if (ed.point)
						ed.point--;
					break;
				case 'H': ed.point = 0; break;
				case 'F': ed.point = ed.buf.len; break;
				default: break;
				}
			} else if (seq[0] == 'O') {
				if (seq[1] == 'H')
					ed.point = 0;
				else if (seq[1] == 'F')
					ed.point = ed.buf.len;
			}
			break;
		}
		default:
			if ((unsigned char)c >= 32)
				insert_text(&ed, &c, 1);
			break;
		}
		refresh(&ed);
	}

	result = xstrndup(ed.buf.b ? ed.buf.b : "", ed.buf.len);
	leave_raw();
	buf_free(&ed.buf);
	free(ed.saved_current);
	return result;
}

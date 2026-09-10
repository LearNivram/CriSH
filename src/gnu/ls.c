/* ls.c - a GNU-flavoured ls.
 *
 * The BSD ls on a Mac has no --color, no --group-directories-first and no
 * --time-style, which is exactly the set of flags a Linux dotfile or script
 * reaches for.  This one has them, reads LS_COLORS, and falls back to CriSH's
 * own theme when LS_COLORS is not set.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../color.h"
#include "gnu.h"

/* ------------------------------------------------------------------ state */

enum { SORT_NAME, SORT_TIME, SORT_SIZE, SORT_EXT, SORT_NONE, SORT_VERSION };
enum { TIME_MTIME, TIME_ATIME, TIME_CTIME };

typedef struct {
	int long_format;
	int all;          /* -a */
	int almost_all;   /* -A */
	int one_per_line; /* -1 */
	int directory;    /* -d */
	int classify;     /* -F */
	int slash_dirs;   /* -p */
	int human;        /* -h */
	int inode;        /* -i */
	int numeric_ids;  /* -n */
	int quote_names;  /* -Q */
	int recursive;    /* -R */
	int reverse;      /* -r */
	int sort;
	int time_field;
	int group_dirs_first;
	int colour;    /* COLOR_AUTO / ALWAYS / NEVER */
	int colour_on;
	int show_group;
	const char *time_style;
} Ls;

typedef struct {
	char *name;    /* what to print */
	char *path;    /* how to reach it */
	struct stat st;
	int have_stat;
	int is_link;
	char *link_target;
} Entry;

static Ls ls;

/* -------------------------------------------------------------- LS_COLORS */

typedef struct SuffixRule {
	char *suffix;
	char *body;
	struct SuffixRule *next;
} SuffixRule;

static char *ls_key[16]; /* di ln ex fi pi so bd cd or mi */
static SuffixRule *ls_suffixes;
static char *ls_colors_seen; /* the LS_COLORS the table was built from */

enum { K_DI, K_LN, K_EX, K_FI, K_PI, K_SO, K_BD, K_CD, K_OR, K_MI, K_COUNT };

static const char *const key_names[K_COUNT] = { "di", "ln", "ex", "fi", "pi",
						"so", "bd", "cd", "or", "mi" };

static void read_ls_colors(void)
{
	const char *spec;
	char *copy, *save = NULL, *item;

	spec = var_get("LS_COLORS");
	if (!spec)
		spec = "";
	/* the shell stays alive between calls, so rebuild when it changed */
	if (ls_colors_seen && strcmp(ls_colors_seen, spec) == 0)
		return;
	{
		int i;

		for (i = 0; i < K_COUNT; i++) {
			free(ls_key[i]);
			ls_key[i] = NULL;
		}
		while (ls_suffixes) {
			SuffixRule *r = ls_suffixes;

			ls_suffixes = r->next;
			free(r->suffix);
			free(r->body);
			free(r);
		}
	}
	free(ls_colors_seen);
	ls_colors_seen = xstrdup(spec);
	if (!*spec)
		return;
	copy = xstrdup(spec);
	for (item = strtok_r(copy, ":", &save); item; item = strtok_r(NULL, ":", &save)) {
		char *eq = strchr(item, '=');
		int i;

		if (!eq)
			continue;
		*eq = '\0';
		if (item[0] == '*') {
			SuffixRule *r = xcalloc(1, sizeof *r);

			r->suffix = xstrdup(item + 1);
			r->body = xstrdup(eq + 1);
			r->next = ls_suffixes;
			ls_suffixes = r;
			continue;
		}
		for (i = 0; i < K_COUNT; i++) {
			if (strcmp(item, key_names[i]) == 0) {
				free(ls_key[i]);
				ls_key[i] = xstrdup(eq + 1);
				break;
			}
		}
	}
	free(copy);
}

/* The escape for an entry: LS_COLORS if it has an opinion, else the theme. */
static const char *entry_color(const Entry *e)
{
	static char buf[64];
	mode_t m = e->have_stat ? e->st.st_mode : 0;
	int key = -1;
	ColorRole role = C_FILE;

	if (!ls.colour_on)
		return "";
	read_ls_colors();

	if (e->is_link) {
		struct stat target;

		if (stat(e->path, &target) != 0) {
			key = K_OR; /* orphan */
			role = C_CMD_MISSING;
		} else {
			key = K_LN;
			role = C_LINK;
		}
	} else if (S_ISDIR(m)) {
		key = K_DI;
		role = C_DIR;
	} else if (S_ISFIFO(m)) {
		key = K_PI;
		role = C_FIFO;
	} else if (S_ISSOCK(m)) {
		key = K_SO;
		role = C_SOCK;
	} else if (S_ISBLK(m)) {
		key = K_BD;
		role = C_BLOCK;
	} else if (S_ISCHR(m)) {
		key = K_CD;
		role = C_CHAR;
	} else if (m & (S_IXUSR | S_IXGRP | S_IXOTH)) {
		key = K_EX;
		role = C_EXEC;
	} else {
		SuffixRule *r;

		for (r = ls_suffixes; r; r = r->next) {
			if (str_suffix(e->name, r->suffix)) {
				snprintf(buf, sizeof buf, "\033[%sm", r->body);
				return buf;
			}
		}
		key = K_FI;
		role = C_FILE;
	}
	if (key >= 0 && ls_key[key]) {
		snprintf(buf, sizeof buf, "\033[%sm", ls_key[key]);
		return buf;
	}
	/* --color=always is this tool's own decision, not the shell's */
	return color_force(role);
}

/* ------------------------------------------------------------------ sorting */

static int version_cmp_ls(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			long x = strtol(a, (char **)&a, 10);
			long y = strtol(b, (char **)&b, 10);

			if (x != y)
				return x < y ? -1 : 1;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++;
		b++;
	}
	return *a ? 1 : (*b ? -1 : 0);
}

static time_t entry_time(const Entry *e)
{
	if (!e->have_stat)
		return 0;
	if (ls.time_field == TIME_ATIME)
		return e->st.st_atime;
	if (ls.time_field == TIME_CTIME)
		return e->st.st_ctime;
	return e->st.st_mtime;
}

static const char *extension_of(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot : "";
}

static int compare_entries(const void *va, const void *vb)
{
	const Entry *a = va, *b = vb;
	int r = 0;

	if (ls.group_dirs_first) {
		int da = a->have_stat && S_ISDIR(a->st.st_mode);
		int db = b->have_stat && S_ISDIR(b->st.st_mode);

		if (da != db)
			return da ? -1 : 1;
	}
	switch (ls.sort) {
	case SORT_NONE:
		return 0;
	case SORT_TIME: {
		time_t ta = entry_time(a), tb = entry_time(b);

		r = ta == tb ? strcmp(a->name, b->name) : (ta > tb ? -1 : 1);
		break;
	}
	case SORT_SIZE: {
		off_t sa = a->have_stat ? a->st.st_size : 0;
		off_t sb = b->have_stat ? b->st.st_size : 0;

		r = sa == sb ? strcmp(a->name, b->name) : (sa > sb ? -1 : 1);
		break;
	}
	case SORT_EXT:
		r = strcmp(extension_of(a->name), extension_of(b->name));
		if (!r)
			r = strcmp(a->name, b->name);
		break;
	case SORT_VERSION:
		r = version_cmp_ls(a->name, b->name);
		break;
	default:
		r = strcmp(a->name, b->name);
		break;
	}
	return ls.reverse ? -r : r;
}

/* ---------------------------------------------------------------- printing */

static void mode_string(mode_t m, int is_link, char *out)
{
	const char *rwx = "rwxrwxrwx";
	int i;

	out[0] = is_link	? 'l'
		 : S_ISDIR(m)	? 'd'
		 : S_ISCHR(m)	? 'c'
		 : S_ISBLK(m)	? 'b'
		 : S_ISFIFO(m)	? 'p'
		 : S_ISSOCK(m)	? 's'
				: '-';
	for (i = 0; i < 9; i++)
		out[i + 1] = (m & (1 << (8 - i))) ? rwx[i] : '-';
	if (m & S_ISUID)
		out[3] = (m & S_IXUSR) ? 's' : 'S';
	if (m & S_ISGID)
		out[6] = (m & S_IXGRP) ? 's' : 'S';
	if (m & S_ISVTX)
		out[9] = (m & S_IXOTH) ? 't' : 'T';
	out[10] = '\0';
}

static char *human_size(off_t n)
{
	static const char units[] = "BKMGTPE";
	double v = (double)n;
	int u = 0;
	char buf[32];

	while (v >= 1024 && u < 6) {
		v /= 1024;
		u++;
	}
	if (!u)
		snprintf(buf, sizeof buf, "%lld", (long long)n);
	else if (v < 10)
		snprintf(buf, sizeof buf, "%.1f%c", v, units[u]);
	else
		snprintf(buf, sizeof buf, "%.0f%c", v, units[u]);
	return xstrdup(buf);
}

static char *format_time(time_t t)
{
	struct tm tm;
	char buf[64];
	time_t now = time(NULL);

	localtime_r(&t, &tm);
	if (ls.time_style) {
		if (ls.time_style[0] == '+') {
			strftime(buf, sizeof buf, ls.time_style + 1, &tm);
			return xstrdup(buf);
		}
		if (strcmp(ls.time_style, "full-iso") == 0) {
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S.000000000 %z", &tm);
			return xstrdup(buf);
		}
		if (strcmp(ls.time_style, "long-iso") == 0) {
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
			return xstrdup(buf);
		}
		if (strcmp(ls.time_style, "iso") == 0) {
			strftime(buf, sizeof buf, "%m-%d %H:%M", &tm);
			return xstrdup(buf);
		}
	}
	/* GNU's default: the year instead of the clock once it is old enough */
	if (now - t > 15552000 || t - now > 3600)
		strftime(buf, sizeof buf, "%b %e  %Y", &tm);
	else
		strftime(buf, sizeof buf, "%b %e %H:%M", &tm);
	return xstrdup(buf);
}

static char classify_char(const Entry *e)
{
	mode_t m = e->have_stat ? e->st.st_mode : 0;

	if (S_ISDIR(m))
		return '/';
	if (ls.slash_dirs && !ls.classify)
		return 0;
	if (e->is_link)
		return '@';
	if (S_ISFIFO(m))
		return '|';
	if (S_ISSOCK(m))
		return '=';
	if (m & (S_IXUSR | S_IXGRP | S_IXOTH))
		return '*';
	return 0;
}

/* The visible text of an entry, without colour, so widths line up. */
static char *entry_label(const Entry *e)
{
	Buf b;
	char cls = (ls.classify || ls.slash_dirs) ? classify_char(e) : 0;

	buf_init(&b);
	if (ls.quote_names)
		buf_putc(&b, '"');
	buf_puts(&b, e->name);
	if (ls.quote_names)
		buf_putc(&b, '"');
	if (cls)
		buf_putc(&b, cls);
	return buf_take(&b);
}

static void print_name(const Entry *e)
{
	const char *on = entry_color(e);
	char *label = entry_label(e);

	if (*on)
		fputs(on, stdout);
	fputs(label, stdout);
	if (*on)
		fputs("\033[0m", stdout);
	free(label);
}

static int term_columns(void)
{
	struct winsize ws;
	const char *env = var_get("COLUMNS");

	if (env && *env) {
		int n = atoi(env);

		if (n > 0)
			return n;
	}
	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

static void print_long(Entry *entries, size_t n, int show_total)
{
	size_t i;
	int w_links = 1, w_user = 1, w_group = 1, w_size = 1, w_inode = 1;
	char **sizes = xcalloc(n ? n : 1, sizeof *sizes);
	long long total = 0;

	for (i = 0; i < n; i++) {
		char buf[64];
		struct passwd *pw;
		struct group *gr;
		int len;

		if (!entries[i].have_stat)
			continue;
		total += entries[i].st.st_blocks;

		len = snprintf(buf, sizeof buf, "%u", (unsigned)entries[i].st.st_nlink);
		if (len > w_links)
			w_links = len;
		len = snprintf(buf, sizeof buf, "%llu",
			       (unsigned long long)entries[i].st.st_ino);
		if (len > w_inode)
			w_inode = len;

		if (ls.numeric_ids || !(pw = getpwuid(entries[i].st.st_uid)))
			len = snprintf(buf, sizeof buf, "%u", entries[i].st.st_uid);
		else
			len = (int)strlen(pw->pw_name);
		if (len > w_user)
			w_user = len;

		if (ls.numeric_ids || !(gr = getgrgid(entries[i].st.st_gid)))
			len = snprintf(buf, sizeof buf, "%u", entries[i].st.st_gid);
		else
			len = (int)strlen(gr->gr_name);
		if (len > w_group)
			w_group = len;

		sizes[i] = ls.human ? human_size(entries[i].st.st_size)
				    : xasprintf("%lld", (long long)entries[i].st.st_size);
		len = (int)strlen(sizes[i]);
		if (len > w_size)
			w_size = len;
	}

	if (n && show_total)
		printf("total %lld\n", total / 2);

	for (i = 0; i < n; i++) {
		char modes[12];
		char *when;
		struct passwd *pw;
		struct group *gr;

		if (!entries[i].have_stat) {
			printf("?????????? ? ? ? ? ? %s\n", entries[i].name);
			continue;
		}
		if (ls.inode)
			printf("%*llu ", w_inode,
			       (unsigned long long)entries[i].st.st_ino);

		mode_string(entries[i].st.st_mode, entries[i].is_link, modes);
		printf("%s %*u ", modes, w_links, (unsigned)entries[i].st.st_nlink);

		pw = ls.numeric_ids ? NULL : getpwuid(entries[i].st.st_uid);
		if (pw)
			printf("%-*s ", w_user, pw->pw_name);
		else
			printf("%-*u ", w_user, entries[i].st.st_uid);

		if (ls.show_group) {
			gr = ls.numeric_ids ? NULL : getgrgid(entries[i].st.st_gid);
			if (gr)
				printf("%-*s ", w_group, gr->gr_name);
			else
				printf("%-*u ", w_group, entries[i].st.st_gid);
		}

		printf("%*s ", w_size, sizes[i] ? sizes[i] : "0");
		when = format_time(entry_time(&entries[i]));
		printf("%s ", when);
		free(when);

		print_name(&entries[i]);
		if (entries[i].is_link && entries[i].link_target)
			printf(" -> %s", entries[i].link_target);
		putchar('\n');
	}
	for (i = 0; i < n; i++)
		free(sizes[i]);
	free(sizes);
}

static void print_columns(Entry *entries, size_t n)
{
	size_t i;
	size_t longest = 0;
	size_t cols, rows, c, r;
	int width = term_columns();

	if (ls.one_per_line || !isatty(1)) {
		for (i = 0; i < n; i++) {
			if (ls.inode && entries[i].have_stat)
				printf("%llu ", (unsigned long long)entries[i].st.st_ino);
			print_name(&entries[i]);
			putchar('\n');
		}
		return;
	}

	for (i = 0; i < n; i++) {
		char *label = entry_label(&entries[i]);

		if (strlen(label) > longest)
			longest = strlen(label);
		free(label);
	}
	if (!longest)
		longest = 1;
	cols = (size_t)width / (longest + 2);
	if (cols < 1)
		cols = 1;
	rows = (n + cols - 1) / cols;

	for (r = 0; r < rows; r++) {
		for (c = 0; c < cols; c++) {
			size_t idx = c * rows + r;
			char *label;
			size_t pad;

			if (idx >= n)
				continue;
			label = entry_label(&entries[idx]);
			print_name(&entries[idx]);
			pad = longest + 2 > strlen(label) ? longest + 2 - strlen(label) : 1;
			if (c + 1 < cols && idx + rows < n)
				while (pad--)
					putchar(' ');
			free(label);
		}
		putchar('\n');
	}
}

/* ------------------------------------------------------------------ listing */

static void entry_fill(Entry *e)
{
	struct stat lst;

	e->have_stat = 0;
	e->is_link = 0;
	if (lstat(e->path, &lst) != 0)
		return;
	e->have_stat = 1;
	e->st = lst;
	if (S_ISLNK(lst.st_mode)) {
		char target[PATH_MAX];
		ssize_t got = readlink(e->path, target, sizeof target - 1);

		e->is_link = 1;
		if (got >= 0) {
			target[got] = '\0';
			e->link_target = xstrdup(target);
		}
		/* the long format shows the link's own mode, the colour follows
		 * the target, which entry_color works out for itself */
	}
}

static void entries_free(Entry *entries, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(entries[i].name);
		free(entries[i].path);
		free(entries[i].link_target);
	}
	free(entries);
}

static int list_directory(const char *path, int show_header, Vec *subdirs)
{
	DIR *d = opendir(path);
	struct dirent *de;
	Entry *entries = NULL;
	size_t n = 0, cap = 0;
	int rc = 0;

	if (!d) {
		gnu_error("ls", "cannot open directory '%s': %s", path, strerror(errno));
		return 1;
	}
	if (show_header)
		printf("%s:\n", path);

	while ((de = readdir(d))) {
		if (!ls.all && !ls.almost_all && de->d_name[0] == '.')
			continue;
		if (ls.almost_all && (strcmp(de->d_name, ".") == 0 ||
				      strcmp(de->d_name, "..") == 0))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			entries = xrealloc(entries, cap * sizeof *entries);
		}
		memset(&entries[n], 0, sizeof entries[n]);
		entries[n].name = xstrdup(de->d_name);
		entries[n].path = str_suffix(path, "/")
					  ? xasprintf("%s%s", path, de->d_name)
					  : xasprintf("%s/%s", path, de->d_name);
		entry_fill(&entries[n]);
		n++;
	}
	closedir(d);

	if (n > 1 && ls.sort != SORT_NONE)
		mergesort(entries, n, sizeof *entries, compare_entries);
	else if (n > 1 && ls.reverse) {
		size_t i;

		for (i = 0; i < n / 2; i++) {
			Entry tmp = entries[i];

			entries[i] = entries[n - 1 - i];
			entries[n - 1 - i] = tmp;
		}
	}

	if (ls.long_format)
		print_long(entries, n, 1);
	else
		print_columns(entries, n);

	if (ls.recursive && subdirs) {
		size_t i;

		for (i = 0; i < n; i++) {
			if (!entries[i].have_stat || entries[i].is_link)
				continue;
			if (!S_ISDIR(entries[i].st.st_mode))
				continue;
			if (strcmp(entries[i].name, ".") == 0 ||
			    strcmp(entries[i].name, "..") == 0)
				continue;
			vec_pushs(subdirs, entries[i].path);
		}
	}
	entries_free(entries, n);
	return rc;
}

/* ------------------------------------------------------------------ options */

int gnu_ls(int argc, char **argv)
{
	Vec operands;
	int i, rc = 0;
	size_t k;

	memset(&ls, 0, sizeof ls);
	ls.sort = SORT_NAME;
	ls.time_field = TIME_MTIME;
	ls.colour = COLOR_AUTO;
	ls.show_group = 1;
	vec_init(&operands);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (strcmp(a, "--") == 0) {
				i++;
				break;
			}
			if (gnu_long_opt(a, "color", &val) || gnu_long_opt(a, "colour", &val)) {
				const char *when = val ? val : "always";

				if (strcmp(when, "never") == 0 || strcmp(when, "none") == 0)
					ls.colour = COLOR_NEVER;
				else if (strcmp(when, "auto") == 0 || strcmp(when, "tty") == 0)
					ls.colour = COLOR_AUTO;
				else
					ls.colour = COLOR_ALWAYS;
				continue;
			}
			if (gnu_long_opt(a, "group-directories-first", NULL)) {
				ls.group_dirs_first = 1;
				continue;
			}
			if (gnu_long_opt(a, "time-style", &val)) {
				ls.time_style = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "sort", &val)) {
				const char *how = val ? val : argv[++i];

				if (strcmp(how, "none") == 0)
					ls.sort = SORT_NONE;
				else if (strcmp(how, "size") == 0)
					ls.sort = SORT_SIZE;
				else if (strcmp(how, "time") == 0)
					ls.sort = SORT_TIME;
				else if (strcmp(how, "extension") == 0)
					ls.sort = SORT_EXT;
				else if (strcmp(how, "version") == 0)
					ls.sort = SORT_VERSION;
				else
					ls.sort = SORT_NAME;
				continue;
			}
			if (gnu_long_opt(a, "all", NULL)) {
				ls.all = 1;
				continue;
			}
			if (gnu_long_opt(a, "almost-all", NULL)) {
				ls.almost_all = 1;
				continue;
			}
			if (gnu_long_opt(a, "reverse", NULL)) {
				ls.reverse = 1;
				continue;
			}
			if (gnu_long_opt(a, "recursive", NULL)) {
				ls.recursive = 1;
				continue;
			}
			if (gnu_long_opt(a, "human-readable", NULL)) {
				ls.human = 1;
				continue;
			}
			if (gnu_long_opt(a, "classify", NULL)) {
				ls.classify = 1;
				continue;
			}
			if (gnu_long_opt(a, "inode", NULL)) {
				ls.inode = 1;
				continue;
			}
			if (gnu_long_opt(a, "numeric-uid-gid", NULL)) {
				ls.numeric_ids = 1;
				ls.long_format = 1;
				continue;
			}
			if (gnu_long_opt(a, "directory", NULL)) {
				ls.directory = 1;
				continue;
			}
			if (gnu_long_opt(a, "quote-name", NULL)) {
				ls.quote_names = 1;
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: ls [-1AadFhilnpQRrStuUX] [--color[=WHEN]]\n"
				       "          [--group-directories-first] "
				       "[--time-style=STYLE]\n"
				       "          [--sort=WORD] [file ...]\n");
				vec_free(&operands);
				return 0;
			}
			gnu_error("ls", "unrecognized option '%s'", a);
			vec_free(&operands);
			return 2;
		}
		if (a[0] == '-' && a[1]) {
			const char *p;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'l': ls.long_format = 1; break;
				case 'a': ls.all = 1; break;
				case 'A': ls.almost_all = 1; break;
				case '1': ls.one_per_line = 1; break;
				case 'd': ls.directory = 1; break;
				case 'F': ls.classify = 1; break;
				case 'p': ls.slash_dirs = 1; break;
				case 'h': ls.human = 1; break;
				case 'i': ls.inode = 1; break;
				case 'n': ls.numeric_ids = ls.long_format = 1; break;
				case 'Q': ls.quote_names = 1; break;
				case 'R': ls.recursive = 1; break;
				case 'r': ls.reverse = 1; break;
				case 'S': ls.sort = SORT_SIZE; break;
				case 't': ls.sort = SORT_TIME; break;
				case 'X': ls.sort = SORT_EXT; break;
				case 'U': ls.sort = SORT_NONE; break;
				case 'v': ls.sort = SORT_VERSION; break;
				case 'u': ls.time_field = TIME_ATIME; break;
				case 'c': ls.time_field = TIME_CTIME; break;
				case 'g': ls.long_format = 1; break;
				case 'o': ls.long_format = 1; ls.show_group = 0; break;
				case 'G': ls.show_group = 0; break;
				case 'C': ls.one_per_line = 0; break;
				case 'f': ls.sort = SORT_NONE; ls.all = 1; break;
				case 'k': break;
				default:
					gnu_error("ls", "invalid option -- '%c'", *p);
					vec_free(&operands);
					return 2;
				}
			}
			continue;
		}
		vec_pushs(&operands, a);
	}
	for (; i < argc; i++)
		vec_pushs(&operands, argv[i]);

	ls.colour_on = ls.colour == COLOR_ALWAYS ||
		       (ls.colour == COLOR_AUTO && !var_get("NO_COLOR") && isatty(1));

	if (!operands.len)
		vec_pushs(&operands, ".");

	{
		Vec dirs, pending;
		Entry *files = NULL;
		size_t nfiles = 0;
		int header;

		vec_init(&dirs);
		vec_init(&pending);

		/* GNU lists the named files together first, then each directory. */
		for (k = 0; k < operands.len; k++) {
			struct stat st;

			if (lstat(operands.v[k], &st) != 0) {
				gnu_error("ls", "cannot access '%s': %s", operands.v[k],
					  strerror(errno));
				rc = 2;
				continue;
			}
			if (S_ISDIR(st.st_mode) && !ls.directory) {
				vec_pushs(&dirs, operands.v[k]);
				continue;
			}
			files = xrealloc(files, (nfiles + 1) * sizeof *files);
			memset(&files[nfiles], 0, sizeof files[nfiles]);
			files[nfiles].name = xstrdup(operands.v[k]);
			files[nfiles].path = xstrdup(operands.v[k]);
			entry_fill(&files[nfiles]);
			nfiles++;
		}

		if (nfiles > 1 && ls.sort != SORT_NONE)
			mergesort(files, nfiles, sizeof *files, compare_entries);
		if (nfiles) {
			if (ls.long_format)
				print_long(files, nfiles, 0);
			else
				print_columns(files, nfiles);
		}
		entries_free(files, nfiles);

		header = dirs.len + (nfiles ? 1 : 0) > 1 || ls.recursive;
		for (k = 0; k < dirs.len; k++) {
			if (k || nfiles)
				putchar('\n');
			if (list_directory(dirs.v[k], header, ls.recursive ? &pending : NULL))
				rc = 2;
		}
		/* -R: breadth first, which keeps the headers in a readable order */
		while (pending.len) {
			char *next = vec_remove(&pending, 0);

			putchar('\n');
			if (list_directory(next, 1, &pending))
				rc = 2;
			free(next);
		}
		vec_free(&dirs);
		vec_free(&pending);
	}

	vec_free(&operands);
	return rc;
}

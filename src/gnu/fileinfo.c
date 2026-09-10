/* fileinfo.c - stat, readlink, realpath and mktemp.
 *
 * BSD stat has no -c, BSD readlink has no -f, and BSD mktemp wants a template.
 * Scripts written against GNU expect otherwise.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* mktemp(3) is what `mktemp -u` means; the warning is expected. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gnu.h"

/* -------------------------------------------------------------------- stat */

static void mode_string(mode_t m, char *out)
{
	const char *rwx = "rwxrwxrwx";
	int i;

	out[0] = S_ISDIR(m)	? 'd'
		 : S_ISLNK(m)	? 'l'
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

static void stat_format(const char *fmt, const char *path, const struct stat *st, Buf *out)
{
	const char *p;

	for (p = fmt; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': buf_putc(out, '\n'); break;
			case 't': buf_putc(out, '\t'); break;
			case '\\': buf_putc(out, '\\'); break;
			case '0': buf_putc(out, '\0'); break;
			default: buf_putc(out, *p); break;
			}
			continue;
		}
		if (*p != '%') {
			buf_putc(out, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'n':
			buf_puts(out, path);
			break;
		case 'N':
			buf_printf(out, "'%s'", path);
			if (S_ISLNK(st->st_mode)) {
				char target[PATH_MAX];
				ssize_t n = readlink(path, target, sizeof target - 1);

				if (n > 0) {
					target[n] = '\0';
					buf_printf(out, " -> '%s'", target);
				}
			}
			break;
		case 's':
			buf_printf(out, "%lld", (long long)st->st_size);
			break;
		case 'b':
			buf_printf(out, "%lld", (long long)st->st_blocks);
			break;
		case 'B':
			buf_puts(out, "512");
			break;
		case 'f':
			buf_printf(out, "%x", st->st_mode);
			break;
		case 'a':
			buf_printf(out, "%04o", st->st_mode & 07777);
			break;
		case 'A': {
			char m[12];
			mode_string(st->st_mode, m);
			buf_puts(out, m);
			break;
		}
		case 'u':
			buf_printf(out, "%u", st->st_uid);
			break;
		case 'U': {
			struct passwd *pw = getpwuid(st->st_uid);
			buf_puts(out, pw ? pw->pw_name : "UNKNOWN");
			break;
		}
		case 'g':
			buf_printf(out, "%u", st->st_gid);
			break;
		case 'G': {
			struct group *gr = getgrgid(st->st_gid);
			buf_puts(out, gr ? gr->gr_name : "UNKNOWN");
			break;
		}
		case 'i':
			buf_printf(out, "%llu", (unsigned long long)st->st_ino);
			break;
		case 'h':
			buf_printf(out, "%u", (unsigned)st->st_nlink);
			break;
		case 'd':
			buf_printf(out, "%lld", (long long)st->st_dev);
			break;
		case 'D':
			buf_printf(out, "%llx", (unsigned long long)st->st_dev);
			break;
		case 'F':
			buf_puts(out, S_ISDIR(st->st_mode)	? "directory"
				      : S_ISLNK(st->st_mode)	? "symbolic link"
				      : S_ISCHR(st->st_mode)	? "character special file"
				      : S_ISBLK(st->st_mode)	? "block special file"
				      : S_ISFIFO(st->st_mode)	? "fifo"
				      : S_ISSOCK(st->st_mode)	? "socket"
				      : st->st_size == 0	? "regular empty file"
								: "regular file");
			break;
		case 'X':
			buf_printf(out, "%lld", (long long)st->st_atime);
			break;
		case 'Y':
			buf_printf(out, "%lld", (long long)st->st_mtime);
			break;
		case 'Z':
			buf_printf(out, "%lld", (long long)st->st_ctime);
			break;
		case 'W':
			buf_printf(out, "%lld", (long long)st->st_birthtimespec.tv_sec);
			break;
		case 'x':
		case 'y':
		case 'z':
		case 'w': {
			time_t t = *p == 'x'   ? st->st_atime
				   : *p == 'y' ? st->st_mtime
				   : *p == 'z' ? st->st_ctime
					       : st->st_birthtimespec.tv_sec;
			struct tm tm;
			char buf[64];

			localtime_r(&t, &tm);
			strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
			buf_printf(out, "%s.%09ld %+03ld%02ld", buf, 0L,
				   tm.tm_gmtoff / 3600, (tm.tm_gmtoff % 3600) / 60);
			break;
		}
		case '%':
			buf_putc(out, '%');
			break;
		case '\0':
			buf_putc(out, '%');
			p--;
			break;
		default:
			buf_putc(out, '%');
			buf_putc(out, *p);
			break;
		}
	}
}

int gnu_stat(int argc, char **argv)
{
	const char *format = NULL;
	int dereference = 0, terse = 0;
	int i, rc = 0;
	Vec files;
	size_t k;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			if (gnu_long_opt(a, "format", &val) ||
			    gnu_long_opt(a, "printf", &val)) {
				format = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "dereference", NULL)) {
				dereference = 1;
				continue;
			}
			if (gnu_long_opt(a, "terse", NULL)) {
				terse = 1;
				continue;
			}
			if (a[1] == 'c' || a[1] == 'f') {
				format = a[2] ? a + 2 : argv[++i];
				continue;
			}
			if (a[1] == 'L') {
				dereference = 1;
				continue;
			}
			if (a[1] == 't') {
				terse = 1;
				continue;
			}
			gnu_error("stat", "invalid option -- '%s'", a);
			vec_free(&files);
			return 1;
		}
		vec_pushs(&files, a);
	}
	if (!files.len) {
		gnu_error("stat", "missing operand");
		vec_free(&files);
		return 1;
	}
	if (!format)
		format = terse ? "%n %s %b %f %u %g %D %i %h %X %Y %Z %B"
			       : "  File: %N\n  Size: %s\tBlocks: %b\tIO Block: %B\t%F\n"
				 "Device: %Dh\tInode: %i\tLinks: %h\n"
				 "Access: (%a/%A)\tUid: (%5u/%8U)\tGid: (%5g/%8G)\n"
				 "Access: %x\nModify: %y\nChange: %z";

	for (k = 0; k < files.len; k++) {
		struct stat st;
		Buf out;

		if ((dereference ? stat(files.v[k], &st) : lstat(files.v[k], &st)) != 0) {
			gnu_error("stat", "cannot stat '%s': %s", files.v[k], strerror(errno));
			rc = 1;
			continue;
		}
		buf_init(&out);
		stat_format(format, files.v[k], &st, &out);
		fwrite(out.b, 1, out.len, stdout);
		putchar('\n');
		buf_free(&out);
	}
	vec_free(&files);
	return rc;
}

/* --------------------------------------------------- readlink and realpath */

/* Resolve every component; missing trailing components are allowed when
 * `missing_ok` is set, which is what -m and --canonicalize-missing mean. */
static char *canonicalise(const char *path, int missing_ok, int require_exists)
{
	char resolved[PATH_MAX];
	char work[PATH_MAX];
	char *tail = NULL;

	if (realpath(path, resolved))
		return xstrdup(resolved);
	if (require_exists)
		return NULL;
	if (!missing_ok)
		return NULL;

	/* peel components off the end until the rest resolves */
	snprintf(work, sizeof work, "%s", path);
	{
		Buf suffix;

		buf_init(&suffix);
		for (;;) {
			char *slash = strrchr(work, '/');

			if (!slash) {
				char cwd[PATH_MAX];

				if (!getcwd(cwd, sizeof cwd)) {
					buf_free(&suffix);
					return NULL;
				}
				{
					char *r = xasprintf("%s/%s%s", cwd, work,
							    suffix.b ? suffix.b : "");
					buf_free(&suffix);
					return r;
				}
			}
			{
				Buf next;

				buf_init(&next);
				buf_putc(&next, '/');
				buf_puts(&next, slash + 1);
				buf_puts(&next, suffix.b ? suffix.b : "");
				buf_free(&suffix);
				suffix = next;
			}
			*slash = '\0';
			if (!*work) {
				char *r = xasprintf("/%s", suffix.b ? suffix.b + 1 : "");
				buf_free(&suffix);
				return r;
			}
			if (realpath(work, resolved)) {
				char *r = xasprintf("%s%s", resolved,
						    suffix.b ? suffix.b : "");
				buf_free(&suffix);
				return r;
			}
		}
	}
	(void)tail;
}

int gnu_readlink(int argc, char **argv)
{
	int canonical = 0, exists = 0, missing = 0, quiet = 0, zero = 0, no_newline = 0;
	int i, rc = 0;
	Vec files;
	size_t k;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1]) {
			const char *p;

			if (gnu_long_opt(a, "canonicalize", NULL)) {
				canonical = 1;
				continue;
			}
			if (gnu_long_opt(a, "canonicalize-existing", NULL)) {
				canonical = exists = 1;
				continue;
			}
			if (gnu_long_opt(a, "canonicalize-missing", NULL)) {
				canonical = missing = 1;
				continue;
			}
			if (gnu_long_opt(a, "no-newline", NULL)) {
				no_newline = 1;
				continue;
			}
			if (gnu_long_opt(a, "zero", NULL)) {
				zero = 1;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL) || gnu_long_opt(a, "silent", NULL)) {
				quiet = 1;
				continue;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'f': canonical = 1; missing = 1; break;
				case 'e': canonical = exists = 1; break;
				case 'm': canonical = missing = 1; break;
				case 'n': no_newline = 1; break;
				case 'z': zero = 1; break;
				case 'q':
				case 's': quiet = 1; break;
				case 'v': quiet = 0; break;
				default:
					gnu_error("readlink", "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&files, a);
	}
	if (!files.len) {
		gnu_error("readlink", "missing operand");
		vec_free(&files);
		return 1;
	}

	for (k = 0; k < files.len; k++) {
		char *result = NULL;

		if (canonical) {
			result = canonicalise(files.v[k], missing || !exists, exists);
			/* -f wants every component but the last to exist */
			if (result && !missing && !exists) {
				struct stat st;
				char *dir = xstrdup(result);
				char *slash = strrchr(dir, '/');

				if (slash && slash != dir)
					*slash = '\0';
				if (stat(dir, &st) != 0) {
					free(result);
					result = NULL;
				}
				free(dir);
			}
		} else {
			char target[PATH_MAX];
			ssize_t n = readlink(files.v[k], target, sizeof target - 1);

			if (n >= 0) {
				target[n] = '\0';
				result = xstrdup(target);
			}
		}
		if (!result) {
			if (!quiet)
				gnu_error("readlink", "%s: %s", files.v[k], strerror(errno));
			rc = 1;
			continue;
		}
		fputs(result, stdout);
		if (zero)
			putchar('\0');
		else if (!no_newline)
			putchar('\n');
		free(result);
	}
	vec_free(&files);
	return rc;
}

int gnu_realpath(int argc, char **argv)
{
	const char *relative_to = NULL;
	int missing = 0, no_symlinks = 0, quiet = 0, zero = 0;
	int i, rc = 0;
	Vec files;
	size_t k;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			const char *p;

			if (gnu_long_opt(a, "relative-to", &val)) {
				relative_to = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "canonicalize-missing", NULL)) {
				missing = 1;
				continue;
			}
			if (gnu_long_opt(a, "no-symlinks", NULL)) {
				no_symlinks = 1;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL)) {
				quiet = 1;
				continue;
			}
			if (gnu_long_opt(a, "zero", NULL)) {
				zero = 1;
				continue;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'm': missing = 1; break;
				case 's': no_symlinks = 1; break;
				case 'q': quiet = 1; break;
				case 'z': zero = 1; break;
				case 'e': break;
				default:
					gnu_error("realpath", "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&files, a);
	}
	if (!files.len) {
		gnu_error("realpath", "missing operand");
		vec_free(&files);
		return 1;
	}
	(void)no_symlinks;

	for (k = 0; k < files.len; k++) {
		char *result = canonicalise(files.v[k], 1, !missing && 0);

		if (!result) {
			if (!quiet)
				gnu_error("realpath", "%s: %s", files.v[k], strerror(errno));
			rc = 1;
			continue;
		}
		if (relative_to) {
			char *base = canonicalise(relative_to, 1, 0);

			if (base) {
				size_t common = 0, bl = strlen(base);
				Buf rel;

				while (common < bl && result[common] &&
				       base[common] == result[common])
					common++;
				if (common == bl && (result[common] == '/' || !result[common])) {
					char *r = xstrdup(result[common] == '/'
								  ? result + common + 1
								  : ".");
					free(result);
					result = r;
				} else {
					/* walk up from base, then down into result */
					size_t up = 0, j;

					while (common > 0 && base[common - 1] != '/' &&
					       result[common - 1] != '/')
						common--;
					for (j = common; j < bl; j++)
						if (base[j] == '/')
							up++;
					if (common < bl)
						up++;
					buf_init(&rel);
					for (j = 0; j < up; j++)
						buf_puts(&rel, "../");
					buf_puts(&rel, result + common);
					free(result);
					result = buf_take(&rel);
				}
				free(base);
			}
		}
		fputs(result, stdout);
		putchar(zero ? '\0' : '\n');
		free(result);
	}
	vec_free(&files);
	return rc;
}

/* ------------------------------------------------------------------ mktemp */

int gnu_mktemp(int argc, char **argv)
{
	int make_dir = 0, dry_run = 0, use_tmpdir = 0, quiet = 0;
	const char *suffix = "";
	const char *tmpdir_opt = NULL;
	const char *template = NULL;
	int i;
	Buf path;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			const char *p;

			if (gnu_long_opt(a, "directory", NULL)) {
				make_dir = 1;
				continue;
			}
			if (gnu_long_opt(a, "dry-run", NULL)) {
				dry_run = 1;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL)) {
				quiet = 1;
				continue;
			}
			if (gnu_long_opt(a, "suffix", &val)) {
				suffix = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "tmpdir", &val)) {
				use_tmpdir = 1;
				tmpdir_opt = val;
				continue;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'd': make_dir = 1; break;
				case 'u': dry_run = 1; break;
				case 'q': quiet = 1; break;
				case 't': use_tmpdir = 1; break;
				case 'p':
					tmpdir_opt = p[1] ? p + 1 : argv[++i];
					use_tmpdir = 1;
					p = a + strlen(a) - 1;
					break;
				default:
					gnu_error("mktemp", "invalid option -- '%c'", *p);
					return 1;
				}
			}
			continue;
		}
		template = a;
	}

	if (!template)
		template = "tmp.XXXXXXXXXX";

	buf_init(&path);
	if (use_tmpdir && !strchr(template, '/')) {
		const char *dir = tmpdir_opt;

		if (!dir)
			dir = var_get("TMPDIR");
		if (!dir || !*dir)
			dir = "/tmp";
		buf_puts(&path, dir);
		if (!str_suffix(dir, "/"))
			buf_putc(&path, '/');
	} else if (!strchr(template, '/') && !use_tmpdir) {
		/* GNU creates in the current directory unless -t or -p is given */
	}
	buf_puts(&path, template);
	buf_puts(&path, suffix);

	{
		char *buf = buf_take(&path);
		int fd;

		if (!strstr(buf, "XXX")) {
			gnu_error("mktemp", "too few X's in template '%s'", template);
			free(buf);
			return 1;
		}
		if (dry_run) {
			if (!mktemp(buf) || !*buf) {
				free(buf);
				return 1;
			}
			puts(buf);
			free(buf);
			return 0;
		}
		if (make_dir) {
			if (!mkdtemp(buf)) {
				if (!quiet)
					gnu_error("mktemp", "failed to create directory: %s",
						  strerror(errno));
				free(buf);
				return 1;
			}
			puts(buf);
			free(buf);
			return 0;
		}
		fd = *suffix ? mkstemps(buf, (int)strlen(suffix)) : mkstemp(buf);
		if (fd < 0) {
			if (!quiet)
				gnu_error("mktemp", "failed to create file: %s",
					  strerror(errno));
			free(buf);
			return 1;
		}
		close(fd);
		puts(buf);
		free(buf);
	}
	return 0;
}

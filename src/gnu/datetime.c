/* datetime.c - date and timeout.
 *
 * `timeout` does not exist on macOS at all, and BSD `date` has no -d, no
 * --iso-8601 and no %N.  Both are the reason a lot of Linux scripts stop at
 * the first line on a Mac.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gnu.h"

/* ---------------------------------------------------------------- date -d */

struct rel {
	long years, months, days, hours, minutes, seconds;
};

static int unit_amount(const char *word, struct rel *r, long n)
{
	if (str_prefix(word, "year"))
		r->years += n;
	else if (str_prefix(word, "month"))
		r->months += n;
	else if (str_prefix(word, "fortnight"))
		r->days += 14 * n;
	else if (str_prefix(word, "week"))
		r->days += 7 * n;
	else if (str_prefix(word, "day"))
		r->days += n;
	else if (str_prefix(word, "hour"))
		r->hours += n;
	else if (str_prefix(word, "min"))
		r->minutes += n;
	else if (str_prefix(word, "sec"))
		r->seconds += n;
	else
		return 0;
	return 1;
}

/* Absolute date forms: YYYY-MM-DD[ HH:MM[:SS]], HH:MM[:SS], @epoch. */
static int parse_absolute(const char **pp, struct tm *tm, int *have_date, int *have_time)
{
	const char *p = *pp;
	int y, mo, d, h, mi, s;
	int n = 0;

	while (isspace((unsigned char)*p))
		p++;

	if (sscanf(p, "%d-%d-%d%n", &y, &mo, &d, &n) == 3 && n > 0 && y > 1000) {
		tm->tm_year = y - 1900;
		tm->tm_mon = mo - 1;
		tm->tm_mday = d;
		tm->tm_hour = tm->tm_min = tm->tm_sec = 0;
		*have_date = 1;
		p += n;
		while (isspace((unsigned char)*p) || *p == 'T')
			p++;
		n = 0;
		if (sscanf(p, "%d:%d:%d%n", &h, &mi, &s, &n) == 3 && n > 0) {
			tm->tm_hour = h;
			tm->tm_min = mi;
			tm->tm_sec = s;
			*have_time = 1;
			p += n;
		} else if (sscanf(p, "%d:%d%n", &h, &mi, &n) == 2 && n > 0) {
			tm->tm_hour = h;
			tm->tm_min = mi;
			*have_time = 1;
			p += n;
		}
		if (*p == 'Z')
			p++;
		*pp = p;
		return 1;
	}

	n = 0;
	if (sscanf(p, "%d/%d/%d%n", &mo, &d, &y, &n) == 3 && n > 0) {
		tm->tm_year = (y > 1000 ? y : 2000 + y) - 1900;
		tm->tm_mon = mo - 1;
		tm->tm_mday = d;
		tm->tm_hour = tm->tm_min = tm->tm_sec = 0;
		*have_date = 1;
		*pp = p + n;
		return 1;
	}

	n = 0;
	if (sscanf(p, "%d:%d:%d%n", &h, &mi, &s, &n) == 3 && n > 0) {
		tm->tm_hour = h;
		tm->tm_min = mi;
		tm->tm_sec = s;
		*have_time = 1;
		*pp = p + n;
		return 1;
	}
	n = 0;
	if (sscanf(p, "%d:%d%n", &h, &mi, &n) == 2 && n > 0) {
		tm->tm_hour = h;
		tm->tm_min = mi;
		tm->tm_sec = 0;
		*have_time = 1;
		*pp = p + n;
		return 1;
	}
	return 0;
}

/* GNU-style date string: absolute parts, relative offsets, or both. */
static int parse_date_string(const char *spec, time_t *out, int utc)
{
	struct tm tm;
	time_t base = time(NULL);
	struct rel r;
	const char *p = spec;
	int have_date = 0, have_time = 0, have_rel = 0;

	memset(&r, 0, sizeof r);

	while (isspace((unsigned char)*p))
		p++;

	if (*p == '@') {
		*out = (time_t)strtoll(p + 1, NULL, 10);
		return 1;
	}

	if (utc)
		gmtime_r(&base, &tm);
	else
		localtime_r(&base, &tm);
	tm.tm_isdst = -1;

	if (str_casecmp(p, "now") == 0) {
		*out = base;
		return 1;
	}

	for (;;) {
		char word[64];
		int n = 0;
		long amount;
		int sign = 1;

		while (isspace((unsigned char)*p) || *p == ',')
			p++;
		if (!*p)
			break;

		if (parse_absolute(&p, &tm, &have_date, &have_time))
			continue;

		if (sscanf(p, "%63s%n", word, &n) != 1)
			break;

		if (str_casecmp(word, "today") == 0) {
			tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
			have_date = have_time = 1;
			p += n;
			continue;
		}
		if (str_casecmp(word, "tomorrow") == 0) {
			r.days += 1;
			have_rel = 1;
			p += n;
			continue;
		}
		if (str_casecmp(word, "yesterday") == 0) {
			r.days -= 1;
			have_rel = 1;
			p += n;
			continue;
		}
		if (str_casecmp(word, "midnight") == 0) {
			tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
			have_time = 1;
			p += n;
			continue;
		}
		if (str_casecmp(word, "noon") == 0) {
			tm.tm_hour = 12;
			tm.tm_min = tm.tm_sec = 0;
			have_time = 1;
			p += n;
			continue;
		}
		if (str_casecmp(word, "ago") == 0) {
			r.years = -r.years;
			r.months = -r.months;
			r.days = -r.days;
			r.hours = -r.hours;
			r.minutes = -r.minutes;
			r.seconds = -r.seconds;
			p += n;
			continue;
		}
		if (str_casecmp(word, "next") == 0) {
			sign = 1;
			p += n;
			{
				char unit[64];
				int n2 = 0;
				if (sscanf(p, "%63s%n", unit, &n2) == 1 &&
				    unit_amount(unit, &r, sign)) {
					have_rel = 1;
					p += n2;
					continue;
				}
			}
			continue;
		}
		if (str_casecmp(word, "last") == 0) {
			p += n;
			{
				char unit[64];
				int n2 = 0;
				if (sscanf(p, "%63s%n", unit, &n2) == 1 &&
				    unit_amount(unit, &r, -1)) {
					have_rel = 1;
					p += n2;
					continue;
				}
			}
			continue;
		}

		/* [+-]N unit, or Nunit */
		{
			const char *q = word;
			char *end;

			if (*q == '+' || *q == '-')
				q++;
			if (isdigit((unsigned char)*q)) {
				amount = strtol(word, &end, 10);
				if (*end) {
					if (unit_amount(end, &r, amount)) {
						have_rel = 1;
						p += n;
						continue;
					}
				} else {
					char unit[64];
					int n2 = 0;
					const char *after = p + n;

					if (sscanf(after, "%63s%n", unit, &n2) == 1 &&
					    unit_amount(unit, &r, amount)) {
						have_rel = 1;
						p += n + n2;
						continue;
					}
					/* a bare year */
					if (amount > 1000 && amount < 3000) {
						tm.tm_year = (int)amount - 1900;
						have_date = 1;
						p += n;
						continue;
					}
				}
			}
		}
		if (unit_amount(word, &r, 1)) {
			have_rel = 1;
			p += n;
			continue;
		}
		return 0;
	}

	if (!have_date && !have_time && !have_rel)
		return 0;

	tm.tm_year += (int)r.years;
	tm.tm_mon += (int)r.months;
	tm.tm_mday += (int)r.days;
	tm.tm_hour += (int)r.hours;
	tm.tm_min += (int)r.minutes;
	tm.tm_sec += (int)r.seconds;
	tm.tm_isdst = -1;

	*out = utc ? timegm(&tm) : mktime(&tm);
	return *out != (time_t)-1;
}

/* strftime plus the GNU extensions scripts rely on. */
static char *format_time(const char *fmt, const struct tm *tm, time_t t, long nsec, int utc)
{
	Buf out;
	const char *p;

	buf_init(&out);
	for (p = fmt; *p; p++) {
		char piece[256];
		char spec[8];

		if (*p != '%') {
			buf_putc(&out, *p);
			continue;
		}
		p++;
		if (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') {
			/* GNU padding flags: honour - (no pad) and _ (space pad) */
			int flag = *p++;
			char sub[3];
			int width = 0;

			while (isdigit((unsigned char)*p))
				width = width * 10 + (*p++ - '0');
			sub[0] = '%';
			sub[1] = *p;
			sub[2] = '\0';
			strftime(piece, sizeof piece, sub, tm);
			if (flag == '-') {
				char *q = piece;
				while (q[0] == '0' && q[1])
					q++;
				buf_puts(&out, q);
			} else if (flag == '_') {
				char *q = piece;
				while (q[0] == '0' && q[1])
					q++;
				buf_printf(&out, "%*s", width ? width : 2, q);
			} else if (flag == '^') {
				char *q;
				for (q = piece; *q; q++)
					*q = (char)toupper((unsigned char)*q);
				buf_puts(&out, piece);
			} else {
				buf_puts(&out, piece);
			}
			continue;
		}
		switch (*p) {
		case '\0':
			buf_putc(&out, '%');
			p--;
			break;
		case 'N':
			buf_printf(&out, "%09ld", nsec);
			break;
		case 's':
			buf_printf(&out, "%lld", (long long)t);
			break;
		case ':': {
			/* %:z and %::z */
			int colons = 0;
			const char *q = p;

			while (*q == ':') {
				colons++;
				q++;
			}
			if (*q == 'z') {
				long off = utc ? 0 : tm->tm_gmtoff;
				long a = off < 0 ? -off : off;

				if (colons == 1)
					buf_printf(&out, "%c%02ld:%02ld", off < 0 ? '-' : '+',
						   a / 3600, (a % 3600) / 60);
				else
					buf_printf(&out, "%c%02ld:%02ld:%02ld",
						   off < 0 ? '-' : '+', a / 3600,
						   (a % 3600) / 60, a % 60);
				p = q;
			} else {
				buf_putc(&out, '%');
				buf_putc(&out, ':');
			}
			break;
		}
		case 'z': {
			long off = utc ? 0 : tm->tm_gmtoff;
			long a = off < 0 ? -off : off;

			buf_printf(&out, "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600,
				   (a % 3600) / 60);
			break;
		}
		case '%':
			buf_putc(&out, '%');
			break;
		default:
			spec[0] = '%';
			spec[1] = *p;
			spec[2] = '\0';
			if (strftime(piece, sizeof piece, spec, tm))
				buf_puts(&out, piece);
			break;
		}
	}
	return buf_take(&out);
}

int gnu_date(int argc, char **argv)
{
	int utc = 0;
	const char *datestr = NULL;
	const char *reference = NULL;
	const char *format = NULL;
	const char *setstr = NULL;
	char iso_buf[64];
	int i;
	time_t t;
	long nsec = 0;
	struct tm tm;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '+') {
			format = a + 1;
			continue;
		}
		if (a[0] == '-' && a[1] == '-') {
			if (gnu_long_opt(a, "utc", NULL) || gnu_long_opt(a, "universal", NULL)) {
				utc = 1;
				continue;
			}
			if (gnu_long_opt(a, "date", &val)) {
				datestr = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "reference", &val)) {
				reference = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "set", &val)) {
				setstr = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "iso-8601", &val)) {
				const char *want = val ? val : "date";

				if (str_prefix(want, "s"))
					format = "%Y-%m-%dT%H:%M:%S%:z";
				else if (str_prefix(want, "n"))
					format = "%Y-%m-%dT%H:%M:%S,%N%:z";
				else if (str_prefix(want, "m"))
					format = "%Y-%m-%dT%H:%M%:z";
				else if (str_prefix(want, "h"))
					format = "%Y-%m-%dT%H%:z";
				else
					format = "%Y-%m-%d";
				continue;
			}
			if (gnu_long_opt(a, "rfc-3339", &val)) {
				const char *want = val ? val : "date";

				if (str_prefix(want, "s"))
					format = "%Y-%m-%d %H:%M:%S%:z";
				else if (str_prefix(want, "n"))
					format = "%Y-%m-%d %H:%M:%S.%N%:z";
				else
					format = "%Y-%m-%d";
				continue;
			}
			if (gnu_long_opt(a, "rfc-email", NULL) ||
			    gnu_long_opt(a, "rfc-2822", NULL)) {
				format = "%a, %d %b %Y %H:%M:%S %z";
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: date [-u] [-d STRING] [-r FILE] "
				       "[--iso-8601[=FMT]] [+FORMAT]\n");
				return 0;
			}
			gnu_error("date", "unrecognized option '%s'", a);
			return 1;
		}
		if (a[0] == '-' && a[1]) {
			switch (a[1]) {
			case 'u':
				utc = 1;
				continue;
			case 'd':
				datestr = a[2] ? a + 2 : argv[++i];
				continue;
			case 'r':
				reference = a[2] ? a + 2 : argv[++i];
				continue;
			case 's':
				setstr = a[2] ? a + 2 : argv[++i];
				continue;
			case 'R':
				format = "%a, %d %b %Y %H:%M:%S %z";
				continue;
			case 'I': {
				const char *want = a + 2;

				if (str_prefix(want, "s"))
					format = "%Y-%m-%dT%H:%M:%S%:z";
				else if (str_prefix(want, "n"))
					format = "%Y-%m-%dT%H:%M:%S,%N%:z";
				else if (str_prefix(want, "m"))
					format = "%Y-%m-%dT%H:%M%:z";
				else if (str_prefix(want, "h"))
					format = "%Y-%m-%dT%H%:z";
				else
					format = "%Y-%m-%d";
				continue;
			}
			default:
				gnu_error("date", "invalid option -- '%c'", a[1]);
				return 1;
			}
		}
		/* a bare +FORMAT was handled above; anything else is unsupported */
		gnu_error("date", "setting the system clock is not supported");
		return 1;
	}
	(void)setstr;

	if (reference) {
		struct stat st;

		if (stat(reference, &st) != 0)
			return gnu_file_error("date", reference);
		t = st.st_mtime;
		nsec = st.st_mtimespec.tv_nsec;
	} else if (datestr) {
		if (!parse_date_string(datestr, &t, utc)) {
			gnu_error("date", "invalid date '%s'", datestr);
			return 1;
		}
	} else {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		t = tv.tv_sec;
		nsec = tv.tv_usec * 1000;
	}

	if (utc)
		gmtime_r(&t, &tm);
	else
		localtime_r(&t, &tm);

	if (!format) {
		snprintf(iso_buf, sizeof iso_buf, "%%a %%b %%e %%H:%%M:%%S %%Z %%Y");
		format = iso_buf;
	}
	{
		char *s = format_time(format, &tm, t, nsec, utc);

		puts(s);
		free(s);
	}
	return 0;
}

/* ---------------------------------------------------------------- timeout */

static volatile sig_atomic_t timed_out;
static pid_t timeout_child;

static void on_alarm(int sig)
{
	(void)sig;
	timed_out = 1;
}

static double parse_duration(const char *s, int *ok)
{
	char *end;
	double v = strtod(s, &end);

	*ok = end != s;
	switch (*end) {
	case '\0':
	case 's': break;
	case 'm': v *= 60; break;
	case 'h': v *= 3600; break;
	case 'd': v *= 86400; break;
	default: *ok = 0; break;
	}
	return v;
}

int gnu_timeout(int argc, char **argv)
{
	int i = 1;
	int sig = SIGTERM;
	double kill_after = 0;
	int preserve_status = 0, foreground = 0;
	double duration;
	int ok = 1;
	Vec cmd;
	int status = 0;

	for (; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] != '-' || !a[1])
			break;
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (gnu_long_opt(a, "signal", &val)) {
			sig = trap_signal_number(val ? val : argv[++i]);
			continue;
		}
		if (gnu_long_opt(a, "kill-after", &val)) {
			kill_after = parse_duration(val ? val : argv[++i], &ok);
			continue;
		}
		if (gnu_long_opt(a, "preserve-status", NULL)) {
			preserve_status = 1;
			continue;
		}
		if (gnu_long_opt(a, "foreground", NULL)) {
			foreground = 1;
			continue;
		}
		if (gnu_long_opt(a, "help", NULL)) {
			printf("usage: timeout [-s SIG] [-k DURATION] DURATION COMMAND "
			       "[ARG]...\n");
			return 0;
		}
		if (a[1] == 's') {
			sig = trap_signal_number(a[2] ? a + 2 : argv[++i]);
			continue;
		}
		if (a[1] == 'k') {
			kill_after = parse_duration(a[2] ? a + 2 : argv[++i], &ok);
			continue;
		}
		gnu_error("timeout", "invalid option -- '%s'", a);
		return 125;
	}
	(void)foreground;

	if (sig < 0) {
		gnu_error("timeout", "invalid signal");
		return 125;
	}
	if (i >= argc) {
		gnu_error("timeout", "missing operand");
		return 125;
	}
	duration = parse_duration(argv[i++], &ok);
	if (!ok) {
		gnu_error("timeout", "invalid time interval");
		return 125;
	}
	if (i >= argc) {
		gnu_error("timeout", "missing command");
		return 125;
	}

	vec_init(&cmd);
	for (; i < argc; i++)
		vec_pushs(&cmd, argv[i]);

	timed_out = 0;
	timeout_child = fork();
	if (timeout_child < 0) {
		gnu_error("timeout", "%s", strerror(errno));
		vec_free(&cmd);
		return 125;
	}
	if (timeout_child == 0) {
		char *path;

		signal(SIGALRM, SIG_DFL);
		path = path_lookup(cmd.v[0]);
		if (!path) {
			const Builtin *b = builtin_find(cmd.v[0]);
			const GnuTool *g = gnu_find(cmd.v[0]);

			if (b)
				_exit(b->fn((int)cmd.len, vec_argv(&cmd)));
			if (g)
				_exit(g->fn((int)cmd.len, vec_argv(&cmd)));
			gnu_error("timeout", "%s: No such file or directory", cmd.v[0]);
			_exit(127);
		}
		execve(path, vec_argv(&cmd), vars_environ());
		gnu_error("timeout", "%s: %s", cmd.v[0], strerror(errno));
		_exit(126);
	}

	{
		struct sigaction sa, old_alarm;
		struct itimerval it, off;
		int fired = 0;

		/* No SA_RESTART on purpose: the alarm has to interrupt waitpid
		 * below, which is how the deadline is noticed at all. */
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_alarm;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGALRM, &sa, &old_alarm);

		memset(&it, 0, sizeof it);
		it.it_value.tv_sec = (time_t)duration;
		it.it_value.tv_usec =
			(suseconds_t)((duration - (double)(time_t)duration) * 1e6);
		if (it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0)
			it.it_value.tv_usec = 1;
		setitimer(ITIMER_REAL, &it, NULL);

		for (;;) {
			pid_t r = waitpid(timeout_child, &status, 0);

			if (r == timeout_child)
				break;
			if (r < 0 && errno == EINTR) {
				if (timed_out) {
					fired = 1;
					kill(timeout_child, sig);
					if (kill_after > 0) {
						memset(&it, 0, sizeof it);
						it.it_value.tv_sec = (time_t)kill_after;
						setitimer(ITIMER_REAL, &it, NULL);
						timed_out = 0;
						if (waitpid(timeout_child, &status, 0) ==
						    timeout_child)
							break;
						kill(timeout_child, SIGKILL);
					}
				}
				continue;
			}
			if (r < 0)
				break;
		}

		/* timeout runs inside the shell, so it must leave nothing behind.
		 * A timer still armed after the command finished early used to
		 * fire seconds later in the shell itself and interrupt whatever
		 * read was in progress, so a $(...) came back empty. */
		memset(&off, 0, sizeof off);
		setitimer(ITIMER_REAL, &off, NULL);
		sigaction(SIGALRM, &old_alarm, NULL);
		if (timed_out)
			fired = 1;
		timed_out = 0;
		timeout_child = 0;

		vec_free(&cmd);
		if (fired || (WIFSIGNALED(status) && WTERMSIG(status) == sig)) {
			if (preserve_status)
				return WIFEXITED(status) ? WEXITSTATUS(status)
							 : 128 + WTERMSIG(status);
			return 124;
		}
		return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	}
}

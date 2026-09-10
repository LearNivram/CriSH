/* arith.c - the $(( )) evaluator.
 *
 * Full C operator set and precedence, including assignment, ternary,
 * pre/post increment, ** and base#digits literals.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

typedef struct {
	const char *p;
	int ok;
	int depth;
	char err[128];
} A;

static long a_comma(A *a);

static void a_fail(A *a, const char *fmt, ...)
{
	va_list ap;

	if (!a->ok)
		return;
	a->ok = 0;
	va_start(ap, fmt);
	vsnprintf(a->err, sizeof a->err, fmt, ap);
	va_end(ap);
}

static void skip_ws(A *a)
{
	while (isspace((unsigned char)*a->p))
		a->p++;
}

static int eat(A *a, const char *op)
{
	size_t n = strlen(op);

	skip_ws(a);
	if (strncmp(a->p, op, n) == 0) {
		/* do not let < match <<, + match ++ and so on */
		char next = a->p[n];
		if (n == 1) {
			char c = op[0];
			if ((c == '<' || c == '>' || c == '+' || c == '-' || c == '&' ||
			     c == '|' || c == '*' || c == '=' || c == '!' || c == '/' ||
			     c == '%' || c == '^') &&
			    (next == '=' || next == c))
				return 0;
		}
		if (n == 2 && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0 ||
			       strcmp(op, "**") == 0) &&
		    next == '=')
			return 0;
		a->p += n;
		return 1;
	}
	return 0;
}

/* Read NAME or NAME[subscript]; returns an owned key string, or NULL. */
static char *read_lvalue(A *a, char **subscript)
{
	const char *start;
	char *name;

	skip_ws(a);
	*subscript = NULL;
	if (!is_name_char((unsigned char)*a->p, 1))
		return NULL;
	start = a->p;
	while (is_name_char((unsigned char)*a->p, 0))
		a->p++;
	name = xstrndup(start, (size_t)(a->p - start));
	if (*a->p == '[') {
		int depth = 0;
		const char *sub_start;
		a->p++;
		sub_start = a->p;
		depth = 1;
		while (*a->p && depth) {
			if (*a->p == '[')
				depth++;
			else if (*a->p == ']')
				if (--depth == 0)
					break;
			a->p++;
		}
		*subscript = xstrndup(sub_start, (size_t)(a->p - sub_start));
		if (*a->p == ']')
			a->p++;
	}
	return name;
}

static long lvalue_get(const char *name, const char *sub)
{
	const char *val;
	int ok = 1;
	long n;

	if (sub) {
		char *idx = expand_to_string(sub);
		Var *v = var_find(name);
		if (v && (v->flags & V_ASSOC)) {
			val = var_array_get(name, idx);
		} else {
			val = var_array_get(name, idx);
		}
		free(idx);
	} else {
		val = var_get(name);
	}
	if (!val || !*val)
		return 0;
	n = arith_eval(val, &ok);
	return ok ? n : 0;
}

static void lvalue_set(const char *name, const char *sub, long value)
{
	char buf[32];

	snprintf(buf, sizeof buf, "%ld", value);
	if (sub) {
		char *idx = expand_to_string(sub);
		var_array_set(name, idx, buf, 0);
		free(idx);
	} else {
		var_set(name, buf, 0);
	}
}

static long parse_number(A *a)
{
	const char *p = a->p;
	long base = 10, value = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		base = 16;
	} else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') &&
		   (p[2] == '0' || p[2] == '1')) {
		p += 2;
		base = 2;
	} else {
		const char *q = p;
		long b = 0;
		while (isdigit((unsigned char)*q)) {
			b = b * 10 + (*q - '0');
			q++;
		}
		if (*q == '#' && q > p && b >= 2 && b <= 64) {
			base = b;
			p = q + 1;
		} else if (p[0] == '0' && isdigit((unsigned char)p[1])) {
			base = 8;
			p++;
		}
	}

	for (;;) {
		int c = (unsigned char)*p, d;

		if (isdigit(c))
			d = c - '0';
		else if (isupper(c))
			d = base > 36 ? c - 'A' + 36 : c - 'A' + 10;
		else if (islower(c))
			d = c - 'a' + 10;
		else if (c == '@')
			d = 62;
		else if (c == '_')
			d = 63;
		else
			break;
		if (d >= base)
			break;
		value = value * base + d;
		p++;
	}
	a->p = p;
	return value;
}

static long a_unary(A *a);

static long a_primary(A *a)
{
	long v;

	skip_ws(a);
	if (*a->p == '(') {
		a->p++;
		v = a_comma(a);
		skip_ws(a);
		if (*a->p == ')')
			a->p++;
		else
			a_fail(a, "missing `)'");
		return v;
	}
	if (isdigit((unsigned char)*a->p))
		return parse_number(a);
	if (is_name_char((unsigned char)*a->p, 1)) {
		char *sub;
		char *name = read_lvalue(a, &sub);
		long cur;

		if (!name) {
			a_fail(a, "syntax error");
			return 0;
		}
		skip_ws(a);
		if (a->p[0] == '+' && a->p[1] == '+') {
			a->p += 2;
			cur = lvalue_get(name, sub);
			lvalue_set(name, sub, cur + 1);
			free(name);
			free(sub);
			return cur;
		}
		if (a->p[0] == '-' && a->p[1] == '-') {
			a->p += 2;
			cur = lvalue_get(name, sub);
			lvalue_set(name, sub, cur - 1);
			free(name);
			free(sub);
			return cur;
		}
		/* assignment operators */
		{
			static const struct {
				const char *op;
				int kind;
			} ops[] = { { "=", 0 },   { "+=", 1 },  { "-=", 2 },	{ "*=", 3 },
				    { "/=", 4 },  { "%=", 5 },  { "<<=", 6 }, { ">>=", 7 },
				    { "&=", 8 },  { "^=", 9 },  { "|=", 10 }, { "**=", 11 },
				    { NULL, 0 } };
			int i;
			const char *save = a->p;

			for (i = 0; ops[i].op; i++) {
				size_t n = strlen(ops[i].op);
				if (strncmp(a->p, ops[i].op, n) == 0) {
					if (ops[i].kind == 0 && a->p[1] == '=')
						break; /* == is a comparison */
					a->p += n;
					{
						long rhs = a_comma(a);
						long left = ops[i].kind ? lvalue_get(name, sub) : 0;
						long res = rhs;
						switch (ops[i].kind) {
						case 1: res = left + rhs; break;
						case 2: res = left - rhs; break;
						case 3: res = left * rhs; break;
						case 4:
							if (!rhs) {
								a_fail(a, "division by 0");
								res = 0;
							} else {
								res = left / rhs;
							}
							break;
						case 5:
							if (!rhs) {
								a_fail(a, "division by 0");
								res = 0;
							} else {
								res = left % rhs;
							}
							break;
						case 6: res = left << rhs; break;
						case 7: res = left >> rhs; break;
						case 8: res = left & rhs; break;
						case 9: res = left ^ rhs; break;
						case 10: res = left | rhs; break;
						case 11: {
							long r = 1, e = rhs;
							while (e-- > 0)
								r *= left;
							res = r;
							break;
						}
						default: break;
						}
						lvalue_set(name, sub, res);
						free(name);
						free(sub);
						return res;
					}
				}
			}
			a->p = save;
		}
		v = lvalue_get(name, sub);
		free(name);
		free(sub);
		return v;
	}
	a_fail(a, "syntax error near `%s'", a->p);
	return 0;
}

static long a_unary(A *a)
{
	skip_ws(a);
	if (a->p[0] == '+' && a->p[1] == '+') {
		char *sub, *name;
		long v;
		a->p += 2;
		name = read_lvalue(a, &sub);
		if (!name) {
			a_fail(a, "syntax error after `++'");
			return 0;
		}
		v = lvalue_get(name, sub) + 1;
		lvalue_set(name, sub, v);
		free(name);
		free(sub);
		return v;
	}
	if (a->p[0] == '-' && a->p[1] == '-') {
		char *sub, *name;
		long v;
		a->p += 2;
		name = read_lvalue(a, &sub);
		if (!name) {
			a_fail(a, "syntax error after `--'");
			return 0;
		}
		v = lvalue_get(name, sub) - 1;
		lvalue_set(name, sub, v);
		free(name);
		free(sub);
		return v;
	}
	if (eat(a, "!"))
		return !a_unary(a);
	if (eat(a, "~"))
		return ~a_unary(a);
	if (eat(a, "-"))
		return -a_unary(a);
	if (eat(a, "+"))
		return a_unary(a);
	return a_primary(a);
}

static long a_power(A *a)
{
	long base = a_unary(a);

	skip_ws(a);
	if (a->p[0] == '*' && a->p[1] == '*' && a->p[2] != '=') {
		long e, r = 1;
		a->p += 2;
		e = a_power(a); /* right associative */
		if (e < 0)
			return 0;
		while (e-- > 0)
			r *= base;
		return r;
	}
	return base;
}

static long a_mul(A *a)
{
	long v = a_power(a);

	for (;;) {
		skip_ws(a);
		if (eat(a, "*")) {
			v *= a_power(a);
		} else if (eat(a, "/")) {
			long d = a_power(a);
			if (!d) {
				a_fail(a, "division by 0");
				return 0;
			}
			v /= d;
		} else if (eat(a, "%")) {
			long d = a_power(a);
			if (!d) {
				a_fail(a, "division by 0");
				return 0;
			}
			v %= d;
		} else {
			return v;
		}
	}
}

static long a_add(A *a)
{
	long v = a_mul(a);

	for (;;) {
		skip_ws(a);
		if (eat(a, "+"))
			v += a_mul(a);
		else if (eat(a, "-"))
			v -= a_mul(a);
		else
			return v;
	}
}

static long a_shift(A *a)
{
	long v = a_add(a);

	for (;;) {
		skip_ws(a);
		if (eat(a, "<<"))
			v <<= a_add(a);
		else if (eat(a, ">>"))
			v >>= a_add(a);
		else
			return v;
	}
}

static long a_rel(A *a)
{
	long v = a_shift(a);

	for (;;) {
		skip_ws(a);
		if (eat(a, "<="))
			v = v <= a_shift(a);
		else if (eat(a, ">="))
			v = v >= a_shift(a);
		else if (eat(a, "<"))
			v = v < a_shift(a);
		else if (eat(a, ">"))
			v = v > a_shift(a);
		else
			return v;
	}
}

static long a_eq(A *a)
{
	long v = a_rel(a);

	for (;;) {
		skip_ws(a);
		if (eat(a, "=="))
			v = v == a_rel(a);
		else if (eat(a, "!="))
			v = v != a_rel(a);
		else
			return v;
	}
}

static long a_band(A *a)
{
	long v = a_eq(a);

	while (1) {
		skip_ws(a);
		if (a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') {
			a->p++;
			v &= a_eq(a);
		} else {
			return v;
		}
	}
}

static long a_bxor(A *a)
{
	long v = a_band(a);

	while (1) {
		skip_ws(a);
		if (a->p[0] == '^' && a->p[1] != '=') {
			a->p++;
			v ^= a_band(a);
		} else {
			return v;
		}
	}
}

static long a_bor(A *a)
{
	long v = a_bxor(a);

	while (1) {
		skip_ws(a);
		if (a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') {
			a->p++;
			v |= a_bxor(a);
		} else {
			return v;
		}
	}
}

static long a_and(A *a)
{
	long v = a_bor(a);

	for (;;) {
		skip_ws(a);
		if (a->p[0] == '&' && a->p[1] == '&') {
			a->p += 2;
			v = a_bor(a) && v;
		} else {
			return v;
		}
	}
}

static long a_or(A *a)
{
	long v = a_and(a);

	for (;;) {
		skip_ws(a);
		if (a->p[0] == '|' && a->p[1] == '|') {
			a->p += 2;
			v = a_and(a) || v;
		} else {
			return v;
		}
	}
}

static long a_ternary(A *a)
{
	long cond = a_or(a);

	skip_ws(a);
	if (*a->p == '?') {
		long yes, no;
		a->p++;
		yes = a_ternary(a);
		skip_ws(a);
		if (*a->p == ':')
			a->p++;
		else
			a_fail(a, "expected `:' in conditional expression");
		no = a_ternary(a);
		return cond ? yes : no;
	}
	return cond;
}

static long a_comma(A *a)
{
	long v = a_ternary(a);

	for (;;) {
		skip_ws(a);
		if (*a->p == ',') {
			a->p++;
			v = a_ternary(a);
		} else {
			return v;
		}
	}
}

long arith_eval(const char *expr, int *ok)
{
	A a;
	char *expanded;
	long v;

	if (!expr)
		expr = "";
	/* $ substitutions and quotes inside the expression are resolved first */
	expanded = expand_to_string(expr);

	a.p = expanded;
	a.ok = 1;
	a.depth = 0;
	a.err[0] = '\0';

	skip_ws(&a);
	if (!*a.p) {
		free(expanded);
		if (ok)
			*ok = 1;
		return 0;
	}
	v = a_comma(&a);
	skip_ws(&a);
	if (a.ok && *a.p)
		a_fail(&a, "syntax error near `%s'", a.p);
	if (!a.ok) {
		shell_error("%s: %s", expr, a.err);
		if (ok)
			*ok = 0;
		free(expanded);
		return 0;
	}
	free(expanded);
	if (ok)
		*ok = 1;
	return v;
}

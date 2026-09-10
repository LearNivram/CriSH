/* regex.c - a backtracking regular expression engine.
 *
 * The pattern compiles to a chain of nodes; matching walks the chain with an
 * explicit continuation list so that alternation, repetition and capture all
 * backtrack without the C stack having to model the whole pattern.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "regex.h"
#include "util.h"

enum {
	N_CHAR,
	N_ANY,
	N_CLASS,
	N_ALT,
	N_REP,
	N_OPEN,
	N_CLOSE,
	N_BACKREF,
	N_BOL,
	N_EOL,
	N_WORDB,  /* \b */
	N_NWORDB, /* \B */
	N_WSTART, /* \< */
	N_WEND,   /* \> */
	N_BUFSTART,
	N_BUFEND
};

typedef struct RNode {
	int type;
	int c;                   /* N_CHAR */
	unsigned char set[32];   /* N_CLASS bitmap */
	int negate;              /* N_CLASS */
	int group;               /* N_OPEN/N_CLOSE/N_BACKREF */
	int min, max, greedy;    /* N_REP */
	struct RNode *child;     /* N_REP body */
	struct RNode **branches; /* N_ALT */
	size_t nbranches;
	struct RNode *next;
	struct RNode *alloc_next; /* every node, for freeing */
} RNode;

struct Rx {
	RNode *prog;
	RNode *all;
	int ngroups;
	int flags;
	int mode;
	int anchored_start;
};

/* --------------------------------------------------------------- compiler */

typedef struct {
	const char *p;
	Rx *rx;
	int mode;
	int flags;
	const char *err;
	int group_counter;
} Cx;

static RNode *node(Cx *cx, int type)
{
	RNode *n = xcalloc(1, sizeof *n);

	n->type = type;
	n->max = 1;
	n->min = 1;
	n->greedy = 1;
	n->alloc_next = cx->rx->all;
	cx->rx->all = n;
	return n;
}

static void set_add(unsigned char *set, int c)
{
	set[(c & 0xff) >> 3] |= (unsigned char)(1 << (c & 7));
}

static int set_has(const unsigned char *set, int c)
{
	return (set[(c & 0xff) >> 3] >> (c & 7)) & 1;
}

static void set_range(unsigned char *set, int lo, int hi)
{
	int c;

	for (c = lo; c <= hi; c++)
		set_add(set, c);
}

static void set_class(unsigned char *set, const char *name, size_t len)
{
	int c;

	for (c = 0; c < 256; c++) {
		int hit = 0;

		if (len == 5 && !strncmp(name, "alpha", 5))
			hit = isalpha(c);
		else if (len == 5 && !strncmp(name, "digit", 5))
			hit = isdigit(c);
		else if (len == 5 && !strncmp(name, "alnum", 5))
			hit = isalnum(c);
		else if (len == 5 && !strncmp(name, "upper", 5))
			hit = isupper(c);
		else if (len == 5 && !strncmp(name, "lower", 5))
			hit = islower(c);
		else if (len == 5 && !strncmp(name, "space", 5))
			hit = isspace(c);
		else if (len == 5 && !strncmp(name, "blank", 5))
			hit = (c == ' ' || c == '\t');
		else if (len == 5 && !strncmp(name, "punct", 5))
			hit = ispunct(c);
		else if (len == 5 && !strncmp(name, "print", 5))
			hit = isprint(c);
		else if (len == 5 && !strncmp(name, "graph", 5))
			hit = isgraph(c);
		else if (len == 5 && !strncmp(name, "cntrl", 5))
			hit = iscntrl(c);
		else if (len == 6 && !strncmp(name, "xdigit", 6))
			hit = isxdigit(c);
		else if (len == 4 && !strncmp(name, "word", 4))
			hit = isalnum(c) || c == '_';
		if (hit)
			set_add(set, c);
	}
}

/* \d \w \s and friends inside and outside a bracket expression. */
static int escape_class(int c, unsigned char *set)
{
	int i;

	switch (c) {
	case 'd':
		set_range(set, '0', '9');
		return 1;
	case 'D':
		for (i = 0; i < 256; i++)
			if (!isdigit(i))
				set_add(set, i);
		return 1;
	case 'w':
		for (i = 0; i < 256; i++)
			if (isalnum(i) || i == '_')
				set_add(set, i);
		return 1;
	case 'W':
		for (i = 0; i < 256; i++)
			if (!(isalnum(i) || i == '_'))
				set_add(set, i);
		return 1;
	case 's':
		for (i = 0; i < 256; i++)
			if (isspace(i))
				set_add(set, i);
		return 1;
	case 'S':
		for (i = 0; i < 256; i++)
			if (!isspace(i))
				set_add(set, i);
		return 1;
	default:
		return 0;
	}
}

static int escape_char(int c)
{
	switch (c) {
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case 'f': return '\f';
	case 'v': return '\v';
	case 'a': return '\a';
	case 'e': return 0x1b;
	case '0': return '\0';
	default: return -1;
	}
}

static RNode *parse_alt(Cx *cx);

static RNode *parse_bracket(Cx *cx)
{
	RNode *n = node(cx, N_CLASS);
	const char *p = cx->p + 1;

	if (*p == '^') {
		n->negate = 1;
		p++;
	}
	if (*p == ']') {
		set_add(n->set, ']');
		p++;
	}
	while (*p && *p != ']') {
		int lo;

		if (*p == '[' && p[1] == ':') {
			const char *close = strstr(p, ":]");
			if (close) {
				set_class(n->set, p + 2, (size_t)(close - p - 2));
				p = close + 2;
				continue;
			}
		}
		if (*p == '\\' && p[1]) {
			unsigned char tmp[32];
			int e;

			memset(tmp, 0, sizeof tmp);
			if (escape_class((unsigned char)p[1], tmp)) {
				int i;
				for (i = 0; i < 32; i++)
					n->set[i] |= tmp[i];
				p += 2;
				continue;
			}
			e = escape_char((unsigned char)p[1]);
			lo = e >= 0 ? e : (unsigned char)p[1];
			p += 2;
		} else {
			lo = (unsigned char)*p;
			p++;
		}
		if (*p == '-' && p[1] && p[1] != ']') {
			int hi;
			p++;
			if (*p == '\\' && p[1]) {
				int e = escape_char((unsigned char)p[1]);
				hi = e >= 0 ? e : (unsigned char)p[1];
				p += 2;
			} else {
				hi = (unsigned char)*p;
				p++;
			}
			set_range(n->set, lo, hi);
		} else {
			set_add(n->set, lo);
		}
	}
	if (*p != ']') {
		cx->err = "unterminated [";
		return NULL;
	}
	cx->p = p + 1;
	if (cx->flags & RX_ICASE) {
		int c;
		for (c = 'a'; c <= 'z'; c++) {
			if (set_has(n->set, c))
				set_add(n->set, toupper(c));
			if (set_has(n->set, toupper(c)))
				set_add(n->set, c);
		}
	}
	return n;
}

/* Is the current position a group open in this flavour? */
static int at_group_open(Cx *cx)
{
	if (cx->mode == RX_BRE)
		return cx->p[0] == '\\' && cx->p[1] == '(';
	return cx->p[0] == '(';
}

static int at_group_close(Cx *cx)
{
	if (cx->mode == RX_BRE)
		return cx->p[0] == '\\' && cx->p[1] == ')';
	return cx->p[0] == ')';
}

static int at_alt(Cx *cx)
{
	if (cx->mode == RX_BRE)
		return cx->p[0] == '\\' && cx->p[1] == '|';
	return cx->p[0] == '|';
}

static RNode *parse_atom(Cx *cx, int *is_anchor)
{
	int c = (unsigned char)*cx->p;

	*is_anchor = 0;

	if (at_group_open(cx)) {
		RNode *open, *close, *body, *tail;
		int idx;
		int capture = 1;

		cx->p += cx->mode == RX_BRE ? 2 : 1;
		if (cx->mode == RX_PCRE && cx->p[0] == '?' && cx->p[1] == ':') {
			capture = 0;
			cx->p += 2;
		}
		idx = capture ? ++cx->group_counter : 0;
		if (idx >= RX_MAX_GROUPS) {
			cx->err = "too many groups";
			return NULL;
		}
		body = parse_alt(cx);
		if (cx->err)
			return NULL;
		if (!at_group_close(cx)) {
			cx->err = "unmatched (";
			return NULL;
		}
		cx->p += cx->mode == RX_BRE ? 2 : 1;

		if (!capture) {
			/* (?:...) needs one node so that a quantifier has a body */
			RNode *wrap = node(cx, N_REP);
			wrap->child = body;
			wrap->min = 1;
			wrap->max = 1;
			return wrap;
		}
		open = node(cx, N_OPEN);
		close = node(cx, N_CLOSE);
		open->group = idx;
		close->group = idx;
		open->next = body ? body : close;
		if (body) {
			tail = body;
			while (tail->next)
				tail = tail->next;
			tail->next = close;
		}
		{
			/* a quantifier must repeat the whole group, so wrap it */
			RNode *wrap = node(cx, N_REP);
			wrap->child = open;
			wrap->min = 1;
			wrap->max = 1;
			return wrap;
		}
	}

	if (c == '[')
		return parse_bracket(cx);

	if (c == '.') {
		RNode *n = node(cx, N_ANY);
		cx->p++;
		return n;
	}

	if (c == '^') {
		RNode *n = node(cx, N_BOL);
		cx->p++;
		*is_anchor = 1;
		return n;
	}
	if (c == '$') {
		const char *after = cx->p + 1;
		int at_end = *after == '\0' || at_alt(cx) ||
			     (cx->mode != RX_BRE && *after == ')') ||
			     (cx->mode == RX_BRE && after[0] == '\\' && after[1] == ')');

		if (cx->mode != RX_BRE || at_end) {
			RNode *n = node(cx, N_EOL);
			cx->p++;
			*is_anchor = 1;
			return n;
		}
	}

	if (c == '\\') {
		int e = (unsigned char)cx->p[1];
		unsigned char tmp[32];

		memset(tmp, 0, sizeof tmp);
		if (escape_class(e, tmp)) {
			RNode *n = node(cx, N_CLASS);
			memcpy(n->set, tmp, sizeof tmp);
			cx->p += 2;
			return n;
		}
		switch (e) {
		case 'b': {
			RNode *n = node(cx, N_WORDB);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		case 'B': {
			RNode *n = node(cx, N_NWORDB);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		case '<': {
			RNode *n = node(cx, N_WSTART);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		case '>': {
			RNode *n = node(cx, N_WEND);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		case 'A': {
			RNode *n = node(cx, N_BUFSTART);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		case 'z':
		case 'Z': {
			RNode *n = node(cx, N_BUFEND);
			cx->p += 2;
			*is_anchor = 1;
			return n;
		}
		default:
			break;
		}
		if (e >= '1' && e <= '9') {
			RNode *n = node(cx, N_BACKREF);
			n->group = e - '0';
			cx->p += 2;
			return n;
		}
		if (e == 'x' && isxdigit((unsigned char)cx->p[2])) {
			int v = 0, k = 0;
			cx->p += 2;
			while (k < 2 && isxdigit((unsigned char)*cx->p)) {
				int d = tolower((unsigned char)*cx->p);
				v = v * 16 + (isdigit(d) ? d - '0' : d - 'a' + 10);
				cx->p++;
				k++;
			}
			{
				RNode *n = node(cx, N_CHAR);
				n->c = v;
				return n;
			}
		}
		{
			int lit = escape_char(e);
			RNode *n = node(cx, N_CHAR);
			n->c = lit >= 0 ? lit : e;
			cx->p += e ? 2 : 1;
			return n;
		}
	}

	{
		RNode *n = node(cx, N_CHAR);
		n->c = c;
		cx->p++;
		return n;
	}
}

/* {m,n} in ERE/PCRE, \{m,n\} in BRE */
static int parse_brace(Cx *cx, int *min, int *max)
{
	const char *p = cx->p;
	int m = 0, n = -1;
	int have = 0;

	if (cx->mode == RX_BRE) {
		if (!(p[0] == '\\' && p[1] == '{'))
			return 0;
		p += 2;
	} else {
		if (p[0] != '{')
			return 0;
		p++;
	}
	if (!isdigit((unsigned char)*p) && *p != ',')
		return 0;
	while (isdigit((unsigned char)*p)) {
		m = m * 10 + (*p - '0');
		p++;
		have = 1;
	}
	if (*p == ',') {
		p++;
		if (isdigit((unsigned char)*p)) {
			n = 0;
			while (isdigit((unsigned char)*p)) {
				n = n * 10 + (*p - '0');
				p++;
			}
		} else {
			n = INT_MAX;
		}
	} else {
		n = m;
	}
	if (!have && n < 0)
		return 0;
	if (cx->mode == RX_BRE) {
		if (!(p[0] == '\\' && p[1] == '}'))
			return 0;
		p += 2;
	} else {
		if (*p != '}')
			return 0;
		p++;
	}
	*min = m;
	*max = n;
	cx->p = p;
	return 1;
}

static RNode *wrap_rep(Cx *cx, RNode *atom, int min, int max)
{
	RNode *rep = node(cx, N_REP);

	rep->child = atom;
	rep->min = min;
	rep->max = max;
	rep->greedy = 1;
	if (cx->mode == RX_PCRE && *cx->p == '?') {
		rep->greedy = 0;
		cx->p++;
	}
	return rep;
}

static RNode *parse_concat(Cx *cx)
{
	RNode *head = NULL, **tail = &head;

	while (*cx->p && !at_alt(cx) && !at_group_close(cx)) {
		int is_anchor = 0;
		RNode *atom = parse_atom(cx, &is_anchor);
		int min, max;

		if (cx->err)
			return NULL;
		if (!atom)
			return NULL;

		for (;;) {
			if (!is_anchor && *cx->p == '*') {
				cx->p++;
				atom = wrap_rep(cx, atom, 0, INT_MAX);
				continue;
			}
			if (!is_anchor && cx->mode != RX_BRE && *cx->p == '+') {
				cx->p++;
				atom = wrap_rep(cx, atom, 1, INT_MAX);
				continue;
			}
			if (!is_anchor && cx->mode != RX_BRE && *cx->p == '?') {
				cx->p++;
				atom = wrap_rep(cx, atom, 0, 1);
				continue;
			}
			if (!is_anchor && cx->mode == RX_BRE && cx->p[0] == '\\' &&
			    cx->p[1] == '+') {
				cx->p += 2;
				atom = wrap_rep(cx, atom, 1, INT_MAX);
				continue;
			}
			if (!is_anchor && cx->mode == RX_BRE && cx->p[0] == '\\' &&
			    cx->p[1] == '?') {
				cx->p += 2;
				atom = wrap_rep(cx, atom, 0, 1);
				continue;
			}
			if (!is_anchor && parse_brace(cx, &min, &max)) {
				atom = wrap_rep(cx, atom, min, max);
				continue;
			}
			break;
		}
		*tail = atom;
		while (*tail)
			tail = &(*tail)->next;
	}
	return head;
}

static RNode *parse_alt(Cx *cx)
{
	RNode *first = parse_concat(cx);
	RNode *alt;

	if (cx->err)
		return NULL;
	if (!at_alt(cx))
		return first;

	alt = node(cx, N_ALT);
	alt->branches = xcalloc(8, sizeof *alt->branches);
	alt->nbranches = 0;
	{
		size_t cap = 8;
		alt->branches[alt->nbranches++] = first;
		while (at_alt(cx)) {
			RNode *branch;
			cx->p += cx->mode == RX_BRE ? 2 : 1;
			branch = parse_concat(cx);
			if (cx->err)
				return NULL;
			if (alt->nbranches == cap) {
				cap *= 2;
				alt->branches = xrealloc(alt->branches,
							 cap * sizeof *alt->branches);
			}
			alt->branches[alt->nbranches++] = branch;
		}
	}
	return alt;
}

Rx *rx_compile(const char *pattern, int mode, int flags, const char **err)
{
	Rx *rx = xcalloc(1, sizeof *rx);
	Cx cx;

	cx.p = pattern;
	cx.rx = rx;
	cx.mode = mode;
	cx.flags = flags;
	cx.err = NULL;
	cx.group_counter = 0;
	rx->flags = flags;
	rx->mode = mode;

	rx->prog = parse_alt(&cx);
	if (cx.err || *cx.p) {
		if (err)
			*err = cx.err ? cx.err : "trailing characters in pattern";
		rx_free(rx);
		return NULL;
	}
	rx->ngroups = cx.group_counter;
	if (err)
		*err = NULL;
	return rx;
}

void rx_free(Rx *rx)
{
	RNode *n;

	if (!rx)
		return;
	n = rx->all;
	while (n) {
		RNode *next = n->alloc_next;
		free(n->branches);
		free(n);
		n = next;
	}
	free(rx);
}

int rx_ngroups(const Rx *rx)
{
	return rx ? rx->ngroups : 0;
}

/* ---------------------------------------------------------------- matcher */

typedef struct RepCtx RepCtx;

typedef struct Kont {
	RNode *node;
	RepCtx *rep;
	struct Kont *next;
} Kont;

struct RepCtx {
	RNode *rep;
	int count;
	const char *start;
	Kont *k;
};

typedef struct {
	Rx *rx;
	const char *base;
	const char *end;
	long gstart[RX_MAX_GROUPS];
	long gend[RX_MAX_GROUPS];
	const char *mend;
	int steps;
	/* POSIX wants the longest match, Perl the first one the engine finds.
	 * In longest mode a success is recorded and then rejected, so that
	 * backtracking keeps going and every alternative is seen. */
	int longest;
	int found;
	long bstart[RX_MAX_GROUPS];
	long bend[RX_MAX_GROUPS];
} Mx;

#define RX_STEP_LIMIT 4000000

static int m_chain(Mx *mx, RNode *n, Kont *k, const char *s);

static int is_word(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static int pop_kont(Mx *mx, Kont *k, const char *s)
{
	if (!k) {
		if (mx->longest) {
			if (!mx->found || s > mx->mend) {
				int i;

				mx->mend = s;
				for (i = 0; i < RX_MAX_GROUPS; i++) {
					mx->bstart[i] = mx->gstart[i];
					mx->bend[i] = mx->gend[i];
				}
			}
			mx->found = 1;
			return 0; /* force further backtracking */
		}
		mx->mend = s;
		return 1;
	}
	if (k->rep) {
		RepCtx *ctx = k->rep;
		RNode *rep = ctx->rep;
		int count = ctx->count;

		/* an iteration that consumed nothing must not loop forever */
		if (s == ctx->start) {
			if (count >= rep->min)
				return m_chain(mx, rep->next, ctx->k, s);
			return 0;
		}
		{
			int can_more = count < rep->max;
			int can_stop = count >= rep->min;

			if (rep->greedy) {
				if (can_more) {
					RepCtx nctx;
					Kont kk;

					nctx.rep = rep;
					nctx.count = count + 1;
					nctx.start = s;
					nctx.k = ctx->k;
					kk.node = NULL;
					kk.rep = &nctx;
					kk.next = NULL;
					if (m_chain(mx, rep->child, &kk, s))
						return 1;
				}
				if (can_stop)
					return m_chain(mx, rep->next, ctx->k, s);
				return 0;
			}
			if (can_stop && m_chain(mx, rep->next, ctx->k, s))
				return 1;
			if (can_more) {
				RepCtx nctx;
				Kont kk;

				nctx.rep = rep;
				nctx.count = count + 1;
				nctx.start = s;
				nctx.k = ctx->k;
				kk.node = NULL;
				kk.rep = &nctx;
				kk.next = NULL;
				return m_chain(mx, rep->child, &kk, s);
			}
			return 0;
		}
	}
	return m_chain(mx, k->node, k->next, s);
}

static int m_chain(Mx *mx, RNode *n, Kont *k, const char *s)
{
	if (++mx->steps > RX_STEP_LIMIT)
		return 0;

	while (n) {
		switch (n->type) {
		case N_CHAR: {
			int a, b;
			if (s >= mx->end)
				return 0;
			a = (unsigned char)*s;
			b = n->c;
			if (mx->rx->flags & RX_ICASE) {
				a = tolower(a);
				b = tolower(b);
			}
			if (a != b)
				return 0;
			s++;
			n = n->next;
			continue;
		}
		case N_ANY:
			if (s >= mx->end)
				return 0;
			if ((mx->rx->flags & RX_NEWLINE) && *s == '\n')
				return 0;
			s++;
			n = n->next;
			continue;
		case N_CLASS: {
			int c;
			if (s >= mx->end)
				return 0;
			c = (unsigned char)*s;
			{
				int hit = set_has(n->set, c);
				if ((mx->rx->flags & RX_ICASE) && !hit)
					hit = set_has(n->set, isupper(c) ? tolower(c)
									 : toupper(c));
				if (n->negate) {
					if (hit)
						return 0;
					if ((mx->rx->flags & RX_NEWLINE) && c == '\n')
						return 0;
				} else if (!hit) {
					return 0;
				}
			}
			s++;
			n = n->next;
			continue;
		}
		case N_BOL:
			if (s == mx->base)
				; /* ok */
			else if ((mx->rx->flags & RX_MULTILINE) && s[-1] == '\n')
				; /* ok */
			else
				return 0;
			n = n->next;
			continue;
		case N_EOL:
			if (s == mx->end)
				; /* ok */
			else if ((mx->rx->flags & RX_MULTILINE) && *s == '\n')
				; /* ok */
			else
				return 0;
			n = n->next;
			continue;
		case N_BUFSTART:
			if (s != mx->base)
				return 0;
			n = n->next;
			continue;
		case N_BUFEND:
			if (s != mx->end)
				return 0;
			n = n->next;
			continue;
		case N_WORDB: {
			int before = s > mx->base && is_word(s[-1]);
			int after = s < mx->end && is_word(*s);
			if (before == after)
				return 0;
			n = n->next;
			continue;
		}
		case N_NWORDB: {
			int before = s > mx->base && is_word(s[-1]);
			int after = s < mx->end && is_word(*s);
			if (before != after)
				return 0;
			n = n->next;
			continue;
		}
		case N_WSTART: {
			int before = s > mx->base && is_word(s[-1]);
			int after = s < mx->end && is_word(*s);
			if (before || !after)
				return 0;
			n = n->next;
			continue;
		}
		case N_WEND: {
			int before = s > mx->base && is_word(s[-1]);
			int after = s < mx->end && is_word(*s);
			if (!before || after)
				return 0;
			n = n->next;
			continue;
		}
		case N_OPEN: {
			long save = mx->gstart[n->group];

			mx->gstart[n->group] = s - mx->base;
			if (m_chain(mx, n->next, k, s))
				return 1;
			mx->gstart[n->group] = save;
			return 0;
		}
		case N_CLOSE: {
			long save = mx->gend[n->group];

			mx->gend[n->group] = s - mx->base;
			if (m_chain(mx, n->next, k, s))
				return 1;
			mx->gend[n->group] = save;
			return 0;
		}
		case N_BACKREF: {
			long a = mx->gstart[n->group], b = mx->gend[n->group];
			size_t len;

			if (a < 0 || b < a)
				return 0;
			len = (size_t)(b - a);
			if (s + len > mx->end)
				return 0;
			if (mx->rx->flags & RX_ICASE) {
				size_t i;
				for (i = 0; i < len; i++)
					if (tolower((unsigned char)s[i]) !=
					    tolower((unsigned char)mx->base[a + i]))
						return 0;
			} else if (memcmp(s, mx->base + a, len) != 0) {
				return 0;
			}
			s += len;
			n = n->next;
			continue;
		}
		case N_ALT: {
			Kont kk;
			size_t i;

			kk.node = n->next;
			kk.rep = NULL;
			kk.next = k;
			for (i = 0; i < n->nbranches; i++) {
				if (n->branches[i]) {
					if (m_chain(mx, n->branches[i], &kk, s))
						return 1;
				} else {
					if (m_chain(mx, n->next, k, s))
						return 1;
				}
			}
			return 0;
		}
		case N_REP: {
			RNode *rep = n;
			int can_more = rep->max > 0;
			int can_stop = rep->min == 0;

			if (rep->greedy) {
				if (can_more) {
					RepCtx ctx;
					Kont kk;

					ctx.rep = rep;
					ctx.count = 1;
					ctx.start = s;
					ctx.k = k;
					kk.node = NULL;
					kk.rep = &ctx;
					kk.next = NULL;
					if (m_chain(mx, rep->child, &kk, s))
						return 1;
				}
				if (can_stop)
					return m_chain(mx, rep->next, k, s);
				return 0;
			}
			if (can_stop && m_chain(mx, rep->next, k, s))
				return 1;
			if (can_more) {
				RepCtx ctx;
				Kont kk;

				ctx.rep = rep;
				ctx.count = 1;
				ctx.start = s;
				ctx.k = k;
				kk.node = NULL;
				kk.rep = &ctx;
				kk.next = NULL;
				return m_chain(mx, rep->child, &kk, s);
			}
			return 0;
		}
		default:
			return 0;
		}
	}
	return pop_kont(mx, k, s);
}

static void mx_reset(Mx *mx)
{
	int i;

	for (i = 0; i < RX_MAX_GROUPS; i++) {
		mx->gstart[i] = -1;
		mx->gend[i] = -1;
		mx->bstart[i] = -1;
		mx->bend[i] = -1;
	}
	mx->mend = NULL;
	mx->found = 0;
}

int rx_search(Rx *rx, const char *text, size_t len, size_t from, RxMatch *m)
{
	Mx mx;
	size_t i;

	if (!rx)
		return 0;
	mx.rx = rx;
	mx.base = text;
	mx.end = text + len;
	mx.steps = 0;
	mx.longest = rx->mode != RX_PCRE;

	for (i = from; i <= len; i++) {
		int hit;

		mx_reset(&mx);
		mx.steps = 0;
		hit = m_chain(&mx, rx->prog, NULL, text + i);
		if (mx.longest)
			hit = mx.found;
		if (hit) {
			if (m) {
				const long *gs = mx.longest ? mx.bstart : mx.gstart;
				const long *ge = mx.longest ? mx.bend : mx.gend;
				int g;

				m->start = (long)i;
				m->end = mx.mend - text;
				m->ngroups = rx->ngroups;
				for (g = 0; g <= rx->ngroups && g < RX_MAX_GROUPS; g++) {
					m->group[g].start = gs[g];
					m->group[g].end = ge[g];
				}
				m->group[0].start = m->start;
				m->group[0].end = m->end;
			}
			return 1;
		}
	}
	return 0;
}

int rx_match_full(Rx *rx, const char *text, size_t len)
{
	RxMatch m;

	if (!rx_search(rx, text, len, 0, &m))
		return 0;
	return m.start == 0 && m.end == (long)len;
}

int rx_matches(const char *pattern, const char *text, int mode, int flags)
{
	const char *err;
	Rx *rx = rx_compile(pattern, mode, flags, &err);
	int r;

	if (!rx)
		return 0;
	r = rx_search(rx, text, strlen(text), 0, NULL);
	rx_free(rx);
	return r;
}

int rx_matches_anchored(const char *pattern, const char *text, char **group)
{
	const char *err;
	Rx *rx = rx_compile(pattern, RX_BRE, 0, &err);
	RxMatch m;
	int n = 0;

	*group = NULL;
	if (!rx)
		return 0;
	if (rx_search(rx, text, strlen(text), 0, &m) && m.start == 0) {
		n = (int)m.end;
		if (rx_ngroups(rx) > 0) {
			if (m.group[1].start >= 0 && m.group[1].end >= m.group[1].start)
				*group = xstrndup(text + m.group[1].start,
						  (size_t)(m.group[1].end - m.group[1].start));
			else
				*group = xstrdup("");
		}
	} else if (rx_ngroups(rx) > 0) {
		*group = xstrdup("");
	}
	rx_free(rx);
	return n;
}

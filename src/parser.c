/* parser.c - tokenizer and recursive descent parser.
 *
 * The lexer keeps quotes in the word text and only tracks nesting, so that
 * `"$a"` and `$a` reach the expander as the same kind of object and quoting
 * is decided in one place (expand.c).
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

/* ------------------------------------------------------------------ tokens */

enum {
	T_EOF = 0,
	T_WORD,
	T_ASSIGN,
	T_IO_NUMBER,
	T_NEWLINE,
	T_SEMI,      /* ;   */
	T_DSEMI,     /* ;;  */
	T_SEMI_AMP,  /* ;&  */
	T_DSEMI_AMP, /* ;;& */
	T_AMP,       /* &   */
	T_AND_AND,   /* &&  */
	T_OR_OR,     /* ||  */
	T_PIPE,      /* |   */
	T_PIPE_AMP,  /* |&  */
	T_LPAREN,
	T_RPAREN,
	T_DLPAREN, /* ((  */
	T_LESS,
	T_GREAT,
	T_DGREAT,     /* >>  */
	T_CLOBBER,    /* >|  */
	T_LESSAND,    /* <&  */
	T_GREATAND,   /* >&  */
	T_LESSGREAT,  /* <>  */
	T_DLESS,      /* <<  */
	T_DLESSDASH,  /* <<- */
	T_TLESS,      /* <<< */
	T_ANDGREAT,   /* &>  */
	T_ANDDGREAT   /* &>> */
};

typedef struct {
	int type;
	char *text;
	int io_number;
	int lineno;
	char *varname; /* {fd}> form */
} Tok;

typedef struct PendTok {
	Tok tok;
	struct PendTok *next;
} PendTok;

typedef struct AliasFrame {
	char *name;
	struct AliasFrame *next;
} AliasFrame;

struct Parser {
	const char *src;
	size_t pos;
	size_t len;
	int lineno;
	const char *origin;

	Tok cur;
	int have_cur;
	PendTok *pending;

	/* heredocs whose bodies must be read at the next newline */
	Redir *here_queue[64];
	int here_count;

	AliasFrame *aliases;
	int incomplete;
	char *error;
};

/* ------------------------------------------------------------- tree memory */

Node *node_new(NodeType type)
{
	Node *n = xcalloc(1, sizeof *n);

	n->type = type;
	vec_init(&n->words);
	vec_init(&n->assigns);
	vec_init(&n->items);
	vec_init(&n->cond);
	n->lineno = sh.lineno;
	return n;
}

static void redir_free(Redir *r)
{
	while (r) {
		Redir *next = r->next;
		free(r->word);
		free(r->varname);
		free(r);
		r = next;
	}
}

static void case_free(CaseItem *c)
{
	while (c) {
		CaseItem *next = c->next;
		vec_free(&c->patterns);
		node_free(c->body);
		free(c);
		c = next;
	}
}

void node_free(Node *n)
{
	size_t i;

	if (!n)
		return;
	vec_free(&n->words);
	vec_free(&n->assigns);
	vec_free(&n->items);
	vec_free(&n->cond);
	redir_free(n->redir);
	for (i = 0; i < n->nkids; i++)
		node_free(n->kids[i]);
	free(n->kids);
	free(n->kidflags);
	node_free(n->left);
	node_free(n->right);
	node_free(n->body);
	node_free(n->els);
	case_free(n->cases);
	free(n->name);
	free(n->a1);
	free(n->a2);
	free(n->a3);
	free(n->word);
	free(n);
}

static void node_add_kid(Node *n, Node *kid, int flag)
{
	n->kids = xrealloc(n->kids, (n->nkids + 1) * sizeof *n->kids);
	n->kidflags = xrealloc(n->kidflags, (n->nkids + 1) * sizeof *n->kidflags);
	n->kids[n->nkids] = kid;
	n->kidflags[n->nkids] = flag;
	n->nkids++;
}

static void node_add_redir(Node *n, Redir *r)
{
	Redir **p = &n->redir;

	while (*p)
		p = &(*p)->next;
	*p = r;
}

/* ------------------------------------------------------------------- lexer */

static int at(Parser *p, size_t off)
{
	return p->pos + off < p->len ? (unsigned char)p->src[p->pos + off] : -1;
}

static int is_meta(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
	       c == ';' || c == '(' || c == ')' || c == '<' || c == '>';
}

static void perror_at(Parser *p, const char *fmt, ...)
{
	Buf b;
	va_list ap;

	if (p->error)
		return;
	buf_init(&b);
	buf_printf(&b, "%s: line %d: ", p->origin ? p->origin : "crish", p->lineno);
	va_start(ap, fmt);
	buf_vprintf(&b, fmt, ap);
	va_end(ap);
	p->error = buf_take(&b);
}

/* Copy a single-quoted run, quotes included. */
static int scan_single(Parser *p, Buf *b)
{
	buf_putc(b, '\'');
	p->pos++;
	while (p->pos < p->len && p->src[p->pos] != '\'') {
		if (p->src[p->pos] == '\n')
			p->lineno++;
		buf_putc(b, p->src[p->pos++]);
	}
	if (p->pos >= p->len) {
		p->incomplete = 1;
		perror_at(p, "unexpected EOF while looking for matching `''");
		return 0;
	}
	buf_putc(b, '\'');
	p->pos++;
	return 1;
}

static int scan_dollar(Parser *p, Buf *b, int in_dquote);
static int scan_backtick(Parser *p, Buf *b);

/* Copy a double-quoted run, quotes included, expanding nothing. */
static int scan_double(Parser *p, Buf *b)
{
	buf_putc(b, '"');
	p->pos++;
	while (p->pos < p->len && p->src[p->pos] != '"') {
		int c = (unsigned char)p->src[p->pos];

		if (c == '\\' && p->pos + 1 < p->len) {
			buf_putc(b, '\\');
			if (p->src[p->pos + 1] == '\n')
				p->lineno++;
			buf_putc(b, p->src[p->pos + 1]);
			p->pos += 2;
			continue;
		}
		if (c == '$') {
			if (!scan_dollar(p, b, 1))
				return 0;
			continue;
		}
		if (c == '`') {
			if (!scan_backtick(p, b))
				return 0;
			continue;
		}
		if (c == '\n')
			p->lineno++;
		buf_putc(b, p->src[p->pos++]);
	}
	if (p->pos >= p->len) {
		p->incomplete = 1;
		perror_at(p, "unexpected EOF while looking for matching `\"'");
		return 0;
	}
	buf_putc(b, '"');
	p->pos++;
	return 1;
}

static int scan_backtick(Parser *p, Buf *b)
{
	buf_putc(b, '`');
	p->pos++;
	while (p->pos < p->len && p->src[p->pos] != '`') {
		if (p->src[p->pos] == '\\' && p->pos + 1 < p->len) {
			buf_putc(b, p->src[p->pos]);
			buf_putc(b, p->src[p->pos + 1]);
			p->pos += 2;
			continue;
		}
		if (p->src[p->pos] == '\n')
			p->lineno++;
		buf_putc(b, p->src[p->pos++]);
	}
	if (p->pos >= p->len) {
		p->incomplete = 1;
		perror_at(p, "unexpected EOF while looking for matching ``'");
		return 0;
	}
	buf_putc(b, '`');
	p->pos++;
	return 1;
}

/* Copy a balanced construct: open/close counted, quotes respected. */
static int scan_balanced(Parser *p, Buf *b, char open, char close)
{
	int depth = 0;

	for (;;) {
		int c;

		if (p->pos >= p->len) {
			p->incomplete = 1;
			perror_at(p, "unexpected EOF while looking for `%c'", close);
			return 0;
		}
		c = (unsigned char)p->src[p->pos];
		if (c == '\\' && p->pos + 1 < p->len) {
			buf_putc(b, p->src[p->pos]);
			buf_putc(b, p->src[p->pos + 1]);
			p->pos += 2;
			continue;
		}
		if (c == '\'') {
			if (!scan_single(p, b))
				return 0;
			continue;
		}
		if (c == '"') {
			if (!scan_double(p, b))
				return 0;
			continue;
		}
		if (c == '`') {
			if (!scan_backtick(p, b))
				return 0;
			continue;
		}
		if (c == '#' && depth > 0 && open == '(' &&
		    (b->len == 0 || strchr(" \t\n(;&|", b->b[b->len - 1]))) {
			/* a comment inside $( ... ); 16#ff is not one */
			while (p->pos < p->len && p->src[p->pos] != '\n')
				buf_putc(b, p->src[p->pos++]);
			continue;
		}
		if (c == '\n')
			p->lineno++;
		buf_putc(b, p->src[p->pos++]);
		if (c == open) {
			depth++;
		} else if (c == close) {
			depth--;
			if (depth == 0)
				return 1;
		}
	}
}

/* Copy a $... construct starting at '$'. */
static int scan_dollar(Parser *p, Buf *b, int in_dquote)
{
	int c1 = at(p, 1);

	buf_putc(b, '$');
	p->pos++;
	if (c1 == '(') {
		if (at(p, 1) == '(') {
			size_t save = p->pos;
			int save_line = p->lineno;
			Buf inner;

			/* Try $(( ... )) first, fall back to $( (subshell) ). */
			buf_init(&inner);
			if (scan_balanced(p, &inner, '(', ')') && at(p, 0) == ')') {
				buf_putc(&inner, ')');
				p->pos++;
				buf_put(b, inner.b, inner.len);
				buf_free(&inner);
				return 1;
			}
			buf_free(&inner);
			p->pos = save;
			p->lineno = save_line;
			p->incomplete = 0;
			free(p->error);
			p->error = NULL;
		}
		return scan_balanced(p, b, '(', ')');
	}
	if (c1 == '{')
		return scan_balanced(p, b, '{', '}');
	if (!in_dquote && c1 == '\'') /* $'...' ANSI-C quoting */
		return scan_single(p, b);
	if (!in_dquote && c1 == '"') /* $"..." locale quoting, treated as "..." */
		return scan_double(p, b);
	return 1;
}

/* True when the text collected so far is an assignment prefix, so that a
 * following '(' opens an array literal rather than a subshell. */
static int assign_prefix(const char *s, size_t len)
{
	size_t i = 0;

	if (!len || s[len - 1] != '=')
		return 0;
	if (!is_name_char((unsigned char)s[0], 1))
		return 0;
	while (i < len && is_name_char((unsigned char)s[i], i == 0))
		i++;
	if (i < len && s[i] == '[') { /* NAME[subscript]= */
		int depth = 0;
		while (i < len) {
			if (s[i] == '[')
				depth++;
			else if (s[i] == ']' && --depth == 0) {
				i++;
				break;
			}
			i++;
		}
	}
	if (i < len && s[i] == '+')
		i++;
	return i == len - 1 && s[i] == '=';
}

static char *scan_word(Parser *p, int *quoted)
{
	Buf b;

	buf_init(&b);
	*quoted = 0;
	while (p->pos < p->len) {
		int c = (unsigned char)p->src[p->pos];

		if (c == '\\') {
			if (at(p, 1) == '\n') { /* line continuation */
				p->pos += 2;
				p->lineno++;
				continue;
			}
			*quoted = 1;
			buf_putc(&b, '\\');
			p->pos++;
			if (p->pos < p->len)
				buf_putc(&b, p->src[p->pos++]);
			continue;
		}
		if (c == '\'') {
			*quoted = 1;
			if (!scan_single(p, &b))
				goto fail;
			continue;
		}
		if (c == '"') {
			*quoted = 1;
			if (!scan_double(p, &b))
				goto fail;
			continue;
		}
		if (c == '`') {
			if (!scan_backtick(p, &b))
				goto fail;
			continue;
		}
		if (c == '$') {
			if (!scan_dollar(p, &b, 0))
				goto fail;
			continue;
		}
		if ((c == '<' || c == '>') && at(p, 1) == '(') {
			/* process substitution belongs to the word */
			buf_putc(&b, c);
			p->pos++;
			if (!scan_balanced(p, &b, '(', ')'))
				goto fail;
			continue;
		}
		if (sh.shopt.extglob && strchr("?*+@!", c) && at(p, 1) == '(') {
			/* an extended pattern group: @(a|b), !(x), *(y) */
			buf_putc(&b, c);
			p->pos++;
			if (!scan_balanced(p, &b, '(', ')'))
				goto fail;
			continue;
		}
		if (c == '(' && assign_prefix(b.b ? b.b : "", b.len)) {
			if (!scan_balanced(p, &b, '(', ')'))
				goto fail;
			continue;
		}
		if (is_meta(c))
			break;
		buf_putc(&b, p->src[p->pos++]);
	}
	return buf_take(&b);

fail:
	buf_free(&b);
	return NULL;
}

static void tok_free(Tok *t)
{
	free(t->text);
	free(t->varname);
	t->text = NULL;
	t->varname = NULL;
}

static Tok lex(Parser *p);

/* Read heredoc bodies queued on the current line. */
static void read_heredocs(Parser *p)
{
	int i;

	for (i = 0; i < p->here_count; i++) {
		Redir *r = p->here_queue[i];
		char *delim = unquote(r->word);
		int strip = r->fd == -2; /* <<- marked by the parser */
		Buf body;

		r->here_quoted = strcmp(delim, r->word) != 0;
		if (r->fd == -2)
			r->fd = -1;
		buf_init(&body);
		for (;;) {
			size_t start = p->pos, eol;
			const char *line;
			size_t linelen;

			if (p->pos >= p->len) {
				p->incomplete = 1;
				perror_at(p, "here-document delimited by end-of-file (wanted `%s')", delim);
				break;
			}
			eol = start;
			while (eol < p->len && p->src[eol] != '\n')
				eol++;
			line = p->src + start;
			linelen = eol - start;
			p->pos = eol < p->len ? eol + 1 : eol;
			p->lineno++;

			{
				const char *cmp = line;
				size_t cmplen = linelen;

				if (strip)
					while (cmplen && (*cmp == '\t')) {
						cmp++;
						cmplen--;
					}
				if (cmplen == strlen(delim) && memcmp(cmp, delim, cmplen) == 0)
					break;
				buf_put(&body, cmp, cmplen);
				buf_putc(&body, '\n');
			}
		}
		free(r->word);
		r->word = buf_take(&body);
		free(delim);
	}
	p->here_count = 0;
}

static Tok lex_raw(Parser *p)
{
	Tok t;
	int c;

	memset(&t, 0, sizeof t);

	for (;;) {
		while (p->pos < p->len && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t'))
			p->pos++;
		if (p->pos + 1 < p->len && p->src[p->pos] == '\\' && p->src[p->pos + 1] == '\n') {
			p->pos += 2;
			p->lineno++;
			continue;
		}
		break;
	}
	t.lineno = p->lineno;

	if (p->pos >= p->len) {
		t.type = T_EOF;
		return t;
	}

	c = (unsigned char)p->src[p->pos];

	if (c == '#') {
		while (p->pos < p->len && p->src[p->pos] != '\n')
			p->pos++;
		return lex_raw(p);
	}

	if (c == '\n') {
		p->pos++;
		p->lineno++;
		if (p->here_count)
			read_heredocs(p);
		t.type = T_NEWLINE;
		return t;
	}

	switch (c) {
	case ';':
		p->pos++;
		if (at(p, 0) == ';') {
			p->pos++;
			if (at(p, 0) == '&') {
				p->pos++;
				t.type = T_DSEMI_AMP;
			} else {
				t.type = T_DSEMI;
			}
		} else if (at(p, 0) == '&') {
			p->pos++;
			t.type = T_SEMI_AMP;
		} else {
			t.type = T_SEMI;
		}
		return t;
	case '&':
		p->pos++;
		if (at(p, 0) == '&') {
			p->pos++;
			t.type = T_AND_AND;
		} else if (at(p, 0) == '>') {
			p->pos++;
			if (at(p, 0) == '>') {
				p->pos++;
				t.type = T_ANDDGREAT;
			} else {
				t.type = T_ANDGREAT;
			}
		} else {
			t.type = T_AMP;
		}
		return t;
	case '|':
		p->pos++;
		if (at(p, 0) == '|') {
			p->pos++;
			t.type = T_OR_OR;
		} else if (at(p, 0) == '&') {
			p->pos++;
			t.type = T_PIPE_AMP;
		} else {
			t.type = T_PIPE;
		}
		return t;
	case '(':
		p->pos++;
		if (at(p, 0) == '(') {
			p->pos++;
			t.type = T_DLPAREN;
		} else {
			t.type = T_LPAREN;
		}
		return t;
	case ')':
		p->pos++;
		t.type = T_RPAREN;
		return t;
	case '<':
		if (at(p, 1) == '(')
			break; /* process substitution: this is a word */
		p->pos++;
		if (at(p, 0) == '<') {
			p->pos++;
			if (at(p, 0) == '<') {
				p->pos++;
				t.type = T_TLESS;
			} else if (at(p, 0) == '-') {
				p->pos++;
				t.type = T_DLESSDASH;
			} else {
				t.type = T_DLESS;
			}
		} else if (at(p, 0) == '&') {
			p->pos++;
			t.type = T_LESSAND;
		} else if (at(p, 0) == '>') {
			p->pos++;
			t.type = T_LESSGREAT;
		} else {
			t.type = T_LESS;
		}
		return t;
	case '>':
		if (at(p, 1) == '(')
			break; /* process substitution: this is a word */
		p->pos++;
		if (at(p, 0) == '>') {
			p->pos++;
			t.type = T_DGREAT;
		} else if (at(p, 0) == '&') {
			p->pos++;
			t.type = T_GREATAND;
		} else if (at(p, 0) == '|') {
			p->pos++;
			t.type = T_CLOBBER;
		} else {
			t.type = T_GREAT;
		}
		return t;
	default:
		break;
	}

	/* {fd}> and {fd}>> */
	if (c == '{') {
		size_t q = p->pos + 1;
		while (q < p->len && is_name_char((unsigned char)p->src[q], q == p->pos + 1))
			q++;
		if (q > p->pos + 1 && q < p->len && p->src[q] == '}' && q + 1 < p->len &&
		    (p->src[q + 1] == '>' || p->src[q + 1] == '<')) {
			char *name = xstrndup(p->src + p->pos + 1, q - p->pos - 1);
			Tok r;
			p->pos = q + 1;
			r = lex_raw(p);
			r.varname = name;
			return r;
		}
	}

	/* an IO number is digits immediately followed by < or > */
	if (isdigit(c)) {
		size_t q = p->pos;
		while (q < p->len && isdigit((unsigned char)p->src[q]))
			q++;
		if (q < p->len && (p->src[q] == '<' || p->src[q] == '>')) {
			t.type = T_IO_NUMBER;
			t.io_number = atoi(p->src + p->pos);
			p->pos = q;
			return t;
		}
	}

	{
		int quoted = 0;
		char *w = scan_word(p, &quoted);

		if (!w) {
			t.type = T_EOF;
			return t;
		}
		if (!*w && !quoted) {
			/* nothing consumed: skip the offending byte to make progress */
			free(w);
			p->pos++;
			return lex_raw(p);
		}
		t.type = T_WORD;
		t.text = w;
		return t;
	}
}

static Tok lex(Parser *p)
{
	if (p->pending) {
		PendTok *pt = p->pending;
		Tok t = pt->tok;
		p->pending = pt->next;
		free(pt);
		return t;
	}
	return lex_raw(p);
}

static Tok *peek(Parser *p)
{
	if (!p->have_cur) {
		p->cur = lex(p);
		p->have_cur = 1;
	}
	return &p->cur;
}

static Tok take(Parser *p)
{
	Tok t;

	peek(p);
	t = p->cur;
	p->have_cur = 0;
	memset(&p->cur, 0, sizeof p->cur);
	return t;
}

static void drop(Parser *p)
{
	Tok t = take(p);
	tok_free(&t);
}

static void push_tokens(Parser *p, PendTok *list)
{
	PendTok *tail = list;

	if (!list)
		return;
	while (tail->next)
		tail = tail->next;
	if (p->have_cur) {
		PendTok *pt = xcalloc(1, sizeof *pt);
		pt->tok = p->cur;
		pt->next = p->pending;
		p->pending = pt;
		p->have_cur = 0;
		memset(&p->cur, 0, sizeof p->cur);
	}
	tail->next = p->pending;
	p->pending = list;
}

/* ---------------------------------------------------------- reserved words */

static int word_is(Tok *t, const char *s)
{
	return t->type == T_WORD && t->text && strcmp(t->text, s) == 0;
}

/* -------------------------------------------------------------- alias work */

static int alias_active(Parser *p, const char *name)
{
	AliasFrame *f;

	for (f = p->aliases; f; f = f->next)
		if (strcmp(f->name, name) == 0)
			return 1;
	return 0;
}

/* Lex the alias body into a pending token list. */
static int expand_alias(Parser *p, const char *name)
{
	const char *body;
	Parser sub;
	PendTok *head = NULL, **tail = &head;
	AliasFrame *frame;

	if (!sh.shopt.expand_aliases || alias_active(p, name))
		return 0;
	body = alias_get(name);
	if (!body)
		return 0;

	memset(&sub, 0, sizeof sub);
	sub.src = body;
	sub.len = strlen(body);
	sub.lineno = p->lineno;
	sub.origin = p->origin;
	for (;;) {
		Tok t = lex_raw(&sub);
		PendTok *pt;

		if (t.type == T_EOF)
			break;
		pt = xcalloc(1, sizeof *pt);
		pt->tok = t;
		*tail = pt;
		tail = &pt->next;
	}
	free(sub.error);
	push_tokens(p, head);

	frame = xcalloc(1, sizeof *frame);
	frame->name = xstrdup(name);
	frame->next = p->aliases;
	p->aliases = frame;
	return 1;
}

static void alias_pop_all(Parser *p)
{
	while (p->aliases) {
		AliasFrame *f = p->aliases;
		p->aliases = f->next;
		free(f->name);
		free(f);
	}
}

/* ------------------------------------------------------------- productions */

static Node *parse_list(Parser *p, const char *const *stop);
static Node *parse_and_or(Parser *p);
static Node *parse_pipeline(Parser *p);
static Node *parse_command(Parser *p);

static void skip_newlines(Parser *p)
{
	while (peek(p)->type == T_NEWLINE)
		drop(p);
}

static void skip_separators(Parser *p)
{
	for (;;) {
		int t = peek(p)->type;
		if (t == T_NEWLINE || t == T_SEMI)
			drop(p);
		else
			break;
	}
}

static int at_stop(Parser *p, const char *const *stop)
{
	Tok *t = peek(p);
	int i;

	if (t->type == T_EOF)
		return 1;
	if (t->type != T_WORD)
		return 0;
	for (i = 0; stop && stop[i]; i++)
		if (strcmp(stop[i], t->text) == 0)
			return 1;
	return 0;
}

static int parse_redirect(Parser *p, Node *n)
{
	Tok *t = peek(p);
	int fd = -1;
	Tok op;
	Redir *r;
	char *varname = NULL;

	if (t->type == T_IO_NUMBER) {
		Tok num = take(p);
		fd = num.io_number;
		t = peek(p);
	}

	switch (t->type) {
	case T_LESS:
	case T_GREAT:
	case T_DGREAT:
	case T_CLOBBER:
	case T_LESSAND:
	case T_GREATAND:
	case T_LESSGREAT:
	case T_DLESS:
	case T_DLESSDASH:
	case T_TLESS:
	case T_ANDGREAT:
	case T_ANDDGREAT:
		break;
	default:
		if (fd >= 0)
			perror_at(p, "syntax error near unexpected token");
		return 0;
	}

	op = take(p);
	varname = op.varname;
	op.varname = NULL;

	if (peek(p)->type != T_WORD) {
		perror_at(p, "syntax error near unexpected token after redirection");
		free(varname);
		return 0;
	}

	r = xcalloc(1, sizeof *r);
	r->fd = fd;
	r->varname = varname;
	{
		Tok w = take(p);
		r->word = w.text;
		w.text = NULL;
		tok_free(&w);
	}

	switch (op.type) {
	case T_LESS:
		r->type = R_IN;
		break;
	case T_GREAT:
		r->type = R_OUT;
		break;
	case T_DGREAT:
		r->type = R_APPEND;
		break;
	case T_CLOBBER:
		r->type = R_CLOBBER;
		break;
	case T_LESSAND:
		r->type = R_DUP_IN;
		break;
	case T_GREATAND:
		r->type = R_DUP_OUT;
		break;
	case T_LESSGREAT:
		r->type = R_RDWR;
		break;
	case T_TLESS:
		r->type = R_HERESTR;
		break;
	case T_ANDGREAT:
		r->type = R_ALL_OUT;
		break;
	case T_ANDDGREAT:
		r->type = R_ALL_APPEND;
		break;
	case T_DLESS:
	case T_DLESSDASH:
		r->type = R_HEREDOC;
		if (op.type == T_DLESSDASH)
			r->fd = -2; /* marks <<-, resolved in read_heredocs */
		if (p->here_count < (int)(sizeof p->here_queue / sizeof p->here_queue[0]))
			p->here_queue[p->here_count++] = r;
		break;
	default:
		break;
	}
	node_add_redir(n, r);
	return 1;
}

/* NAME=..., NAME+=..., NAME[sub]=... at the head of a simple command */
static int looks_assign(const char *w)
{
	size_t i = 0;

	if (!is_name_char((unsigned char)w[0], 1))
		return 0;
	while (w[i] && is_name_char((unsigned char)w[i], i == 0))
		i++;
	if (w[i] == '[') {
		int depth = 0;
		size_t j = i;
		while (w[j]) {
			if (w[j] == '[')
				depth++;
			else if (w[j] == ']' && --depth == 0) {
				j++;
				break;
			}
			j++;
		}
		if (depth != 0)
			return 0;
		i = j;
	}
	if (w[i] == '+')
		i++;
	return w[i] == '=';
}

static char *scan_arith_section(Parser *p, const char *closers, int *closed_by)
{
	/* Read raw text up to one of the closer characters at depth 0. */
	Buf b;
	int depth = 0;

	buf_init(&b);
	*closed_by = 0;
	while (p->pos < p->len) {
		int c = (unsigned char)p->src[p->pos];

		if (c == '\'' ) {
			if (!scan_single(p, &b))
				break;
			continue;
		}
		if (c == '"') {
			if (!scan_double(p, &b))
				break;
			continue;
		}
		if (c == '$') {
			if (!scan_dollar(p, &b, 0))
				break;
			continue;
		}
		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth--;
		if (depth < 0 || (depth == 0 && strchr(closers, c) && c != '(' && c != '[')) {
			*closed_by = c;
			break;
		}
		if (c == '\n')
			p->lineno++;
		buf_putc(&b, p->src[p->pos++]);
	}
	return buf_take(&b);
}

/* After T_DLPAREN: read up to the matching )) */
static char *parse_arith_body(Parser *p)
{
	Buf b;
	int depth = 1;

	buf_init(&b);
	p->have_cur = 0; /* the (( token consumed the source already */
	while (p->pos < p->len) {
		int c = (unsigned char)p->src[p->pos];

		if (c == '\'') {
			if (!scan_single(p, &b))
				break;
			continue;
		}
		if (c == '"') {
			if (!scan_double(p, &b))
				break;
			continue;
		}
		if (c == '$') {
			if (!scan_dollar(p, &b, 0))
				break;
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth == 1 && at(p, 1) == ')') {
				p->pos += 2;
				return buf_take(&b);
			}
			depth--;
		}
		if (c == '\n')
			p->lineno++;
		buf_putc(&b, p->src[p->pos++]);
	}
	p->incomplete = 1;
	perror_at(p, "unexpected EOF while looking for `))'");
	buf_free(&b);
	return NULL;
}

/* The right hand side of =~ is one word in which ( ) | are regex syntax,
 * not shell syntax, so it is read straight out of the source. */
static char *scan_regex_operand(Parser *p)
{
	Buf b;
	int depth = 0;

	while (p->pos < p->len && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t'))
		p->pos++;
	buf_init(&b);
	while (p->pos < p->len) {
		int c = (unsigned char)p->src[p->pos];

		if (c == '\\' && p->pos + 1 < p->len) {
			buf_putc(&b, p->src[p->pos]);
			buf_putc(&b, p->src[p->pos + 1]);
			p->pos += 2;
			continue;
		}
		if (c == '\'') {
			if (!scan_single(p, &b))
				break;
			continue;
		}
		if (c == '"') {
			if (!scan_double(p, &b))
				break;
			continue;
		}
		if (c == '$') {
			if (!scan_dollar(p, &b, 0))
				break;
			continue;
		}
		if (c == '[') {
			/* a bracket expression may contain ) and whitespace */
			const char *start = p->src + p->pos;
			const char *q = start + 1;

			if (*q == '^')
				q++;
			if (*q == ']')
				q++;
			while (*q && *q != ']')
				q++;
			if (*q == ']') {
				size_t n = (size_t)(q - start) + 1;
				buf_put(&b, start, n);
				p->pos += n;
				continue;
			}
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth == 0)
				break;
			depth--;
		} else if ((c == ' ' || c == '\t' || c == '\n') && depth == 0) {
			break;
		}
		buf_putc(&b, p->src[p->pos++]);
	}
	return buf_take(&b);
}

/* After the [[ word: collect raw tokens until the matching ]] */
static int parse_cond_body(Parser *p, Node *n)
{
	int depth = 0;

	for (;;) {
		Tok *t = peek(p);

		if (t->type == T_EOF) {
			p->incomplete = 1;
			perror_at(p, "unexpected EOF while looking for `]]'");
			return 0;
		}
		if (t->type == T_NEWLINE) {
			drop(p);
			continue;
		}
		if (t->type == T_WORD && strcmp(t->text, "]]") == 0 && depth == 0) {
			drop(p);
			return 1;
		}
		if (t->type == T_DLPAREN) {
			/* inside [[ ]] there is no arithmetic command, so (( is
			 * simply two nested groupings */
			depth += 2;
			vec_pushs(&n->cond, "(");
			vec_pushs(&n->cond, "(");
			drop(p);
			continue;
		}
		if (t->type == T_LPAREN) {
			depth++;
			vec_pushs(&n->cond, "(");
			drop(p);
			continue;
		}
		if (t->type == T_RPAREN) {
			depth--;
			vec_pushs(&n->cond, ")");
			drop(p);
			continue;
		}
		if (t->type == T_AND_AND) {
			vec_pushs(&n->cond, "&&");
			drop(p);
			continue;
		}
		if (t->type == T_OR_OR) {
			vec_pushs(&n->cond, "||");
			drop(p);
			continue;
		}
		if (t->type == T_LESS) {
			vec_pushs(&n->cond, "<");
			drop(p);
			continue;
		}
		if (t->type == T_GREAT) {
			vec_pushs(&n->cond, ">");
			drop(p);
			continue;
		}
		if (t->type == T_WORD) {
			Tok w = take(p);
			int is_match = strcmp(w.text, "=~") == 0;

			vec_push(&n->cond, w.text);
			w.text = NULL;
			tok_free(&w);
			if (is_match && !p->have_cur && !p->pending) {
				char *rhs = scan_regex_operand(p);
				vec_push(&n->cond, rhs);
			}
			continue;
		}
		perror_at(p, "unexpected token in conditional expression");
		return 0;
	}
}

static Node *parse_if(Parser *p)
{
	static const char *const cond_stop[] = { "then", NULL };
	static const char *const body_stop[] = { "elif", "else", "fi", NULL };
	Node *n = node_new(N_IF);

	drop(p); /* if */
	n->left = parse_list(p, cond_stop);
	if (!word_is(peek(p), "then")) {
		perror_at(p, "syntax error: expected `then'");
		node_free(n);
		return NULL;
	}
	drop(p);
	n->body = parse_list(p, body_stop);

	if (word_is(peek(p), "elif")) {
		n->els = parse_if(p); /* reuses the `if` shape; consumes elif..fi */
		return n;
	}
	if (word_is(peek(p), "else")) {
		static const char *const else_stop[] = { "fi", NULL };
		drop(p);
		n->els = parse_list(p, else_stop);
	}
	if (!word_is(peek(p), "fi")) {
		perror_at(p, "syntax error: expected `fi'");
		node_free(n);
		return NULL;
	}
	drop(p);
	return n;
}

static Node *parse_do_group(Parser *p)
{
	static const char *const stop[] = { "done", NULL };
	Node *body;

	skip_newlines(p);
	if (!word_is(peek(p), "do")) {
		perror_at(p, "syntax error: expected `do'");
		return NULL;
	}
	drop(p);
	body = parse_list(p, stop);
	if (!word_is(peek(p), "done")) {
		perror_at(p, "syntax error: expected `done'");
		node_free(body);
		return NULL;
	}
	drop(p);
	return body;
}

static Node *parse_for(Parser *p)
{
	Node *n;

	drop(p); /* for */

	if (peek(p)->type == T_DLPAREN) {
		int closed;

		n = node_new(N_FOR_ARITH);
		p->have_cur = 0;
		n->a1 = scan_arith_section(p, ";", &closed);
		if (closed == ';')
			p->pos++;
		n->a2 = scan_arith_section(p, ";", &closed);
		if (closed == ';')
			p->pos++;
		n->a3 = scan_arith_section(p, ")", &closed);
		if (p->pos + 1 < p->len && p->src[p->pos] == ')' && p->src[p->pos + 1] == ')')
			p->pos += 2;
		skip_separators(p);
		n->body = parse_do_group(p);
		if (!n->body) {
			node_free(n);
			return NULL;
		}
		return n;
	}

	if (peek(p)->type != T_WORD) {
		perror_at(p, "syntax error: expected a variable name after `for'");
		return NULL;
	}
	n = node_new(N_FOR);
	{
		Tok w = take(p);
		n->name = w.text;
		w.text = NULL;
		tok_free(&w);
	}
	skip_newlines(p);
	if (word_is(peek(p), "in")) {
		drop(p);
		while (peek(p)->type == T_WORD && !word_is(peek(p), "do")) {
			Tok w = take(p);
			vec_push(&n->items, w.text);
			w.text = NULL;
			tok_free(&w);
		}
	} else {
		n->no_items = 1;
	}
	skip_separators(p);
	n->body = parse_do_group(p);
	if (!n->body) {
		node_free(n);
		return NULL;
	}
	return n;
}

static Node *parse_select(Parser *p)
{
	Node *n = node_new(N_SELECT);

	drop(p); /* select */
	if (peek(p)->type != T_WORD) {
		perror_at(p, "syntax error: expected a variable name after `select'");
		node_free(n);
		return NULL;
	}
	{
		Tok w = take(p);
		n->name = w.text;
		w.text = NULL;
		tok_free(&w);
	}
	skip_newlines(p);
	if (word_is(peek(p), "in")) {
		drop(p);
		while (peek(p)->type == T_WORD && !word_is(peek(p), "do")) {
			Tok w = take(p);
			vec_push(&n->items, w.text);
			w.text = NULL;
			tok_free(&w);
		}
	} else {
		n->no_items = 1;
	}
	skip_separators(p);
	n->body = parse_do_group(p);
	if (!n->body) {
		node_free(n);
		return NULL;
	}
	return n;
}

static Node *parse_while(Parser *p, NodeType type)
{
	static const char *const stop[] = { "do", NULL };
	Node *n = node_new(type);

	drop(p);
	n->left = parse_list(p, stop);
	n->body = parse_do_group(p);
	if (!n->body) {
		node_free(n);
		return NULL;
	}
	return n;
}

static Node *parse_case(Parser *p)
{
	static const char *const stop[] = { "esac", ";;", NULL };
	Node *n = node_new(N_CASE);
	CaseItem **tail = &n->cases;

	drop(p); /* case */
	if (peek(p)->type != T_WORD) {
		perror_at(p, "syntax error: expected a word after `case'");
		node_free(n);
		return NULL;
	}
	{
		Tok w = take(p);
		n->word = w.text;
		w.text = NULL;
		tok_free(&w);
	}
	skip_newlines(p);
	if (!word_is(peek(p), "in")) {
		perror_at(p, "syntax error: expected `in' after the case word");
		node_free(n);
		return NULL;
	}
	drop(p);
	skip_newlines(p);

	while (!word_is(peek(p), "esac")) {
		CaseItem *item;

		if (peek(p)->type == T_EOF) {
			p->incomplete = 1;
			perror_at(p, "unexpected EOF while looking for `esac'");
			node_free(n);
			return NULL;
		}
		item = xcalloc(1, sizeof *item);
		vec_init(&item->patterns);

		if (peek(p)->type == T_LPAREN)
			drop(p);
		for (;;) {
			if (peek(p)->type != T_WORD) {
				perror_at(p, "syntax error in case pattern");
				case_free(item);
				node_free(n);
				return NULL;
			}
			{
				Tok w = take(p);
				vec_push(&item->patterns, w.text);
				w.text = NULL;
				tok_free(&w);
			}
			if (peek(p)->type == T_PIPE) {
				drop(p);
				continue;
			}
			break;
		}
		if (peek(p)->type != T_RPAREN) {
			perror_at(p, "syntax error: expected `)' in case pattern");
			case_free(item);
			node_free(n);
			return NULL;
		}
		drop(p);
		skip_newlines(p);

		if (!word_is(peek(p), "esac") && peek(p)->type != T_DSEMI &&
		    peek(p)->type != T_SEMI_AMP && peek(p)->type != T_DSEMI_AMP)
			item->body = parse_list(p, stop);

		if (peek(p)->type == T_DSEMI) {
			drop(p);
		} else if (peek(p)->type == T_SEMI_AMP) {
			item->fallthrough = 1;
			drop(p);
		} else if (peek(p)->type == T_DSEMI_AMP) {
			item->retest = 1;
			drop(p);
		}
		skip_newlines(p);

		*tail = item;
		tail = &item->next;
	}
	drop(p); /* esac */
	return n;
}

static Node *parse_group(Parser *p)
{
	static const char *const stop[] = { "}", NULL };
	Node *n = node_new(N_GROUP);

	drop(p); /* { */
	n->body = parse_list(p, stop);
	if (!word_is(peek(p), "}")) {
		p->incomplete = peek(p)->type == T_EOF;
		perror_at(p, "syntax error: expected `}'");
		node_free(n);
		return NULL;
	}
	drop(p);
	return n;
}

static Node *parse_subshell(Parser *p)
{
	Node *n = node_new(N_SUBSHELL);

	drop(p); /* ( */
	n->body = parse_list(p, NULL);
	if (peek(p)->type != T_RPAREN) {
		p->incomplete = peek(p)->type == T_EOF;
		perror_at(p, "syntax error: expected `)'");
		node_free(n);
		return NULL;
	}
	drop(p);
	return n;
}

/* Parse the body of a function definition: name has been consumed. */
static Node *parse_func_body(Parser *p, char *name)
{
	Node *n = node_new(N_FUNC);

	n->name = name;
	skip_newlines(p);
	n->body = parse_command(p);
	if (!n->body) {
		node_free(n);
		return NULL;
	}
	return n;
}

static Node *parse_simple(Parser *p)
{
	Node *n = node_new(N_SIMPLE);
	int seen_word = 0;

	for (;;) {
		Tok *t = peek(p);

		if (t->type == T_IO_NUMBER || (t->type >= T_LESS && t->type <= T_ANDDGREAT)) {
			if (!parse_redirect(p, n)) {
				node_free(n);
				return NULL;
			}
			continue;
		}
		if (t->type != T_WORD)
			break;

		if (!seen_word) {
			/* function definition: name () */
			if (is_valid_name(t->text)) {
				size_t save_pos = p->pos;
				Tok w;
				const char *q;

				/* look ahead for '(' ')' */
				q = p->src + p->pos;
				while (*q == ' ' || *q == '\t')
					q++;
				if (q[0] == '(' && !p->pending) {
					const char *r = q + 1;
					while (*r == ' ' || *r == '\t')
						r++;
					if (*r == ')') {
						w = take(p);
						p->pos = (size_t)(r + 1 - p->src);
						p->have_cur = 0;
						node_free(n);
						return parse_func_body(p, w.text);
					}
				}
				(void)save_pos;
			}
			if (looks_assign(t->text)) {
				Tok w = take(p);
				vec_push(&n->assigns, w.text);
				w.text = NULL;
				tok_free(&w);
				continue;
			}
			if (alias_get(t->text) && sh.shopt.expand_aliases) {
				/* the name itself must be consumed before its body
				 * is pushed, or it comes back as a word */
				Tok w = take(p);
				int done = expand_alias(p, w.text);

				if (done) {
					tok_free(&w);
					continue;
				}
				vec_push(&n->words, w.text);
				w.text = NULL;
				tok_free(&w);
				seen_word = 1;
				continue;
			}
		}

		{
			Tok w = take(p);
			vec_push(&n->words, w.text);
			w.text = NULL;
			tok_free(&w);
			seen_word = 1;
		}
	}

	if (!n->words.len && !n->assigns.len && !n->redir) {
		node_free(n);
		return NULL;
	}
	return n;
}

static Node *parse_command(Parser *p)
{
	Tok *t = peek(p);
	Node *n = NULL;

	if (t->type == T_EOF)
		return NULL;

	if (t->type == T_LPAREN) {
		n = parse_subshell(p);
	} else if (t->type == T_DLPAREN) {
		char *body;
		p->have_cur = 0;
		body = parse_arith_body(p);
		if (!body)
			return NULL;
		n = node_new(N_ARITH);
		n->a1 = body;
	} else if (t->type == T_WORD) {
		if (strcmp(t->text, "{") == 0) {
			n = parse_group(p);
		} else if (strcmp(t->text, "if") == 0) {
			n = parse_if(p);
		} else if (strcmp(t->text, "while") == 0) {
			n = parse_while(p, N_WHILE);
		} else if (strcmp(t->text, "until") == 0) {
			n = parse_while(p, N_UNTIL);
		} else if (strcmp(t->text, "for") == 0) {
			n = parse_for(p);
		} else if (strcmp(t->text, "select") == 0) {
			n = parse_select(p);
		} else if (strcmp(t->text, "case") == 0) {
			n = parse_case(p);
		} else if (strcmp(t->text, "[[") == 0) {
			drop(p);
			n = node_new(N_COND);
			if (!parse_cond_body(p, n)) {
				node_free(n);
				return NULL;
			}
		} else if (strcmp(t->text, "function") == 0) {
			Tok name;
			drop(p);
			if (peek(p)->type != T_WORD) {
				perror_at(p, "syntax error: expected a function name");
				return NULL;
			}
			name = take(p);
			/* optional () */
			{
				const char *q = p->src + p->pos;
				while (*q == ' ' || *q == '\t')
					q++;
				if (q[0] == '(' && !p->pending && !p->have_cur) {
					const char *r = q + 1;
					while (*r == ' ' || *r == '\t')
						r++;
					if (*r == ')')
						p->pos = (size_t)(r + 1 - p->src);
				}
			}
			return parse_func_body(p, name.text);
		} else {
			n = parse_simple(p);
		}
	} else {
		return NULL;
	}

	if (!n)
		return NULL;

	/* trailing redirections on a compound command */
	for (;;) {
		Tok *r = peek(p);
		if (r->type == T_IO_NUMBER || (r->type >= T_LESS && r->type <= T_ANDDGREAT)) {
			if (!parse_redirect(p, n)) {
				node_free(n);
				return NULL;
			}
			continue;
		}
		break;
	}
	return n;
}

static Node *parse_pipeline(Parser *p)
{
	Node *first, *pipe;
	int negate = 0, timed = 0;

	while (peek(p)->type == T_WORD &&
	       (strcmp(peek(p)->text, "!") == 0 || strcmp(peek(p)->text, "time") == 0)) {
		if (peek(p)->text[0] == '!')
			negate = !negate;
		else
			timed = 1;
		drop(p);
		/* `time` alone with -p */
		if (timed && word_is(peek(p), "-p"))
			drop(p);
	}

	first = parse_command(p);
	if (!first)
		return NULL;

	if (peek(p)->type == T_PIPE || peek(p)->type == T_PIPE_AMP) {
		pipe = node_new(N_PIPE);
		node_add_kid(pipe, first, 0);
		while (peek(p)->type == T_PIPE || peek(p)->type == T_PIPE_AMP) {
			int both = peek(p)->type == T_PIPE_AMP;
			Node *next;
			drop(p);
			skip_newlines(p);
			next = parse_command(p);
			if (!next) {
				perror_at(p, "syntax error near unexpected token `|'");
				node_free(pipe);
				return NULL;
			}
			node_add_kid(pipe, next, both);
		}
		first = pipe;
	}

	if (negate) {
		Node *not = node_new(N_NOT);
		not->left = first;
		first = not;
	}
	if (timed) {
		Node *tn = node_new(N_TIME);
		tn->left = first;
		first = tn;
	}
	return first;
}

static Node *parse_and_or(Parser *p)
{
	Node *left = parse_pipeline(p);

	if (!left)
		return NULL;
	for (;;) {
		int t = peek(p)->type;
		Node *n;

		if (t != T_AND_AND && t != T_OR_OR)
			return left;
		drop(p);
		skip_newlines(p);
		n = node_new(t == T_AND_AND ? N_AND : N_OR);
		n->left = left;
		n->right = parse_pipeline(p);
		if (!n->right) {
			perror_at(p, "syntax error near unexpected token");
			node_free(n);
			return NULL;
		}
		left = n;
	}
}

static Node *parse_list(Parser *p, const char *const *stop)
{
	Node *seq = node_new(N_SEQ);

	for (;;) {
		Node *cmd;
		int bg = 0;
		int t;

		skip_separators(p);
		if (at_stop(p, stop))
			break;
		t = peek(p)->type;
		if (t == T_EOF || t == T_RPAREN || t == T_DSEMI || t == T_SEMI_AMP ||
		    t == T_DSEMI_AMP)
			break;

		cmd = parse_and_or(p);
		if (!cmd) {
			if (p->error) {
				node_free(seq);
				return NULL;
			}
			break;
		}
		if (peek(p)->type == T_AMP) {
			bg = 1;
			drop(p);
		} else if (peek(p)->type == T_SEMI) {
			drop(p);
		}
		node_add_kid(seq, cmd, bg);

		if (at_stop(p, stop))
			break;
	}
	if (seq->nkids == 1 && !seq->kidflags[0]) {
		Node *only = seq->kids[0];
		seq->nkids = 0;
		node_free(seq);
		return only;
	}
	return seq;
}

/* ---------------------------------------------------------------- entry pts */

Parser *parser_new(const char *src, const char *origin)
{
	Parser *p = xcalloc(1, sizeof *p);

	p->src = src;
	p->len = strlen(src);
	p->lineno = 1;
	p->origin = origin;
	return p;
}

void parser_free(Parser *p)
{
	if (!p)
		return;
	if (p->have_cur)
		tok_free(&p->cur);
	while (p->pending) {
		PendTok *pt = p->pending;
		p->pending = pt->next;
		tok_free(&pt->tok);
		free(pt);
	}
	alias_pop_all(p);
	free(p->error);
	free(p);
}

int parser_incomplete(Parser *p)
{
	return p->incomplete;
}

Node *parser_next(Parser *p, char **err)
{
	Node *cmd;
	int bg = 0;

	*err = NULL;
	alias_pop_all(p);
	skip_separators(p);
	sh.lineno = p->lineno;
	if (peek(p)->type == T_EOF)
		return NULL;

	cmd = parse_and_or(p);
	if (!cmd) {
		if (p->error) {
			*err = p->error;
			p->error = NULL;
		}
		return NULL;
	}
	if (peek(p)->type == T_AMP) {
		bg = 1;
		drop(p);
	} else if (peek(p)->type == T_SEMI) {
		drop(p);
	}
	if (p->error) {
		*err = p->error;
		p->error = NULL;
		node_free(cmd);
		return NULL;
	}
	if (bg) {
		Node *seq = node_new(N_SEQ);
		node_add_kid(seq, cmd, 1);
		return seq;
	}
	return cmd;
}

Node *parse_string(const char *src, const char *origin, char **err)
{
	Parser *p = parser_new(src, origin);
	Node *seq = node_new(N_SEQ);

	*err = NULL;
	for (;;) {
		char *e = NULL;
		Node *n = parser_next(p, &e);

		if (e) {
			*err = e;
			node_free(seq);
			parser_free(p);
			return NULL;
		}
		if (!n)
			break;
		if (n->type == N_SEQ && n->nkids == 1) {
			node_add_kid(seq, n->kids[0], n->kidflags[0]);
			n->nkids = 0;
			node_free(n);
		} else {
			node_add_kid(seq, n, 0);
		}
	}
	parser_free(p);
	return seq;
}

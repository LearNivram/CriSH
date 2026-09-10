/* awk.c - a POSIX awk built into the shell.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../regex.h"
#include "gnu.h"

/* ------------------------------------------------------------------ values */

#define C_NUM    0x1
#define C_STR    0x2
#define C_STRNUM 0x4 /* came from input and looks like a number */

typedef struct Cell {
	int flags;
	double num;
	char *str;
} Cell;

typedef struct AElemA {
	char *key;
	Cell val;
	struct AElemA *next;
} AElemA;

typedef struct Array {
	AElemA **buckets;
	size_t nbuckets;
	size_t count;
} Array;

typedef struct Sym {
	char *name;
	Cell val;
	Array *arr;
	struct Sym *next;
} Sym;

/* -------------------------------------------------------------------- tree */

typedef enum {
	/* expressions */
	E_NUM, E_STR, E_REGEX, E_VAR, E_FIELD, E_INDEX, E_ASSIGN, E_BIN, E_UN,
	E_TERNARY, E_MATCH, E_IN, E_CALL, E_BUILTIN, E_INCDEC, E_CONCAT, E_GETLINE,
	E_GROUP,
	/* statements */
	S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BLOCK, S_EXPR,
	S_NEXT, S_NEXTFILE, S_EXIT, S_RETURN, S_DELETE, S_BREAK, S_CONTINUE
} NType;

typedef struct ANode {
	NType type;
	double num;
	char *str;
	int op;
	Rx *rx;
	struct ANode *a, *b, *c, *d;
	struct ANode **list;
	size_t nlist;
	struct ANode *next;
	int builtin;
	int redirect; /* 0 none, '>' file, 'A' append, '|' pipe */
} ANode;

typedef struct Rule {
	ANode *pattern;   /* NULL for a bare action */
	ANode *pattern2;  /* range patterns */
	int range_active;
	ANode *action;    /* NULL means { print } */
	int is_begin, is_end;
	struct Rule *next;
} Rule;

typedef struct AwkFunc {
	char *name;
	Vec params;
	ANode *body;
	struct AwkFunc *next;
} AFunc;

/* ------------------------------------------------------------------- state */

typedef struct {
	Rule *rules;
	AFunc *funcs;
	Sym *globals;
	/* the current record and its fields */
	char *record;
	Vec fields;
	int fields_valid;
	int record_valid;
	long nf;
	/* call frame */
	Sym *locals;
	int in_function;
	Cell retval;
	int returning;
	int exiting;
	int exit_code;
	int next_record;
	int next_file;
	int breaking;
	int continuing;
	/* input */
	Vec input_files;
	size_t input_index;
	FILE *input;
	int input_is_stdin;
	/* redirections opened by print > and print | */
	struct Redirect {
		char *name;
		FILE *fp;
		int is_pipe;
		struct Redirect *next;
	} *redirects;
	const char *prog;
} Awk;

static Awk aw;

static void awk_fatal(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fprintf(stderr, "awk: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/* ------------------------------------------------------------- cell basics */

static void cell_clear(Cell *c)
{
	free(c->str);
	c->str = NULL;
	c->flags = 0;
	c->num = 0;
}

static void cell_set_num(Cell *c, double v)
{
	free(c->str);
	c->str = NULL;
	c->num = v;
	c->flags = C_NUM;
}

static void cell_set_str(Cell *c, const char *s)
{
	char *copy = xstrdup(s ? s : "");

	free(c->str);
	c->str = copy;
	c->flags = C_STR;
}

/* A field or input value that looks numeric compares numerically. */
static int looks_numeric(const char *s)
{
	char *end;

	if (!s)
		return 0;
	while (isspace((unsigned char)*s))
		s++;
	if (!*s)
		return 0;
	strtod(s, &end);
	if (end == s)
		return 0;
	while (isspace((unsigned char)*end))
		end++;
	return *end == '\0';
}

static void cell_set_input(Cell *c, const char *s)
{
	cell_set_str(c, s);
	if (looks_numeric(s)) {
		c->num = strtod(s, NULL);
		c->flags |= C_NUM | C_STRNUM;
	}
}

static double cell_num(const Cell *c)
{
	if (c->flags & C_NUM)
		return c->num;
	if (c->str)
		return strtod(c->str, NULL);
	return 0;
}

static const char *var_string(const char *name);

/* Render a number the way awk does: integers without a decimal point. */
static char *number_to_string(double v, const char *fmt)
{
	char buf[64];

	if (v == (long long)v && fabs(v) < 1e16) {
		snprintf(buf, sizeof buf, "%lld", (long long)v);
		return xstrdup(buf);
	}
	snprintf(buf, sizeof buf, fmt ? fmt : "%.6g", v);
	return xstrdup(buf);
}

static const char *cell_str(Cell *c)
{
	if (c->flags & C_STR)
		return c->str ? c->str : "";
	if (c->flags & C_NUM) {
		free(c->str);
		c->str = number_to_string(c->num, var_string("CONVFMT"));
		c->flags |= C_STR;
		return c->str;
	}
	return "";
}

/* The string used when printing, which honours OFMT rather than CONVFMT. */
static char *cell_output_str(Cell *c)
{
	if ((c->flags & C_NUM) && !(c->flags & C_STRNUM) && !(c->flags & C_STR))
		return number_to_string(c->num, var_string("OFMT"));
	return xstrdup(cell_str(c));
}

static int cell_true(Cell *c)
{
	if (c->flags & C_STRNUM)
		return cell_num(c) != 0;
	if (c->flags & C_NUM)
		return c->num != 0;
	if (c->flags & C_STR)
		return c->str && *c->str;
	return 0;
}

static void cell_copy(Cell *dst, const Cell *src)
{
	char *copy = src->str ? xstrdup(src->str) : NULL;

	free(dst->str);
	dst->str = copy;
	dst->num = src->num;
	dst->flags = src->flags;
}

/* ------------------------------------------------------------------ arrays */

static unsigned long hash_key(const char *s)
{
	unsigned long h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static Array *array_new(void)
{
	Array *a = xcalloc(1, sizeof *a);

	a->nbuckets = 64;
	a->buckets = xcalloc(a->nbuckets, sizeof *a->buckets);
	return a;
}

static Cell *array_get(Array *a, const char *key, int create)
{
	unsigned long h = hash_key(key) % a->nbuckets;
	AElemA *e;

	for (e = a->buckets[h]; e; e = e->next)
		if (strcmp(e->key, key) == 0)
			return &e->val;
	if (!create)
		return NULL;
	e = xcalloc(1, sizeof *e);
	e->key = xstrdup(key);
	e->next = a->buckets[h];
	a->buckets[h] = e;
	a->count++;
	return &e->val;
}

static void array_delete(Array *a, const char *key)
{
	unsigned long h = hash_key(key) % a->nbuckets;
	AElemA **p;

	for (p = &a->buckets[h]; *p; p = &(*p)->next) {
		if (strcmp((*p)->key, key) == 0) {
			AElemA *e = *p;
			*p = e->next;
			free(e->key);
			cell_clear(&e->val);
			free(e);
			a->count--;
			return;
		}
	}
}

static void array_clear(Array *a)
{
	size_t i;

	for (i = 0; i < a->nbuckets; i++) {
		AElemA *e = a->buckets[i];

		while (e) {
			AElemA *n = e->next;
			free(e->key);
			cell_clear(&e->val);
			free(e);
			e = n;
		}
		a->buckets[i] = NULL;
	}
	a->count = 0;
}

static void array_keys(Array *a, Vec *out)
{
	size_t i;

	for (i = 0; i < a->nbuckets; i++) {
		AElemA *e;

		for (e = a->buckets[i]; e; e = e->next)
			vec_pushs(out, e->key);
	}
}

/* ------------------------------------------------------------------ symbols */

static Sym *sym_find(const char *name)
{
	Sym *s;

	for (s = aw.locals; s; s = s->next)
		if (strcmp(s->name, name) == 0)
			return s;
	for (s = aw.globals; s; s = s->next)
		if (strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

static Sym *sym_get(const char *name)
{
	Sym *s = sym_find(name);

	if (s)
		return s;
	s = xcalloc(1, sizeof *s);
	s->name = xstrdup(name);
	s->next = aw.globals;
	aw.globals = s;
	return s;
}

static Array *sym_array(const char *name)
{
	Sym *s = sym_get(name);

	if (!s->arr)
		s->arr = array_new();
	return s->arr;
}

static const char *var_string(const char *name)
{
	Sym *s = sym_find(name);

	return s ? cell_str(&s->val) : "";
}

static double var_number(const char *name)
{
	Sym *s = sym_find(name);

	return s ? cell_num(&s->val) : 0;
}

static void var_set_num(const char *name, double v)
{
	cell_set_num(&sym_get(name)->val, v);
}

static void var_set_str_(const char *name, const char *v)
{
	cell_set_str(&sym_get(name)->val, v);
}

/* -------------------------------------------------------------------- lexer */

enum {
	T_EOF = 0, T_NUMBER, T_STRING, T_ERE, T_FUNC_NAME, T_NAME, T_BUILTIN,
	T_GETLINE, T_BEGIN, T_END, T_FUNCTION, T_LBRACE, T_RBRACE, T_LPAREN,
	T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_NEWLINE, T_COMMA,
	T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET, T_BANG,
	T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE, T_MATCH, T_NOMATCH,
	T_AND, T_OR, T_QUESTION, T_COLON, T_DOLLAR, T_ASSIGN, T_ADD_ASSIGN,
	T_SUB_ASSIGN, T_MUL_ASSIGN, T_DIV_ASSIGN, T_MOD_ASSIGN, T_POW_ASSIGN,
	T_INCR, T_DECR, T_APPEND, T_PIPE, T_IN, T_IF, T_ELSE, T_WHILE, T_FOR,
	T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_NEXTFILE, T_EXIT, T_RETURN,
	T_DELETE, T_PRINT, T_PRINTF
};

enum {
	B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF,
	B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT, B_RAND, B_SRAND,
	B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH
};

static const struct {
	const char *name;
	int tok;
	int builtin;
} keywords[] = {
	{ "BEGIN", T_BEGIN, 0 }, { "END", T_END, 0 }, { "function", T_FUNCTION, 0 },
	{ "func", T_FUNCTION, 0 }, { "if", T_IF, 0 }, { "else", T_ELSE, 0 },
	{ "while", T_WHILE, 0 }, { "for", T_FOR, 0 }, { "do", T_DO, 0 },
	{ "break", T_BREAK, 0 }, { "continue", T_CONTINUE, 0 }, { "next", T_NEXT, 0 },
	{ "nextfile", T_NEXTFILE, 0 }, { "exit", T_EXIT, 0 }, { "return", T_RETURN, 0 },
	{ "delete", T_DELETE, 0 }, { "print", T_PRINT, 0 }, { "printf", T_PRINTF, 0 },
	{ "in", T_IN, 0 }, { "getline", T_GETLINE, 0 },
	{ "length", T_BUILTIN, B_LENGTH }, { "substr", T_BUILTIN, B_SUBSTR },
	{ "index", T_BUILTIN, B_INDEX }, { "split", T_BUILTIN, B_SPLIT },
	{ "sub", T_BUILTIN, B_SUB }, { "gsub", T_BUILTIN, B_GSUB },
	{ "match", T_BUILTIN, B_MATCH }, { "sprintf", T_BUILTIN, B_SPRINTF },
	{ "sin", T_BUILTIN, B_SIN }, { "cos", T_BUILTIN, B_COS },
	{ "atan2", T_BUILTIN, B_ATAN2 }, { "exp", T_BUILTIN, B_EXP },
	{ "log", T_BUILTIN, B_LOG }, { "sqrt", T_BUILTIN, B_SQRT },
	{ "int", T_BUILTIN, B_INT }, { "rand", T_BUILTIN, B_RAND },
	{ "srand", T_BUILTIN, B_SRAND }, { "tolower", T_BUILTIN, B_TOLOWER },
	{ "toupper", T_BUILTIN, B_TOUPPER }, { "system", T_BUILTIN, B_SYSTEM },
	{ "close", T_BUILTIN, B_CLOSE }, { "fflush", T_BUILTIN, B_FFLUSH },
	{ NULL, 0, 0 }
};

typedef struct {
	const char *src;
	size_t pos;
	int tok;
	double num;
	char *str;
	int builtin;
	int prev_tok;
	int line;
} Lex;

static Lex lx;

static void lex_next(void);

/* A / after these tokens starts a regex rather than a division. */
static int regex_allowed(int prev)
{
	switch (prev) {
	case T_NAME:
	case T_NUMBER:
	case T_STRING:
	case T_RPAREN:
	case T_RBRACKET:
	case T_INCR:
	case T_DECR:
	case T_DOLLAR:
	case T_BUILTIN:
		return 0;
	default:
		return 1;
	}
}

static void lex_next(void)
{
	const char *p = lx.src + lx.pos;

	free(lx.str);
	lx.str = NULL;
	lx.prev_tok = lx.tok;

	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r')
			p++;
		if (*p == '\\' && p[1] == '\n') {
			p += 2;
			lx.line++;
			continue;
		}
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		break;
	}

	lx.pos = (size_t)(p - lx.src);
	if (!*p) {
		lx.tok = T_EOF;
		return;
	}

	if (*p == '\n') {
		lx.pos++;
		lx.line++;
		lx.tok = T_NEWLINE;
		return;
	}

	if (isdigit((unsigned char)*p) ||
	    (*p == '.' && isdigit((unsigned char)p[1]))) {
		char *end;

		lx.num = strtod(p, &end);
		lx.pos = (size_t)(end - lx.src);
		lx.tok = T_NUMBER;
		return;
	}

	if (*p == '"') {
		Buf b;

		buf_init(&b);
		p++;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) {
				p++;
				switch (*p) {
				case 'n': buf_putc(&b, '\n'); break;
				case 't': buf_putc(&b, '\t'); break;
				case 'r': buf_putc(&b, '\r'); break;
				case '\\': buf_putc(&b, '\\'); break;
				case '"': buf_putc(&b, '"'); break;
				case '/': buf_putc(&b, '/'); break;
				case 'a': buf_putc(&b, '\a'); break;
				case 'b': buf_putc(&b, '\b'); break;
				case 'f': buf_putc(&b, '\f'); break;
				case 'v': buf_putc(&b, '\v'); break;
				default:
					if (*p >= '0' && *p <= '7') {
						int v = 0, n = 0;
						while (n < 3 && *p >= '0' && *p <= '7') {
							v = v * 8 + (*p - '0');
							p++;
							n++;
						}
						p--;
						buf_putc(&b, v);
					} else {
						buf_putc(&b, '\\');
						buf_putc(&b, *p);
					}
					break;
				}
				p++;
				continue;
			}
			buf_putc(&b, *p++);
		}
		if (*p == '"')
			p++;
		lx.str = buf_take(&b);
		lx.pos = (size_t)(p - lx.src);
		lx.tok = T_STRING;
		return;
	}

	if (*p == '/' && regex_allowed(lx.prev_tok)) {
		Buf b;

		buf_init(&b);
		p++;
		while (*p && *p != '/') {
			if (*p == '\\' && p[1]) {
				if (p[1] == '/') {
					buf_putc(&b, '/');
					p += 2;
					continue;
				}
				buf_putc(&b, *p++);
				buf_putc(&b, *p++);
				continue;
			}
			buf_putc(&b, *p++);
		}
		if (*p == '/')
			p++;
		lx.str = buf_take(&b);
		lx.pos = (size_t)(p - lx.src);
		lx.tok = T_ERE;
		return;
	}

	if (isalpha((unsigned char)*p) || *p == '_') {
		const char *start = p;
		int i;

		while (isalnum((unsigned char)*p) || *p == '_')
			p++;
		lx.str = xstrndup(start, (size_t)(p - start));
		lx.pos = (size_t)(p - lx.src);
		for (i = 0; keywords[i].name; i++) {
			if (strcmp(keywords[i].name, lx.str) == 0) {
				lx.tok = keywords[i].tok;
				lx.builtin = keywords[i].builtin;
				return;
			}
		}
		lx.tok = *p == '(' ? T_FUNC_NAME : T_NAME;
		return;
	}

	lx.pos++;
	switch (*p) {
	case '{': lx.tok = T_LBRACE; return;
	case '}': lx.tok = T_RBRACE; return;
	case '(': lx.tok = T_LPAREN; return;
	case ')': lx.tok = T_RPAREN; return;
	case '[': lx.tok = T_LBRACKET; return;
	case ']': lx.tok = T_RBRACKET; return;
	case ';': lx.tok = T_SEMI; return;
	case ',': lx.tok = T_COMMA; return;
	case '?': lx.tok = T_QUESTION; return;
	case ':': lx.tok = T_COLON; return;
	case '$': lx.tok = T_DOLLAR; return;
	case '~': lx.tok = T_MATCH; return;
	case '+':
		if (p[1] == '+') { lx.pos++; lx.tok = T_INCR; return; }
		if (p[1] == '=') { lx.pos++; lx.tok = T_ADD_ASSIGN; return; }
		lx.tok = T_PLUS;
		return;
	case '-':
		if (p[1] == '-') { lx.pos++; lx.tok = T_DECR; return; }
		if (p[1] == '=') { lx.pos++; lx.tok = T_SUB_ASSIGN; return; }
		lx.tok = T_MINUS;
		return;
	case '*':
		if (p[1] == '*') {
			lx.pos++;
			if (p[2] == '=') { lx.pos++; lx.tok = T_POW_ASSIGN; return; }
			lx.tok = T_CARET;
			return;
		}
		if (p[1] == '=') { lx.pos++; lx.tok = T_MUL_ASSIGN; return; }
		lx.tok = T_STAR;
		return;
	case '/':
		if (p[1] == '=') { lx.pos++; lx.tok = T_DIV_ASSIGN; return; }
		lx.tok = T_SLASH;
		return;
	case '%':
		if (p[1] == '=') { lx.pos++; lx.tok = T_MOD_ASSIGN; return; }
		lx.tok = T_PERCENT;
		return;
	case '^':
		if (p[1] == '=') { lx.pos++; lx.tok = T_POW_ASSIGN; return; }
		lx.tok = T_CARET;
		return;
	case '!':
		if (p[1] == '=') { lx.pos++; lx.tok = T_NE; return; }
		if (p[1] == '~') { lx.pos++; lx.tok = T_NOMATCH; return; }
		lx.tok = T_BANG;
		return;
	case '<':
		if (p[1] == '=') { lx.pos++; lx.tok = T_LE; return; }
		lx.tok = T_LT;
		return;
	case '>':
		if (p[1] == '=') { lx.pos++; lx.tok = T_GE; return; }
		if (p[1] == '>') { lx.pos++; lx.tok = T_APPEND; return; }
		lx.tok = T_GT;
		return;
	case '=':
		if (p[1] == '=') { lx.pos++; lx.tok = T_EQ; return; }
		lx.tok = T_ASSIGN;
		return;
	case '&':
		if (p[1] == '&') { lx.pos++; lx.tok = T_AND; return; }
		break;
	case '|':
		if (p[1] == '|') { lx.pos++; lx.tok = T_OR; return; }
		lx.tok = T_PIPE;
		return;
	default:
		break;
	}
	awk_fatal("syntax error at line %d near '%c'", lx.line, *p);
}

/* ------------------------------------------------------------------ parser */

static ANode *node(NType t)
{
	ANode *n = xcalloc(1, sizeof *n);

	n->type = t;
	return n;
}

static ANode *parse_expr(int no_gt);
static ANode *parse_statement(void);
static ANode *parse_block(void);

static void expect(int tok, const char *what)
{
	if (lx.tok != tok)
		awk_fatal("syntax error at line %d: expected %s", lx.line, what);
	lex_next();
}

static void skip_newlines(void)
{
	while (lx.tok == T_NEWLINE || lx.tok == T_SEMI)
		lex_next();
}

static void skip_optional_newlines(void)
{
	while (lx.tok == T_NEWLINE)
		lex_next();
}

static Rx *compile_ere(const char *pat)
{
	const char *err = NULL;
	Rx *rx = rx_compile(pat, RX_ERE, 0, &err);

	if (!rx)
		awk_fatal("bad regular expression /%s/: %s", pat, err ? err : "?");
	return rx;
}

static void node_add(ANode *n, ANode *item)
{
	n->list = xrealloc(n->list, (n->nlist + 1) * sizeof *n->list);
	n->list[n->nlist++] = item;
}

static int is_assign_tok(int t)
{
	return t == T_ASSIGN || t == T_ADD_ASSIGN || t == T_SUB_ASSIGN ||
	       t == T_MUL_ASSIGN || t == T_DIV_ASSIGN || t == T_MOD_ASSIGN ||
	       t == T_POW_ASSIGN;
}

/* Can this token start an expression?  Needed for implicit concatenation. */
static int starts_expr(int t)
{
	switch (t) {
	case T_NUMBER: case T_STRING: case T_ERE: case T_NAME: case T_FUNC_NAME:
	case T_DOLLAR: case T_LPAREN: case T_BANG: case T_MINUS: case T_PLUS:
	case T_INCR: case T_DECR: case T_BUILTIN:
		return 1;
	default:
		return 0;
	}
}

static ANode *parse_primary(int no_gt);

static ANode *parse_lvalue_from(ANode *e)
{
	return e;
}

static ANode *parse_postfix(int no_gt)
{
	ANode *e = parse_primary(no_gt);

	while (lx.tok == T_INCR || lx.tok == T_DECR) {
		ANode *n = node(E_INCDEC);

		n->op = lx.tok == T_INCR ? '+' : '-';
		n->num = 1; /* postfix */
		n->a = e;
		lex_next();
		e = n;
	}
	return e;
}

static ANode *parse_unary(int no_gt);

static ANode *parse_power(int no_gt)
{
	ANode *e = parse_postfix(no_gt);

	if (lx.tok == T_CARET) {
		ANode *n = node(E_BIN);

		n->op = '^';
		n->a = e;
		lex_next();
		n->b = parse_unary(no_gt); /* right associative */
		return n;
	}
	return e;
}

static ANode *parse_unary(int no_gt)
{
	if (lx.tok == T_BANG || lx.tok == T_MINUS || lx.tok == T_PLUS) {
		ANode *n = node(E_UN);

		n->op = lx.tok == T_BANG ? '!' : (lx.tok == T_MINUS ? '-' : '+');
		lex_next();
		n->a = parse_unary(no_gt);
		return n;
	}
	if (lx.tok == T_INCR || lx.tok == T_DECR) {
		ANode *n = node(E_INCDEC);

		n->op = lx.tok == T_INCR ? '+' : '-';
		n->num = 0; /* prefix */
		lex_next();
		n->a = parse_unary(no_gt);
		return n;
	}
	return parse_power(no_gt);
}

static ANode *parse_mul(int no_gt)
{
	ANode *e = parse_unary(no_gt);

	for (;;) {
		int op;

		if (lx.tok == T_STAR)
			op = '*';
		else if (lx.tok == T_SLASH)
			op = '/';
		else if (lx.tok == T_PERCENT)
			op = '%';
		else
			return e;
		{
			ANode *n = node(E_BIN);

			n->op = op;
			n->a = e;
			lex_next();
			n->b = parse_unary(no_gt);
			e = n;
		}
	}
}

static ANode *parse_add(int no_gt)
{
	ANode *e = parse_mul(no_gt);

	for (;;) {
		int op;

		if (lx.tok == T_PLUS)
			op = '+';
		else if (lx.tok == T_MINUS)
			op = '-';
		else
			return e;
		{
			ANode *n = node(E_BIN);

			n->op = op;
			n->a = e;
			lex_next();
			n->b = parse_mul(no_gt);
			e = n;
		}
	}
}

static ANode *parse_concat(int no_gt)
{
	ANode *e = parse_add(no_gt);

	while (starts_expr(lx.tok)) {
		ANode *n;

		/* `getline` and `in` are not concatenation */
		if (lx.tok == T_IN || lx.tok == T_GETLINE)
			break;
		n = node(E_CONCAT);
		n->a = e;
		n->b = parse_add(no_gt);
		e = n;
	}
	return e;
}

static ANode *parse_rel(int no_gt)
{
	ANode *e = parse_concat(no_gt);
	int op = 0;

	switch (lx.tok) {
	case T_LT: op = '<'; break;
	case T_LE: op = 'l'; break;
	case T_GT: op = no_gt ? 0 : '>'; break;
	case T_GE: op = 'g'; break;
	case T_EQ: op = '='; break;
	case T_NE: op = 'n'; break;
	default: op = 0; break;
	}
	if (!op)
		return e;
	{
		ANode *n = node(E_BIN);

		n->op = op;
		n->a = e;
		lex_next();
		n->b = parse_concat(no_gt);
		return n;
	}
}

static ANode *parse_match(int no_gt)
{
	ANode *e = parse_rel(no_gt);

	while (lx.tok == T_MATCH || lx.tok == T_NOMATCH) {
		ANode *n = node(E_MATCH);

		n->op = lx.tok == T_MATCH ? '~' : '!';
		n->a = e;
		lex_next();
		n->b = parse_rel(no_gt);
		e = n;
	}
	return e;
}

static ANode *parse_in(int no_gt)
{
	ANode *e = parse_match(no_gt);

	while (lx.tok == T_IN) {
		ANode *n = node(E_IN);

		n->a = e;
		lex_next();
		if (lx.tok != T_NAME)
			awk_fatal("syntax error: expected an array name after `in'");
		n->str = xstrdup(lx.str);
		lex_next();
		e = n;
	}
	return e;
}

static ANode *parse_and(int no_gt)
{
	ANode *e = parse_in(no_gt);

	while (lx.tok == T_AND) {
		ANode *n = node(E_BIN);

		n->op = 'a';
		n->a = e;
		lex_next();
		skip_optional_newlines();
		n->b = parse_in(no_gt);
		e = n;
	}
	return e;
}

static ANode *parse_or(int no_gt)
{
	ANode *e = parse_and(no_gt);

	while (lx.tok == T_OR) {
		ANode *n = node(E_BIN);

		n->op = 'o';
		n->a = e;
		lex_next();
		skip_optional_newlines();
		n->b = parse_and(no_gt);
		e = n;
	}
	return e;
}

static ANode *parse_ternary(int no_gt)
{
	ANode *e = parse_or(no_gt);

	if (lx.tok == T_QUESTION) {
		ANode *n = node(E_TERNARY);

		n->a = e;
		lex_next();
		skip_optional_newlines();
		n->b = parse_ternary(no_gt);
		skip_optional_newlines();
		expect(T_COLON, "`:'");
		skip_optional_newlines();
		n->c = parse_ternary(no_gt);
		return n;
	}
	return e;
}

static int is_lvalue(ANode *e)
{
	return e && (e->type == E_VAR || e->type == E_FIELD || e->type == E_INDEX);
}

static ANode *parse_expr(int no_gt)
{
	ANode *e = parse_ternary(no_gt);

	if (is_assign_tok(lx.tok) && is_lvalue(e)) {
		ANode *n = node(E_ASSIGN);

		switch (lx.tok) {
		case T_ASSIGN: n->op = 0; break;
		case T_ADD_ASSIGN: n->op = '+'; break;
		case T_SUB_ASSIGN: n->op = '-'; break;
		case T_MUL_ASSIGN: n->op = '*'; break;
		case T_DIV_ASSIGN: n->op = '/'; break;
		case T_MOD_ASSIGN: n->op = '%'; break;
		default: n->op = '^'; break;
		}
		n->a = parse_lvalue_from(e);
		lex_next();
		n->b = parse_expr(no_gt);
		return n;
	}

	/* cmd | getline [var] */
	while (lx.tok == T_PIPE) {
		size_t save = lx.pos;
		int save_tok = lx.tok;

		lex_next();
		if (lx.tok != T_GETLINE) {
			lx.pos = save;
			lx.tok = save_tok;
			break;
		}
		{
			ANode *n = node(E_GETLINE);

			n->op = '|';
			n->b = e; /* the command */
			lex_next();
			if (lx.tok == T_NAME || lx.tok == T_DOLLAR)
				n->a = parse_postfix(no_gt);
			e = n;
		}
	}
	return e;
}

static ANode *parse_primary(int no_gt)
{
	switch (lx.tok) {
	case T_NUMBER: {
		ANode *n = node(E_NUM);

		n->num = lx.num;
		lex_next();
		return n;
	}
	case T_STRING: {
		ANode *n = node(E_STR);

		n->str = xstrdup(lx.str);
		lex_next();
		return n;
	}
	case T_ERE: {
		ANode *n = node(E_REGEX);

		n->str = xstrdup(lx.str);
		n->rx = compile_ere(lx.str);
		lex_next();
		return n;
	}
	case T_DOLLAR: {
		ANode *n = node(E_FIELD);

		lex_next();
		n->a = parse_primary(no_gt);
		return n;
	}
	case T_LPAREN: {
		ANode *first;

		lex_next();
		first = parse_expr(0);
		if (lx.tok == T_COMMA) {
			/* (a, b) in array */
			ANode *n = node(E_GROUP);

			node_add(n, first);
			while (lx.tok == T_COMMA) {
				lex_next();
				skip_optional_newlines();
				node_add(n, parse_expr(0));
			}
			expect(T_RPAREN, "`)'");
			return n;
		}
		expect(T_RPAREN, "`)'");
		return first;
	}
	case T_GETLINE: {
		ANode *n = node(E_GETLINE);

		lex_next();
		if (lx.tok == T_NAME || lx.tok == T_DOLLAR)
			n->a = parse_postfix(no_gt);
		if (lx.tok == T_LT) {
			n->op = '<';
			lex_next();
			n->b = parse_concat(no_gt);
		}
		return n;
	}
	case T_BUILTIN: {
		ANode *n = node(E_BUILTIN);

		n->builtin = lx.builtin;
		lex_next();
		if (lx.tok == T_LPAREN) {
			lex_next();
			if (lx.tok != T_RPAREN) {
				node_add(n, parse_expr(0));
				while (lx.tok == T_COMMA) {
					lex_next();
					skip_optional_newlines();
					node_add(n, parse_expr(0));
				}
			}
			expect(T_RPAREN, "`)'");
		} else if (n->builtin != B_LENGTH) {
			awk_fatal("%s requires arguments", "builtin");
		}
		return n;
	}
	case T_FUNC_NAME: {
		ANode *n = node(E_CALL);

		n->str = xstrdup(lx.str);
		lex_next();
		expect(T_LPAREN, "`('");
		if (lx.tok != T_RPAREN) {
			node_add(n, parse_expr(0));
			while (lx.tok == T_COMMA) {
				lex_next();
				skip_optional_newlines();
				node_add(n, parse_expr(0));
			}
		}
		expect(T_RPAREN, "`)'");
		return n;
	}
	case T_NAME: {
		char *name = xstrdup(lx.str);

		lex_next();
		if (lx.tok == T_LBRACKET) {
			ANode *n = node(E_INDEX);

			n->str = name;
			lex_next();
			node_add(n, parse_expr(0));
			while (lx.tok == T_COMMA) {
				lex_next();
				node_add(n, parse_expr(0));
			}
			expect(T_RBRACKET, "`]'");
			return n;
		}
		{
			ANode *n = node(E_VAR);

			n->str = name;
			return n;
		}
	}
	default:
		awk_fatal("syntax error at line %d", lx.line);
		return NULL;
	}
}

/* print / printf argument list plus an optional redirection */
static void parse_output(ANode *n)
{
	if (lx.tok != T_NEWLINE && lx.tok != T_SEMI && lx.tok != T_RBRACE &&
	    lx.tok != T_EOF && lx.tok != T_GT && lx.tok != T_APPEND &&
	    lx.tok != T_PIPE) {
		node_add(n, parse_expr(1));
		while (lx.tok == T_COMMA) {
			lex_next();
			skip_optional_newlines();
			node_add(n, parse_expr(1));
		}
	}
	/* a single parenthesised group is an argument list */
	if (n->nlist == 1 && n->list[0]->type == E_GROUP) {
		ANode *g = n->list[0];
		size_t i;

		n->nlist = 0;
		for (i = 0; i < g->nlist; i++)
			node_add(n, g->list[i]);
	}
	if (lx.tok == T_GT) {
		n->redirect = '>';
		lex_next();
		n->d = parse_ternary(1);
	} else if (lx.tok == T_APPEND) {
		n->redirect = 'A';
		lex_next();
		n->d = parse_ternary(1);
	} else if (lx.tok == T_PIPE) {
		n->redirect = '|';
		lex_next();
		n->d = parse_ternary(1);
	}
}

static ANode *parse_simple_statement(void)
{
	switch (lx.tok) {
	case T_PRINT: {
		ANode *n = node(S_PRINT);

		lex_next();
		parse_output(n);
		return n;
	}
	case T_PRINTF: {
		ANode *n = node(S_PRINTF);

		lex_next();
		parse_output(n);
		if (!n->nlist)
			awk_fatal("printf: no format");
		return n;
	}
	case T_DELETE: {
		ANode *n = node(S_DELETE);

		lex_next();
		if (lx.tok != T_NAME)
			awk_fatal("delete: expected an array name");
		n->str = xstrdup(lx.str);
		lex_next();
		if (lx.tok == T_LBRACKET) {
			lex_next();
			node_add(n, parse_expr(0));
			while (lx.tok == T_COMMA) {
				lex_next();
				node_add(n, parse_expr(0));
			}
			expect(T_RBRACKET, "`]'");
		}
		return n;
	}
	case T_NEXT: {
		ANode *n = node(S_NEXT);
		lex_next();
		return n;
	}
	case T_NEXTFILE: {
		ANode *n = node(S_NEXTFILE);
		lex_next();
		return n;
	}
	case T_BREAK: {
		ANode *n = node(S_BREAK);
		lex_next();
		return n;
	}
	case T_CONTINUE: {
		ANode *n = node(S_CONTINUE);
		lex_next();
		return n;
	}
	case T_EXIT: {
		ANode *n = node(S_EXIT);

		lex_next();
		if (starts_expr(lx.tok))
			n->a = parse_expr(0);
		return n;
	}
	case T_RETURN: {
		ANode *n = node(S_RETURN);

		lex_next();
		if (starts_expr(lx.tok))
			n->a = parse_expr(0);
		return n;
	}
	default: {
		ANode *n = node(S_EXPR);

		n->a = parse_expr(0);
		return n;
	}
	}
}

static ANode *parse_statement(void)
{
	switch (lx.tok) {
	case T_LBRACE:
		return parse_block();
	case T_SEMI: {
		ANode *n = node(S_BLOCK);
		lex_next();
		return n;
	}
	case T_IF: {
		ANode *n = node(S_IF);

		lex_next();
		expect(T_LPAREN, "`('");
		n->a = parse_expr(0);
		expect(T_RPAREN, "`)'");
		skip_optional_newlines();
		n->b = parse_statement();
		{
			size_t save = lx.pos;
			int save_tok = lx.tok;
			char *save_str = lx.str ? xstrdup(lx.str) : NULL;

			skip_newlines();
			if (lx.tok == T_ELSE) {
				lex_next();
				skip_optional_newlines();
				n->c = parse_statement();
			} else {
				lx.pos = save;
				lx.tok = save_tok;
				free(lx.str);
				lx.str = save_str;
				save_str = NULL;
			}
			free(save_str);
		}
		return n;
	}
	case T_WHILE: {
		ANode *n = node(S_WHILE);

		lex_next();
		expect(T_LPAREN, "`('");
		n->a = parse_expr(0);
		expect(T_RPAREN, "`)'");
		skip_optional_newlines();
		n->b = parse_statement();
		return n;
	}
	case T_DO: {
		ANode *n = node(S_DO);

		lex_next();
		skip_optional_newlines();
		n->b = parse_statement();
		skip_newlines();
		expect(T_WHILE, "`while'");
		expect(T_LPAREN, "`('");
		n->a = parse_expr(0);
		expect(T_RPAREN, "`)'");
		return n;
	}
	case T_FOR: {
		lex_next();
		expect(T_LPAREN, "`('");
		/* for (k in array) */
		if (lx.tok == T_LPAREN || lx.tok == T_NAME) {
			size_t save = lx.pos;
			int save_tok = lx.tok;
			char *name = lx.str ? xstrdup(lx.str) : NULL;

			if (lx.tok == T_NAME) {
				lex_next();
				if (lx.tok == T_IN) {
					ANode *n = node(S_FORIN);

					lex_next();
					if (lx.tok != T_NAME)
						awk_fatal("for: expected an array name");
					n->str = name;
					n->a = node(E_STR);
					n->a->str = xstrdup(lx.str);
					lex_next();
					expect(T_RPAREN, "`)'");
					skip_optional_newlines();
					n->b = parse_statement();
					return n;
				}
				lx.pos = save;
				lx.tok = save_tok;
				free(lx.str);
				lx.str = name;
				name = NULL;
			}
			free(name);
		}
		{
			ANode *n = node(S_FOR);

			if (lx.tok != T_SEMI)
				n->a = parse_simple_statement();
			expect(T_SEMI, "`;'");
			skip_optional_newlines();
			if (lx.tok != T_SEMI)
				n->b = parse_expr(0);
			expect(T_SEMI, "`;'");
			skip_optional_newlines();
			if (lx.tok != T_RPAREN)
				n->c = parse_simple_statement();
			expect(T_RPAREN, "`)'");
			skip_optional_newlines();
			n->d = parse_statement();
			return n;
		}
	}
	default: {
		ANode *n = parse_simple_statement();

		if (lx.tok == T_SEMI)
			lex_next();
		return n;
	}
	}
}

static ANode *parse_block(void)
{
	ANode *n = node(S_BLOCK);

	expect(T_LBRACE, "`{'");
	for (;;) {
		skip_newlines();
		if (lx.tok == T_RBRACE || lx.tok == T_EOF)
			break;
		node_add(n, parse_statement());
	}
	expect(T_RBRACE, "`}'");
	return n;
}

static void parse_program(const char *src)
{
	Rule **tail = &aw.rules;

	lx.src = src;
	lx.pos = 0;
	lx.line = 1;
	lx.tok = T_NEWLINE;
	lex_next();

	for (;;) {
		Rule *r;

		skip_newlines();
		if (lx.tok == T_EOF)
			break;

		if (lx.tok == T_FUNCTION) {
			AFunc *f = xcalloc(1, sizeof *f);

			lex_next();
			if (lx.tok != T_NAME && lx.tok != T_FUNC_NAME)
				awk_fatal("function: expected a name");
			f->name = xstrdup(lx.str);
			vec_init(&f->params);
			lex_next();
			expect(T_LPAREN, "`('");
			while (lx.tok == T_NAME) {
				vec_pushs(&f->params, lx.str);
				lex_next();
				if (lx.tok == T_COMMA) {
					lex_next();
					skip_optional_newlines();
				}
			}
			expect(T_RPAREN, "`)'");
			skip_optional_newlines();
			f->body = parse_block();
			f->next = aw.funcs;
			aw.funcs = f;
			continue;
		}

		r = xcalloc(1, sizeof *r);
		if (lx.tok == T_BEGIN) {
			r->is_begin = 1;
			lex_next();
			skip_optional_newlines();
			r->action = parse_block();
		} else if (lx.tok == T_END) {
			r->is_end = 1;
			lex_next();
			skip_optional_newlines();
			r->action = parse_block();
		} else if (lx.tok == T_LBRACE) {
			r->action = parse_block();
		} else {
			r->pattern = parse_expr(0);
			if (lx.tok == T_COMMA) {
				lex_next();
				skip_optional_newlines();
				r->pattern2 = parse_expr(0);
			}
			if (lx.tok == T_LBRACE)
				r->action = parse_block();
		}
		*tail = r;
		tail = &r->next;
	}
}

/* -------------------------------------------------------------- the fields */

static void split_record(void);

static const char *get_record(void)
{
	if (!aw.record)
		aw.record = xstrdup("");
	return aw.record;
}

static void set_record(const char *s)
{
	free(aw.record);
	aw.record = xstrdup(s ? s : "");
	aw.fields_valid = 0;
	split_record();
}

/* Rebuild $0 from the fields after one has been assigned. */
static void rebuild_record(void)
{
	const char *ofs = var_string("OFS");
	Buf b;
	long i;

	buf_init(&b);
	for (i = 1; i <= aw.nf; i++) {
		if (i > 1)
			buf_puts(&b, ofs);
		if ((size_t)i - 1 < aw.fields.len)
			buf_puts(&b, aw.fields.v[i - 1]);
	}
	free(aw.record);
	aw.record = buf_take(&b);
}

static void split_into(const char *text, const char *fs, Vec *out);

static void split_record(void)
{
	if (aw.fields_valid)
		return;
	vec_clear(&aw.fields);
	split_into(get_record(), var_string("FS"), &aw.fields);
	aw.nf = (long)aw.fields.len;
	var_set_num("NF", (double)aw.nf);
	aw.fields_valid = 1;
}

static const char *field_get(long n)
{
	if (n == 0)
		return get_record();
	split_record();
	if (n < 1 || (size_t)n > aw.fields.len)
		return "";
	return aw.fields.v[n - 1];
}

static void field_set(long n, const char *value)
{
	if (n == 0) {
		set_record(value);
		return;
	}
	split_record();
	while ((size_t)n > aw.fields.len)
		vec_pushs(&aw.fields, "");
	free(aw.fields.v[n - 1]);
	aw.fields.v[n - 1] = xstrdup(value);
	if (n > aw.nf) {
		aw.nf = n;
		var_set_num("NF", (double)aw.nf);
	}
	rebuild_record();
}

/* Split text on FS: a single space means runs of blanks, a single character
 * is literal, anything longer is an ERE. */
static void split_into(const char *text, const char *fs, Vec *out)
{
	if (!fs)
		fs = " ";

	if (strcmp(fs, " ") == 0) {
		const char *p = text;

		while (*p) {
			const char *start;

			while (*p == ' ' || *p == '\t' || *p == '\n')
				p++;
			if (!*p)
				break;
			start = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n')
				p++;
			vec_push(out, xstrndup(start, (size_t)(p - start)));
		}
		return;
	}
	if (strlen(fs) == 1 && !strchr("\\^$.[]|()*+?{}", fs[0])) {
		const char *p = text;

		if (!*text)
			return;
		for (;;) {
			const char *q = strchr(p, fs[0]);

			if (!q) {
				vec_pushs(out, p);
				return;
			}
			vec_push(out, xstrndup(p, (size_t)(q - p)));
			p = q + 1;
		}
	}
	{
		const char *err = NULL;
		Rx *rx = rx_compile(fs, RX_ERE, 0, &err);
		size_t pos = 0, len = strlen(text);
		RxMatch m;

		if (!rx) {
			vec_pushs(out, text);
			return;
		}
		if (!len) {
			rx_free(rx);
			return;
		}
		while (pos <= len && rx_search(rx, text, len, pos, &m)) {
			if (m.end == m.start) {
				if ((size_t)m.start >= len)
					break;
				m.end = m.start + 1;
				vec_push(out, xstrndup(text + pos, (size_t)m.start - pos + 1));
				pos = (size_t)m.end;
				continue;
			}
			vec_push(out, xstrndup(text + pos, (size_t)m.start - pos));
			pos = (size_t)m.end;
		}
		vec_pushs(out, text + pos);
		rx_free(rx);
	}
}

/* --------------------------------------------------------------- evaluation */

static void eval(ANode *n, Cell *out);
static void exec_stmt(ANode *n);

static char *eval_str(ANode *n)
{
	Cell c;
	char *s;

	memset(&c, 0, sizeof c);
	eval(n, &c);
	s = xstrdup(cell_str(&c));
	cell_clear(&c);
	return s;
}

static double eval_num(ANode *n)
{
	Cell c;
	double v;

	memset(&c, 0, sizeof c);
	eval(n, &c);
	v = cell_num(&c);
	cell_clear(&c);
	return v;
}

static int eval_bool(ANode *n)
{
	Cell c;
	int v;

	memset(&c, 0, sizeof c);
	eval(n, &c);
	v = cell_true(&c);
	cell_clear(&c);
	return v;
}

/* Build the subscript for a[i, j] using SUBSEP. */
static char *index_key(ANode *n)
{
	Buf b;
	size_t i;
	const char *subsep = var_string("SUBSEP");

	buf_init(&b);
	for (i = 0; i < n->nlist; i++) {
		char *piece = eval_str(n->list[i]);

		if (i)
			buf_puts(&b, subsep);
		buf_puts(&b, piece);
		free(piece);
	}
	return buf_take(&b);
}

static void assign_to(ANode *target, Cell *value)
{
	switch (target->type) {
	case E_VAR: {
		Sym *s = sym_get(target->str);

		cell_copy(&s->val, value);
		if (strcmp(target->str, "NF") == 0) {
			long want = (long)cell_num(value);

			split_record();
			while ((size_t)want < aw.fields.len)
				free(vec_remove(&aw.fields, aw.fields.len - 1));
			while ((size_t)want > aw.fields.len)
				vec_pushs(&aw.fields, "");
			aw.nf = want;
			rebuild_record();
		}
		break;
	}
	case E_FIELD:
		field_set((long)eval_num(target->a), cell_str(value));
		break;
	case E_INDEX: {
		char *key = index_key(target);
		Cell *slot = array_get(sym_array(target->str), key, 1);

		cell_copy(slot, value);
		free(key);
		break;
	}
	default:
		awk_fatal("assignment to a non-lvalue");
		break;
	}
}

static void read_lvalue(ANode *target, Cell *out)
{
	eval(target, out);
}

static FILE *redirect_open(const char *name, int mode)
{
	struct Redirect *r;

	for (r = aw.redirects; r; r = r->next)
		if (strcmp(r->name, name) == 0)
			return r->fp;
	r = xcalloc(1, sizeof *r);
	r->name = xstrdup(name);
	if (mode == '|') {
		r->fp = popen(name, "w");
		r->is_pipe = 1;
	} else if (strcmp(name, "/dev/stdout") == 0) {
		r->fp = stdout;
	} else if (strcmp(name, "/dev/stderr") == 0) {
		r->fp = stderr;
	} else {
		r->fp = fopen(name, mode == 'A' ? "a" : "w");
	}
	if (!r->fp) {
		gnu_error("awk", "can't redirect to %s", name);
		free(r->name);
		free(r);
		return NULL;
	}
	r->next = aw.redirects;
	aw.redirects = r;
	return r->fp;
}

/* Input redirections for getline. */
static struct Redirect *input_redirect(const char *name, int mode)
{
	struct Redirect *r;

	for (r = aw.redirects; r; r = r->next)
		if (strcmp(r->name, name) == 0)
			return r;
	r = xcalloc(1, sizeof *r);
	r->name = xstrdup(name);
	if (mode == '|') {
		r->fp = popen(name, "r");
		r->is_pipe = 1;
	} else if (strcmp(name, "-") == 0 || strcmp(name, "/dev/stdin") == 0) {
		r->fp = stdin;
	} else {
		r->fp = fopen(name, "r");
	}
	r->next = aw.redirects;
	aw.redirects = r;
	return r;
}

static int redirect_close(const char *name)
{
	struct Redirect **p;

	for (p = &aw.redirects; *p; p = &(*p)->next) {
		if (strcmp((*p)->name, name) == 0) {
			struct Redirect *r = *p;
			int rc = 0;

			*p = r->next;
			if (r->fp && r->fp != stdout && r->fp != stderr && r->fp != stdin)
				rc = r->is_pipe ? pclose(r->fp) : fclose(r->fp);
			free(r->name);
			free(r);
			return rc;
		}
	}
	return -1;
}

static void close_all_redirects(void)
{
	while (aw.redirects) {
		struct Redirect *r = aw.redirects;

		aw.redirects = r->next;
		if (r->fp && r->fp != stdout && r->fp != stderr && r->fp != stdin) {
			if (r->is_pipe)
				pclose(r->fp);
			else
				fclose(r->fp);
		}
		free(r->name);
		free(r);
	}
}

/* awk's own printf, on top of the C one. */
static void awk_sprintf(ANode *n, size_t first, Buf *out)
{
	char *fmt = eval_str(n->list[first]);
	const char *p = fmt;
	size_t argi = first + 1;

	while (*p) {
		Buf spec;

		if (*p != '%') {
			buf_putc(out, *p++);
			continue;
		}
		p++;
		if (*p == '%') {
			buf_putc(out, '%');
			p++;
			continue;
		}
		buf_init(&spec);
		buf_putc(&spec, '%');
		while (*p && strchr("-+ #0'", *p))
			buf_putc(&spec, *p++);
		if (*p == '*') {
			buf_printf(&spec, "%d",
				   argi < n->nlist ? (int)eval_num(n->list[argi++]) : 0);
			p++;
		} else {
			while (isdigit((unsigned char)*p))
				buf_putc(&spec, *p++);
		}
		if (*p == '.') {
			buf_putc(&spec, *p++);
			if (*p == '*') {
				buf_printf(&spec, "%d",
					   argi < n->nlist ? (int)eval_num(n->list[argi++])
							   : 0);
				p++;
			} else {
				while (isdigit((unsigned char)*p))
					buf_putc(&spec, *p++);
			}
		}
		while (*p && strchr("hlL", *p))
			p++;

		switch (*p) {
		case 'd':
		case 'i': {
			double v = argi < n->nlist ? eval_num(n->list[argi++]) : 0;

			buf_puts(&spec, "lld");
			buf_printf(out, spec.b, (long long)v);
			break;
		}
		case 'o':
		case 'x':
		case 'X':
		case 'u': {
			double v = argi < n->nlist ? eval_num(n->list[argi++]) : 0;

			buf_puts(&spec, "ll");
			buf_putc(&spec, *p);
			buf_printf(out, spec.b, (long long)v);
			break;
		}
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
		case 'a':
		case 'A': {
			double v = argi < n->nlist ? eval_num(n->list[argi++]) : 0;

			buf_putc(&spec, *p);
			buf_printf(out, spec.b, v);
			break;
		}
		case 'c': {
			Cell c;

			memset(&c, 0, sizeof c);
			if (argi < n->nlist)
				eval(n->list[argi++], &c);
			buf_putc(&spec, 'c');
			if ((c.flags & C_STR) && c.str && *c.str)
				buf_printf(out, spec.b, c.str[0]);
			else
				buf_printf(out, spec.b, (int)cell_num(&c));
			cell_clear(&c);
			break;
		}
		case 's': {
			char *v = argi < n->nlist ? eval_str(n->list[argi++]) : xstrdup("");

			buf_putc(&spec, 's');
			buf_printf(out, spec.b, v);
			free(v);
			break;
		}
		default:
			buf_puts(out, spec.b);
			if (*p)
				buf_putc(out, *p);
			break;
		}
		if (*p)
			p++;
		buf_free(&spec);
	}
	free(fmt);
}

static void do_sub(ANode *n, Cell *out, int global)
{
	Rx *rx;
	char *pat = NULL;
	char *rep;
	ANode *target;
	char *subject;
	Buf result;
	size_t pos = 0, len;
	long count = 0;
	RxMatch m;

	if (n->nlist < 2)
		awk_fatal("sub: not enough arguments");

	if (n->list[0]->type == E_REGEX) {
		rx = n->list[0]->rx;
	} else {
		const char *err = NULL;

		pat = eval_str(n->list[0]);
		rx = rx_compile(pat, RX_ERE, 0, &err);
		if (!rx)
			awk_fatal("sub: bad pattern %s", pat);
	}
	rep = eval_str(n->list[1]);
	target = n->nlist > 2 ? n->list[2] : NULL;
	subject = target ? eval_str(target) : xstrdup(field_get(0));
	len = strlen(subject);

	buf_init(&result);
	while (pos <= len && rx_search(rx, subject, len, pos, &m)) {
		const char *r;

		buf_put(&result, subject + pos, (size_t)m.start - pos);
		for (r = rep; *r; r++) {
			if (*r == '\\' && r[1] == '&') {
				buf_putc(&result, '&');
				r++;
				continue;
			}
			if (*r == '\\' && r[1] == '\\') {
				buf_putc(&result, '\\');
				r++;
				continue;
			}
			if (*r == '&') {
				buf_put(&result, subject + m.start,
					(size_t)(m.end - m.start));
				continue;
			}
			buf_putc(&result, *r);
		}
		count++;
		if (m.end > m.start) {
			pos = (size_t)m.end;
		} else {
			if ((size_t)m.end < len)
				buf_putc(&result, subject[m.end]);
			pos = (size_t)m.end + 1;
		}
		if (!global)
			break;
	}
	if (pos < len)
		buf_put(&result, subject + pos, len - pos);

	if (count) {
		Cell v;

		memset(&v, 0, sizeof v);
		cell_set_str(&v, result.b ? result.b : "");
		if (target)
			assign_to(target, &v);
		else
			field_set(0, cell_str(&v));
		cell_clear(&v);
	}
	buf_free(&result);
	free(subject);
	free(rep);
	if (pat) {
		rx_free(rx);
		free(pat);
	}
	cell_set_num(out, (double)count);
}

static void call_builtin(ANode *n, Cell *out)
{
	switch (n->builtin) {
	case B_LENGTH: {
		if (!n->nlist) {
			cell_set_num(out, (double)strlen(field_get(0)));
			break;
		}
		if (n->list[0]->type == E_VAR) {
			Sym *s = sym_find(n->list[0]->str);

			if (s && s->arr) {
				cell_set_num(out, (double)s->arr->count);
				break;
			}
		}
		{
			char *s = eval_str(n->list[0]);

			cell_set_num(out, (double)strlen(s));
			free(s);
		}
		break;
	}
	case B_SUBSTR: {
		char *s = eval_str(n->list[0]);
		double dstart = n->nlist > 1 ? eval_num(n->list[1]) : 1;
		double dlen = n->nlist > 2 ? eval_num(n->list[2]) : 0;
		long slen = (long)strlen(s);
		long start = (long)(dstart < 0 ? dstart - 0.5 : dstart + 0.5);
		long take;

		if (n->nlist > 2) {
			long l = (long)(dlen < 0 ? dlen - 0.5 : dlen + 0.5);
			long end = start + l;

			if (start < 1)
				start = 1;
			take = end - start;
		} else {
			if (start < 1)
				start = 1;
			take = slen - start + 1;
		}
		if (start > slen || take <= 0) {
			cell_set_str(out, "");
		} else {
			if (start + take - 1 > slen)
				take = slen - start + 1;
			{
				char *piece = xstrndup(s + start - 1, (size_t)take);

				cell_set_str(out, piece);
				free(piece);
			}
		}
		free(s);
		break;
	}
	case B_INDEX: {
		char *s = eval_str(n->list[0]);
		char *t = n->nlist > 1 ? eval_str(n->list[1]) : xstrdup("");
		char *hit = strstr(s, t);

		cell_set_num(out, hit ? (double)(hit - s + 1) : 0);
		free(s);
		free(t);
		break;
	}
	case B_SPLIT: {
		char *s = eval_str(n->list[0]);
		const char *fs;
		char *fs_owned = NULL;
		Array *arr;
		Vec parts;
		size_t i;
		char key[32];

		if (n->nlist < 2 || n->list[1]->type != E_VAR)
			awk_fatal("split: the second argument must be an array");
		arr = sym_array(n->list[1]->str);
		array_clear(arr);
		if (n->nlist > 2) {
			if (n->list[2]->type == E_REGEX)
				fs = fs_owned = xstrdup(n->list[2]->str);
			else
				fs = fs_owned = eval_str(n->list[2]);
		} else {
			fs = var_string("FS");
		}
		vec_init(&parts);
		split_into(s, fs, &parts);
		for (i = 0; i < parts.len; i++) {
			snprintf(key, sizeof key, "%zu", i + 1);
			cell_set_input(array_get(arr, key, 1), parts.v[i]);
		}
		cell_set_num(out, (double)parts.len);
		vec_free(&parts);
		free(fs_owned);
		free(s);
		break;
	}
	case B_SUB:
		do_sub(n, out, 0);
		break;
	case B_GSUB:
		do_sub(n, out, 1);
		break;
	case B_MATCH: {
		char *s = eval_str(n->list[0]);
		Rx *rx;
		char *pat = NULL;
		RxMatch m;

		if (n->nlist < 2)
			awk_fatal("match: not enough arguments");
		if (n->list[1]->type == E_REGEX) {
			rx = n->list[1]->rx;
		} else {
			const char *err = NULL;

			pat = eval_str(n->list[1]);
			rx = rx_compile(pat, RX_ERE, 0, &err);
			if (!rx)
				awk_fatal("match: bad pattern %s", pat);
		}
		if (rx_search(rx, s, strlen(s), 0, &m)) {
			var_set_num("RSTART", (double)(m.start + 1));
			var_set_num("RLENGTH", (double)(m.end - m.start));
			cell_set_num(out, (double)(m.start + 1));
		} else {
			var_set_num("RSTART", 0);
			var_set_num("RLENGTH", -1);
			cell_set_num(out, 0);
		}
		if (pat) {
			rx_free(rx);
			free(pat);
		}
		free(s);
		break;
	}
	case B_SPRINTF: {
		Buf b;

		buf_init(&b);
		if (n->nlist)
			awk_sprintf(n, 0, &b);
		cell_set_str(out, b.b ? b.b : "");
		buf_free(&b);
		break;
	}
	case B_SIN: cell_set_num(out, sin(eval_num(n->list[0]))); break;
	case B_COS: cell_set_num(out, cos(eval_num(n->list[0]))); break;
	case B_ATAN2:
		cell_set_num(out, atan2(eval_num(n->list[0]), eval_num(n->list[1])));
		break;
	case B_EXP: cell_set_num(out, exp(eval_num(n->list[0]))); break;
	case B_LOG: cell_set_num(out, log(eval_num(n->list[0]))); break;
	case B_SQRT: cell_set_num(out, sqrt(eval_num(n->list[0]))); break;
	case B_INT: {
		double v = eval_num(n->list[0]);

		cell_set_num(out, v < 0 ? ceil(v) : floor(v));
		break;
	}
	case B_RAND:
		cell_set_num(out, (double)random() / ((double)RAND_MAX + 1));
		break;
	case B_SRAND: {
		static double prev_seed;
		double seed = n->nlist ? eval_num(n->list[0]) : (double)time(NULL);

		srandom((unsigned)seed);
		cell_set_num(out, prev_seed);
		prev_seed = seed;
		break;
	}
	case B_TOLOWER:
	case B_TOUPPER: {
		char *s = n->nlist ? eval_str(n->list[0]) : xstrdup("");
		char *p;

		for (p = s; *p; p++)
			*p = n->builtin == B_TOLOWER ? (char)tolower((unsigned char)*p)
						     : (char)toupper((unsigned char)*p);
		cell_set_str(out, s);
		free(s);
		break;
	}
	case B_SYSTEM: {
		char *cmd = eval_str(n->list[0]);
		int rc;

		fflush(stdout);
		rc = system(cmd);
		cell_set_num(out, rc == -1 ? -1 : (double)(rc >> 8));
		free(cmd);
		break;
	}
	case B_CLOSE: {
		char *name = eval_str(n->list[0]);

		cell_set_num(out, (double)redirect_close(name));
		free(name);
		break;
	}
	case B_FFLUSH:
		fflush(NULL);
		cell_set_num(out, 0);
		break;
	default:
		cell_set_str(out, "");
		break;
	}
}

static void call_function(ANode *n, Cell *out)
{
	AFunc *f;
	Sym *frame = NULL;
	Sym *saved_locals;
	size_t i;

	for (f = aw.funcs; f; f = f->next)
		if (strcmp(f->name, n->str) == 0)
			break;
	if (!f)
		awk_fatal("calling an undefined function %s", n->str);

	for (i = 0; i < f->params.len; i++) {
		Sym *s = xcalloc(1, sizeof *s);

		s->name = xstrdup(f->params.v[i]);
		if (i < n->nlist) {
			ANode *arg = n->list[i];

			/* arrays are passed by reference */
			if (arg->type == E_VAR) {
				Sym *src = sym_find(arg->str);

				if (src && src->arr) {
					s->arr = src->arr;
					s->next = frame;
					frame = s;
					continue;
				}
				if (!src) {
					/* an unset name may become an array in the callee */
					Sym *fresh = sym_get(arg->str);

					(void)fresh;
				}
			}
			{
				Cell v;

				memset(&v, 0, sizeof v);
				eval(arg, &v);
				cell_copy(&s->val, &v);
				cell_clear(&v);
			}
		}
		s->next = frame;
		frame = s;
	}

	saved_locals = aw.locals;
	aw.locals = frame;
	aw.in_function++;
	cell_clear(&aw.retval);
	aw.returning = 0;
	exec_stmt(f->body);
	aw.returning = 0;
	aw.in_function--;
	aw.locals = saved_locals;

	cell_copy(out, &aw.retval);
	cell_clear(&aw.retval);

	while (frame) {
		Sym *next = frame->next;

		free(frame->name);
		cell_clear(&frame->val);
		/* arrays passed by reference are not freed here */
		free(frame);
		frame = next;
	}
}

static int read_record(FILE *f, Buf *into);

static void do_getline(ANode *n, Cell *out)
{
	Buf line;
	int got = 0;

	buf_init(&line);

	if (n->op == '<') {
		char *name = eval_str(n->b);
		struct Redirect *r = input_redirect(name, '<');

		if (!r->fp) {
			cell_set_num(out, -1);
			free(name);
			buf_free(&line);
			return;
		}
		got = read_record(r->fp, &line);
		free(name);
	} else if (n->op == '|') {
		char *cmd = eval_str(n->b);
		struct Redirect *r = input_redirect(cmd, '|');

		if (!r->fp) {
			cell_set_num(out, -1);
			free(cmd);
			buf_free(&line);
			return;
		}
		fflush(stdout);
		got = read_record(r->fp, &line);
		if (got)
			var_set_num("NR", var_number("NR") + 1);
		free(cmd);
	} else {
		if (aw.input)
			got = read_record(aw.input, &line);
		if (got) {
			var_set_num("NR", var_number("NR") + 1);
			var_set_num("FNR", var_number("FNR") + 1);
		}
	}

	if (!got) {
		cell_set_num(out, 0);
		buf_free(&line);
		return;
	}
	if (n->a) {
		Cell v;

		memset(&v, 0, sizeof v);
		cell_set_input(&v, line.b ? line.b : "");
		assign_to(n->a, &v);
		cell_clear(&v);
	} else {
		set_record(line.b ? line.b : "");
		if (n->op != '<')
			; /* NR was already bumped above */
	}
	cell_set_num(out, 1);
	buf_free(&line);
}

static void eval(ANode *n, Cell *out)
{
	if (!n) {
		cell_set_str(out, "");
		return;
	}
	switch (n->type) {
	case E_NUM:
		cell_set_num(out, n->num);
		break;
	case E_STR:
		cell_set_str(out, n->str);
		break;
	case E_REGEX: {
		const char *rec = field_get(0);

		cell_set_num(out, rx_search(n->rx, rec, strlen(rec), 0, NULL) ? 1 : 0);
		break;
	}
	case E_VAR: {
		Sym *s = sym_find(n->str);

		if (strcmp(n->str, "NF") == 0)
			split_record();
		if (!s) {
			cell_clear(out);
			cell_set_str(out, "");
			out->flags = 0;
			break;
		}
		cell_copy(out, &s->val);
		break;
	}
	case E_FIELD:
		cell_set_input(out, field_get((long)eval_num(n->a)));
		break;
	case E_INDEX: {
		char *key = index_key(n);
		Cell *slot = array_get(sym_array(n->str), key, 1);

		cell_copy(out, slot);
		free(key);
		break;
	}
	case E_GROUP:
		if (n->nlist)
			eval(n->list[n->nlist - 1], out);
		break;
	case E_ASSIGN: {
		Cell rhs;

		memset(&rhs, 0, sizeof rhs);
		eval(n->b, &rhs);
		if (n->op) {
			Cell cur;
			double a, b, r;

			memset(&cur, 0, sizeof cur);
			read_lvalue(n->a, &cur);
			a = cell_num(&cur);
			b = cell_num(&rhs);
			switch (n->op) {
			case '+': r = a + b; break;
			case '-': r = a - b; break;
			case '*': r = a * b; break;
			case '/':
				if (b == 0)
					awk_fatal("division by zero");
				r = a / b;
				break;
			case '%':
				if (b == 0)
					awk_fatal("division by zero in %%");
				r = fmod(a, b);
				break;
			default: r = pow(a, b); break;
			}
			cell_clear(&cur);
			cell_set_num(&rhs, r);
		}
		assign_to(n->a, &rhs);
		cell_copy(out, &rhs);
		cell_clear(&rhs);
		break;
	}
	case E_CONCAT: {
		char *a = eval_str(n->a);
		char *b = eval_str(n->b);
		char *joined = xasprintf("%s%s", a, b);

		cell_set_str(out, joined);
		free(a);
		free(b);
		free(joined);
		break;
	}
	case E_BIN: {
		switch (n->op) {
		case 'a':
			cell_set_num(out, eval_bool(n->a) && eval_bool(n->b) ? 1 : 0);
			return;
		case 'o':
			cell_set_num(out, eval_bool(n->a) || eval_bool(n->b) ? 1 : 0);
			return;
		default:
			break;
		}
		if (strchr("<lg>=n", n->op)) {
			Cell a, b;
			int numeric;
			int r;

			memset(&a, 0, sizeof a);
			memset(&b, 0, sizeof b);
			eval(n->a, &a);
			eval(n->b, &b);
			numeric = ((a.flags & C_NUM) && !(a.flags & C_STR)) ||
				  (a.flags & C_STRNUM);
			numeric = numeric && (((b.flags & C_NUM) && !(b.flags & C_STR)) ||
					      (b.flags & C_STRNUM));
			if (!(a.flags & (C_STR | C_NUM)) || !(b.flags & (C_STR | C_NUM)))
				numeric = 1; /* uninitialised compares as both */
			if (numeric) {
				double x = cell_num(&a), y = cell_num(&b);

				r = x < y ? -1 : x > y ? 1 : 0;
			} else {
				r = strcmp(cell_str(&a), cell_str(&b));
				r = r < 0 ? -1 : r > 0 ? 1 : 0;
			}
			cell_clear(&a);
			cell_clear(&b);
			switch (n->op) {
			case '<': cell_set_num(out, r < 0); break;
			case 'l': cell_set_num(out, r <= 0); break;
			case '>': cell_set_num(out, r > 0); break;
			case 'g': cell_set_num(out, r >= 0); break;
			case '=': cell_set_num(out, r == 0); break;
			default: cell_set_num(out, r != 0); break;
			}
			return;
		}
		{
			double a = eval_num(n->a);
			double b = eval_num(n->b);

			switch (n->op) {
			case '+': cell_set_num(out, a + b); break;
			case '-': cell_set_num(out, a - b); break;
			case '*': cell_set_num(out, a * b); break;
			case '/':
				if (b == 0)
					awk_fatal("division by zero");
				cell_set_num(out, a / b);
				break;
			case '%':
				if (b == 0)
					awk_fatal("division by zero in %%");
				cell_set_num(out, fmod(a, b));
				break;
			case '^': cell_set_num(out, pow(a, b)); break;
			default: cell_set_num(out, 0); break;
			}
		}
		break;
	}
	case E_UN: {
		if (n->op == '!') {
			cell_set_num(out, eval_bool(n->a) ? 0 : 1);
			break;
		}
		{
			double v = eval_num(n->a);

			cell_set_num(out, n->op == '-' ? -v : v);
		}
		break;
	}
	case E_INCDEC: {
		Cell cur;
		double v;

		memset(&cur, 0, sizeof cur);
		read_lvalue(n->a, &cur);
		v = cell_num(&cur);
		cell_clear(&cur);
		{
			Cell nv;

			memset(&nv, 0, sizeof nv);
			cell_set_num(&nv, n->op == '+' ? v + 1 : v - 1);
			assign_to(n->a, &nv);
			cell_set_num(out, n->num ? v : cell_num(&nv));
			cell_clear(&nv);
		}
		break;
	}
	case E_TERNARY:
		if (eval_bool(n->a))
			eval(n->b, out);
		else
			eval(n->c, out);
		break;
	case E_MATCH: {
		char *s = eval_str(n->a);
		Rx *rx;
		char *pat = NULL;
		int hit;

		if (n->b->type == E_REGEX) {
			rx = n->b->rx;
		} else {
			const char *err = NULL;

			pat = eval_str(n->b);
			rx = rx_compile(pat, RX_ERE, 0, &err);
			if (!rx)
				awk_fatal("bad dynamic regexp %s", pat);
		}
		hit = rx_search(rx, s, strlen(s), 0, NULL) ? 1 : 0;
		if (pat) {
			rx_free(rx);
			free(pat);
		}
		free(s);
		cell_set_num(out, n->op == '~' ? hit : !hit);
		break;
	}
	case E_IN: {
		char *key;
		Array *arr = sym_array(n->str);

		if (n->a->type == E_GROUP) {
			Buf b;
			size_t i;
			const char *subsep = var_string("SUBSEP");

			buf_init(&b);
			for (i = 0; i < n->a->nlist; i++) {
				char *piece = eval_str(n->a->list[i]);

				if (i)
					buf_puts(&b, subsep);
				buf_puts(&b, piece);
				free(piece);
			}
			key = buf_take(&b);
		} else {
			key = eval_str(n->a);
		}
		cell_set_num(out, array_get(arr, key, 0) ? 1 : 0);
		free(key);
		break;
	}
	case E_CALL:
		call_function(n, out);
		break;
	case E_BUILTIN:
		call_builtin(n, out);
		break;
	case E_GETLINE:
		do_getline(n, out);
		break;
	default:
		cell_set_str(out, "");
		break;
	}
}

/* --------------------------------------------------------------- statements */

static FILE *output_for(ANode *n)
{
	if (!n->redirect)
		return stdout;
	{
		char *name = eval_str(n->d);
		FILE *f = redirect_open(name, n->redirect);

		free(name);
		return f ? f : stdout;
	}
}

static void exec_stmt(ANode *n)
{
	if (!n || aw.exiting || aw.returning || aw.next_record || aw.next_file ||
	    aw.breaking || aw.continuing)
		return;

	switch (n->type) {
	case S_BLOCK: {
		size_t i;

		for (i = 0; i < n->nlist; i++) {
			exec_stmt(n->list[i]);
			if (aw.exiting || aw.returning || aw.next_record || aw.next_file ||
			    aw.breaking || aw.continuing)
				return;
		}
		break;
	}
	case S_EXPR: {
		Cell c;

		memset(&c, 0, sizeof c);
		eval(n->a, &c);
		cell_clear(&c);
		break;
	}
	case S_PRINT: {
		FILE *out = output_for(n);
		const char *ofs = var_string("OFS");
		const char *ors = var_string("ORS");
		size_t i;

		if (!n->nlist) {
			fputs(field_get(0), out);
		} else {
			for (i = 0; i < n->nlist; i++) {
				Cell c;
				char *s;

				memset(&c, 0, sizeof c);
				eval(n->list[i], &c);
				s = cell_output_str(&c);
				if (i)
					fputs(ofs, out);
				fputs(s, out);
				free(s);
				cell_clear(&c);
			}
		}
		fputs(ors, out);
		break;
	}
	case S_PRINTF: {
		FILE *out = output_for(n);
		Buf b;

		buf_init(&b);
		awk_sprintf(n, 0, &b);
		fwrite(b.b ? b.b : "", 1, b.len, out);
		buf_free(&b);
		break;
	}
	case S_IF:
		if (eval_bool(n->a))
			exec_stmt(n->b);
		else
			exec_stmt(n->c);
		break;
	case S_WHILE:
		while (eval_bool(n->a)) {
			exec_stmt(n->b);
			if (aw.breaking) {
				aw.breaking = 0;
				break;
			}
			aw.continuing = 0;
			if (aw.exiting || aw.returning || aw.next_record || aw.next_file)
				break;
		}
		break;
	case S_DO:
		do {
			exec_stmt(n->b);
			if (aw.breaking) {
				aw.breaking = 0;
				break;
			}
			aw.continuing = 0;
			if (aw.exiting || aw.returning || aw.next_record || aw.next_file)
				break;
		} while (eval_bool(n->a));
		break;
	case S_FOR:
		if (n->a)
			exec_stmt(n->a);
		for (;;) {
			if (n->b && !eval_bool(n->b))
				break;
			exec_stmt(n->d);
			if (aw.breaking) {
				aw.breaking = 0;
				break;
			}
			aw.continuing = 0;
			if (aw.exiting || aw.returning || aw.next_record || aw.next_file)
				break;
			if (n->c)
				exec_stmt(n->c);
		}
		break;
	case S_FORIN: {
		Array *arr = sym_array(n->a->str);
		Vec keys;
		size_t i;

		vec_init(&keys);
		array_keys(arr, &keys);
		for (i = 0; i < keys.len; i++) {
			Cell v;

			memset(&v, 0, sizeof v);
			cell_set_input(&v, keys.v[i]);
			{
				ANode target;

				memset(&target, 0, sizeof target);
				target.type = E_VAR;
				target.str = n->str;
				assign_to(&target, &v);
			}
			cell_clear(&v);
			exec_stmt(n->b);
			if (aw.breaking) {
				aw.breaking = 0;
				break;
			}
			aw.continuing = 0;
			if (aw.exiting || aw.returning || aw.next_record || aw.next_file)
				break;
		}
		vec_free(&keys);
		break;
	}
	case S_DELETE: {
		Array *arr = sym_array(n->str);

		if (!n->nlist) {
			array_clear(arr);
		} else {
			char *key = index_key(n);

			array_delete(arr, key);
			free(key);
		}
		break;
	}
	case S_NEXT:
		aw.next_record = 1;
		break;
	case S_NEXTFILE:
		aw.next_file = 1;
		break;
	case S_BREAK:
		aw.breaking = 1;
		break;
	case S_CONTINUE:
		aw.continuing = 1;
		break;
	case S_EXIT:
		if (n->a)
			aw.exit_code = (int)eval_num(n->a);
		aw.exiting = 1;
		break;
	case S_RETURN:
		cell_clear(&aw.retval);
		if (n->a) {
			Cell c;

			memset(&c, 0, sizeof c);
			eval(n->a, &c);
			cell_copy(&aw.retval, &c);
			cell_clear(&c);
		}
		aw.returning = 1;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------- input */

static int read_record(FILE *f, Buf *into)
{
	const char *rs = var_string("RS");
	int c;

	buf_reset(into);
	if (!f)
		return 0;

	if (rs && !*rs) {
		/* paragraph mode */
		int blank_seen = 0;

		while ((c = getc(f)) != EOF) {
			if (c == '\n') {
				if (!into->len)
					continue;
				if (blank_seen)
					return 1;
				blank_seen = 1;
				continue;
			}
			if (blank_seen) {
				buf_putc(into, '\n');
				blank_seen = 0;
			}
			buf_putc(into, c);
		}
		return into->len > 0;
	}
	if (rs && strlen(rs) > 1) {
		/* RS as a regular expression */
		const char *err = NULL;
		Rx *rx = rx_compile(rs, RX_ERE, 0, &err);
		RxMatch m;

		while ((c = getc(f)) != EOF) {
			buf_putc(into, c);
			if (rx && rx_search(rx, into->b, into->len, 0, &m) &&
			    (size_t)m.end == into->len && m.end > m.start) {
				into->len = (size_t)m.start;
				into->b[into->len] = '\0';
				rx_free(rx);
				return 1;
			}
		}
		if (rx)
			rx_free(rx);
		return into->len > 0;
	}
	{
		int sep = rs && *rs ? rs[0] : '\n';

		while ((c = getc(f)) != EOF) {
			if (c == sep)
				return 1;
			buf_putc(into, c);
		}
	}
	return into->len > 0;
}

/* Assignments of the form var=value on the command line. */
static int apply_assignment(const char *arg)
{
	const char *eq = strchr(arg, '=');
	char *name;
	Cell v;

	if (!eq || eq == arg)
		return 0;
	{
		const char *p;

		for (p = arg; p < eq; p++)
			if (!(isalnum((unsigned char)*p) || *p == '_'))
				return 0;
		if (isdigit((unsigned char)arg[0]))
			return 0;
	}
	name = xstrndup(arg, (size_t)(eq - arg));
	memset(&v, 0, sizeof v);
	cell_set_input(&v, eq + 1);
	cell_copy(&sym_get(name)->val, &v);
	cell_clear(&v);
	free(name);
	return 1;
}

static void run_rules(void)
{
	Rule *r;

	for (r = aw.rules; r; r = r->next) {
		int selected;

		if (r->is_begin || r->is_end)
			continue;
		if (!r->pattern) {
			selected = 1;
		} else if (r->pattern2) {
			if (r->range_active) {
				selected = 1;
				if (eval_bool(r->pattern2))
					r->range_active = 0;
			} else if (eval_bool(r->pattern)) {
				selected = 1;
				r->range_active = 1;
				if (eval_bool(r->pattern2))
					r->range_active = 0;
			} else {
				selected = 0;
			}
		} else {
			selected = eval_bool(r->pattern);
		}
		if (!selected)
			continue;
		if (r->action)
			exec_stmt(r->action);
		else
			printf("%s%s", field_get(0), var_string("ORS"));
		if (aw.exiting || aw.next_record || aw.next_file)
			return;
	}
}

static int next_input(void)
{
	while (aw.input_index < aw.input_files.len) {
		const char *name = aw.input_files.v[aw.input_index++];

		if (apply_assignment(name))
			continue;
		var_set_str_("FILENAME", name);
		var_set_num("FNR", 0);
		if (strcmp(name, "-") == 0 || strcmp(name, "/dev/stdin") == 0) {
			aw.input = stdin;
			aw.input_is_stdin = 1;
			return 1;
		}
		aw.input = fopen(name, "r");
		aw.input_is_stdin = 0;
		if (!aw.input) {
			gnu_file_error("awk", name);
			aw.exit_code = 2;
			continue;
		}
		return 1;
	}
	return 0;
}

int gnu_awk(int argc, char **argv)
{
	Buf program;
	int have_program = 0;
	int i;
	Vec assignments;
	Rule *r;

	memset(&aw, 0, sizeof aw);
	aw.prog = "awk";
	vec_init(&aw.fields);
	vec_init(&aw.input_files);
	vec_init(&assignments);
	buf_init(&program);

	var_set_str_("FS", " ");
	var_set_str_("OFS", " ");
	var_set_str_("ORS", "\n");
	var_set_str_("RS", "\n");
	var_set_str_("SUBSEP", "\034");
	var_set_str_("CONVFMT", "%.6g");
	var_set_str_("OFMT", "%.6g");
	var_set_num("NR", 0);
	var_set_num("NF", 0);
	var_set_num("FNR", 0);
	var_set_num("RSTART", 0);
	var_set_num("RLENGTH", -1);
	var_set_str_("FILENAME", "");
	{
		Array *env = sym_array("ENVIRON");
		char **e = vars_environ();

		for (; *e; e++) {
			char *eq = strchr(*e, '=');

			if (!eq)
				continue;
			{
				char *name = xstrndup(*e, (size_t)(eq - *e));

				cell_set_input(array_get(env, name, 1), eq + 1);
				free(name);
			}
		}
	}

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] && strcmp(a, "-") != 0) {
			if (strcmp(a, "--") == 0) {
				i++;
				break;
			}
			if (gnu_long_opt(a, "field-separator", &val)) {
				var_set_str_("FS", val ? val : argv[++i]);
				continue;
			}
			if (gnu_long_opt(a, "assign", &val)) {
				apply_assignment(val ? val : argv[++i]);
				continue;
			}
			if (gnu_long_opt(a, "file", &val)) {
				const char *path = val ? val : argv[++i];
				FILE *f = fopen(path, "r");
				char chunk[4096];
				size_t n;

				if (!f) {
					gnu_file_error("awk", path);
					return 2;
				}
				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					buf_put(&program, chunk, n);
				buf_putc(&program, '\n');
				fclose(f);
				have_program = 1;
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: awk [-F fs] [-v var=value] "
				       "['program' | -f progfile] [file ...]\n");
				return 0;
			}
			if (gnu_long_opt(a, "version", NULL)) {
				printf("awk (CriSH %s) - POSIX awk\n", CRISH_VERSION);
				return 0;
			}
			if (a[1] == 'F') {
				const char *fs = a[2] ? a + 2 : argv[++i];

				if (strcmp(fs, "t") == 0)
					fs = "\t";
				var_set_str_("FS", fs);
				continue;
			}
			if (a[1] == 'v') {
				apply_assignment(a[2] ? a + 2 : argv[++i]);
				continue;
			}
			if (a[1] == 'f') {
				const char *path = a[2] ? a + 2 : argv[++i];
				FILE *f = fopen(path, "r");
				char chunk[4096];
				size_t n;

				if (!f) {
					gnu_file_error("awk", path);
					return 2;
				}
				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					buf_put(&program, chunk, n);
				buf_putc(&program, '\n');
				fclose(f);
				have_program = 1;
				continue;
			}
			gnu_error("awk", "unknown option %s", a);
			return 2;
		}
		if (!have_program) {
			buf_puts(&program, a);
			have_program = 1;
			continue;
		}
		vec_pushs(&aw.input_files, a);
	}
	for (; i < argc; i++) {
		if (!have_program) {
			buf_puts(&program, argv[i]);
			have_program = 1;
			continue;
		}
		vec_pushs(&aw.input_files, argv[i]);
	}

	if (!have_program) {
		gnu_error("awk", "usage: awk [-F fs] [-v var=value] 'program' [file ...]");
		buf_free(&program);
		return 2;
	}

	{
		Array *av = sym_array("ARGV");
		char key[16];
		size_t k;

		cell_set_input(array_get(av, "0", 1), "awk");
		for (k = 0; k < aw.input_files.len; k++) {
			snprintf(key, sizeof key, "%zu", k + 1);
			cell_set_input(array_get(av, key, 1), aw.input_files.v[k]);
		}
		var_set_num("ARGC", (double)(aw.input_files.len + 1));
	}

	parse_program(program.b ? program.b : "");
	buf_free(&program);

	for (r = aw.rules; r; r = r->next) {
		if (r->is_begin) {
			exec_stmt(r->action);
			if (aw.exiting)
				break;
		}
	}

	{
		int needs_input = 0;

		for (r = aw.rules; r; r = r->next)
			if (!r->is_begin)
				needs_input = 1;

		if (needs_input && !aw.exiting) {
			Buf line;

			buf_init(&line);
			if (!aw.input_files.len)
				vec_pushs(&aw.input_files, "-");
			while (next_input()) {
				aw.next_file = 0;
				while (read_record(aw.input, &line)) {
					var_set_num("NR", var_number("NR") + 1);
					var_set_num("FNR", var_number("FNR") + 1);
					set_record(line.b ? line.b : "");
					aw.next_record = 0;
					run_rules();
					aw.next_record = 0;
					if (aw.exiting || aw.next_file)
						break;
				}
				if (aw.input && !aw.input_is_stdin)
					fclose(aw.input);
				aw.input = NULL;
				if (aw.exiting)
					break;
			}
			buf_free(&line);
		}
	}

	aw.exiting = 0;
	for (r = aw.rules; r; r = r->next) {
		if (r->is_end) {
			exec_stmt(r->action);
			if (aw.exiting)
				break;
		}
	}

	close_all_redirects();
	fflush(stdout);
	vec_free(&aw.fields);
	vec_free(&aw.input_files);
	vec_free(&assignments);
	free(aw.record);
	return aw.exit_code;
}

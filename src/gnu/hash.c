/* hash.c - sha256sum, sha1sum, sha512sum, md5sum and base64.
 *
 * macOS has none of the GNU *sum tools; it has `shasum -a 256` with different
 * output, and a `base64` without -w or a long -d.  The digests themselves come
 * from CommonCrypto, which is part of libSystem, so nothing extra is linked.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* md5sum must keep working; the deprecation is about new security uses. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gnu.h"

typedef struct {
	const char *name;
	int bits;
	size_t digest_len;
} Algo;

static const Algo algos[] = {
	{ "md5sum", 128, CC_MD5_DIGEST_LENGTH },
	{ "sha1sum", 160, CC_SHA1_DIGEST_LENGTH },
	{ "sha256sum", 256, CC_SHA256_DIGEST_LENGTH },
	{ "sha512sum", 512, CC_SHA512_DIGEST_LENGTH },
};

static const Algo *algo_for(const char *prog)
{
	size_t i;

	for (i = 0; i < sizeof algos / sizeof algos[0]; i++)
		if (strcmp(algos[i].name, prog) == 0)
			return &algos[i];
	return &algos[2]; /* sha256 */
}

static int digest_stream(FILE *f, const Algo *a, unsigned char *out)
{
	unsigned char chunk[65536];
	size_t n;

	if (a->bits == 128) {
		CC_MD5_CTX ctx;
		CC_MD5_Init(&ctx);
		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
			CC_MD5_Update(&ctx, chunk, (CC_LONG)n);
		CC_MD5_Final(out, &ctx);
	} else if (a->bits == 160) {
		CC_SHA1_CTX ctx;
		CC_SHA1_Init(&ctx);
		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
			CC_SHA1_Update(&ctx, chunk, (CC_LONG)n);
		CC_SHA1_Final(out, &ctx);
	} else if (a->bits == 512) {
		CC_SHA512_CTX ctx;
		CC_SHA512_Init(&ctx);
		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
			CC_SHA512_Update(&ctx, chunk, (CC_LONG)n);
		CC_SHA512_Final(out, &ctx);
	} else {
		CC_SHA256_CTX ctx;
		CC_SHA256_Init(&ctx);
		while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
			CC_SHA256_Update(&ctx, chunk, (CC_LONG)n);
		CC_SHA256_Final(out, &ctx);
	}
	return 0;
}

static char *hex_of(const unsigned char *d, size_t len)
{
	static const char hexd[] = "0123456789abcdef";
	char *out = xmalloc(len * 2 + 1);
	size_t i;

	for (i = 0; i < len; i++) {
		out[i * 2] = hexd[d[i] >> 4];
		out[i * 2 + 1] = hexd[d[i] & 15];
	}
	out[len * 2] = '\0';
	return out;
}

char *sha256_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char digest[CC_SHA512_DIGEST_LENGTH];
	char *hex;

	if (!f)
		return NULL;
	digest_stream(f, &algos[2], digest);
	fclose(f);
	hex = hex_of(digest, CC_SHA256_DIGEST_LENGTH);
	return hex;
}

int gnu_sha(int argc, char **argv)
{
	const char *prog = argv[0];
	const Algo *a;
	int check = 0, binary = 0, tag = 0, quiet = 0, status_only = 0;
	int i, rc = 0;
	Vec files;
	size_t k;

	/* the shell may have invoked us through a path or a link */
	{
		const char *slash = strrchr(prog, '/');
		if (slash)
			prog = slash + 1;
	}
	a = algo_for(prog);

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (arg[0] == '-' && arg[1]) {
			const char *p;

			if (gnu_long_opt(arg, "check", NULL)) {
				check = 1;
				continue;
			}
			if (gnu_long_opt(arg, "tag", NULL)) {
				tag = 1;
				continue;
			}
			if (gnu_long_opt(arg, "binary", NULL)) {
				binary = 1;
				continue;
			}
			if (gnu_long_opt(arg, "quiet", NULL)) {
				quiet = 1;
				continue;
			}
			if (gnu_long_opt(arg, "status", NULL)) {
				status_only = quiet = 1;
				continue;
			}
			if (gnu_long_opt(arg, "text", NULL))
				continue;
			for (p = arg + 1; *p; p++) {
				switch (*p) {
				case 'c': check = 1; break;
				case 'b': binary = 1; break;
				case 't': break;
				case 'q': quiet = 1; break;
				default:
					gnu_error(prog, "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&files, arg);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	if (check) {
		size_t bad = 0, seen = 0;

		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open(prog, files.v[k], &is_stdin);
			Buf line;

			if (!f) {
				rc = 1;
				continue;
			}
			buf_init(&line);
			while (gnu_getdelim(&line, f, '\n') >= 0) {
				char *sep, *want, *name;
				FILE *target;
				unsigned char digest[CC_SHA512_DIGEST_LENGTH];
				char *have;

				if (line.len && line.b[line.len - 1] == '\n')
					line.b[--line.len] = '\0';
				if (!line.len)
					continue;
				sep = strstr(line.b, "  ");
				if (!sep)
					sep = strstr(line.b, " *");
				if (!sep)
					continue;
				want = xstrndup(line.b, (size_t)(sep - line.b));
				name = sep + 2;
				seen++;
				target = fopen(name, "rb");
				if (!target) {
					if (!status_only)
						printf("%s: FAILED open or read\n", name);
					bad++;
					free(want);
					continue;
				}
				digest_stream(target, a, digest);
				fclose(target);
				have = hex_of(digest, a->digest_len);
				if (strcasecmp(have, want) == 0) {
					if (!quiet)
						printf("%s: OK\n", name);
				} else {
					if (!status_only)
						printf("%s: FAILED\n", name);
					bad++;
				}
				free(have);
				free(want);
			}
			buf_free(&line);
			gnu_close(f, is_stdin);
		}
		if (bad) {
			if (!status_only)
				gnu_error(prog, "WARNING: %zu of %zu computed checksums did "
					       "NOT match",
					  bad, seen);
			rc = 1;
		}
		vec_free(&files);
		return rc;
	}

	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *f = gnu_open(prog, files.v[k], &is_stdin);
		unsigned char digest[CC_SHA512_DIGEST_LENGTH];
		char *hex;

		if (!f) {
			rc = 1;
			continue;
		}
		digest_stream(f, a, digest);
		gnu_close(f, is_stdin);
		hex = hex_of(digest, a->digest_len);
		if (tag) {
			char upper[16];
			size_t j;

			snprintf(upper, sizeof upper, "%s", prog);
			for (j = 0; upper[j] && upper[j] != 's'; j++)
				upper[j] = (char)toupper((unsigned char)upper[j]);
			{
				char *sum = strstr(upper, "sum");
				if (sum)
					*sum = '\0';
			}
			for (j = 0; upper[j]; j++)
				upper[j] = (char)toupper((unsigned char)upper[j]);
			printf("%s (%s) = %s\n", upper, files.v[k], hex);
		} else {
			printf("%s %c%s\n", hex, binary ? '*' : ' ',
			       is_stdin ? "-" : files.v[k]);
		}
		free(hex);
	}
	vec_free(&files);
	return rc;
}

/* ------------------------------------------------------------------ base64 */

static const char b64chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int gnu_base64(int argc, char **argv)
{
	int decode = 0, ignore_garbage = 0;
	long wrap = 76;
	int i;
	const char *path = NULL;
	FILE *f;
	int is_stdin = 1;
	Buf input;
	char chunk[65536];
	size_t n;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			if (gnu_long_opt(a, "decode", NULL)) {
				decode = 1;
				continue;
			}
			if (gnu_long_opt(a, "ignore-garbage", NULL)) {
				ignore_garbage = 1;
				continue;
			}
			if (gnu_long_opt(a, "wrap", &val)) {
				wrap = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (a[1] == 'd' && !a[2]) {
				decode = 1;
				continue;
			}
			if (a[1] == 'i' && !a[2]) {
				ignore_garbage = 1;
				continue;
			}
			if (a[1] == 'w') {
				wrap = a[2] ? strtol(a + 2, NULL, 10)
					    : strtol(argv[++i], NULL, 10);
				continue;
			}
			if (strcmp(a, "-") == 0) {
				path = NULL;
				continue;
			}
			gnu_error("base64", "invalid option -- '%s'", a);
			return 1;
		}
		path = a;
	}

	f = gnu_open("base64", path, &is_stdin);
	if (!f)
		return 1;
	buf_init(&input);
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_put(&input, chunk, n);
	gnu_close(f, is_stdin);

	if (!decode) {
		size_t j;
		long col = 0;

		for (j = 0; j < input.len; j += 3) {
			unsigned char b0 = (unsigned char)input.b[j];
			unsigned char b1 = j + 1 < input.len ? (unsigned char)input.b[j + 1] : 0;
			unsigned char b2 = j + 2 < input.len ? (unsigned char)input.b[j + 2] : 0;
			char quad[4];

			quad[0] = b64chars[b0 >> 2];
			quad[1] = b64chars[((b0 & 3) << 4) | (b1 >> 4)];
			quad[2] = j + 1 < input.len ? b64chars[((b1 & 15) << 2) | (b2 >> 6)]
						    : '=';
			quad[3] = j + 2 < input.len ? b64chars[b2 & 63] : '=';
			{
				int q;
				for (q = 0; q < 4; q++) {
					putchar(quad[q]);
					if (wrap > 0 && ++col == wrap) {
						putchar('\n');
						col = 0;
					}
				}
			}
		}
		if (wrap > 0 && col)
			putchar('\n');
		buf_free(&input);
		return 0;
	}

	{
		int rev[256];
		size_t j;
		int acc = 0, bits = 0;

		for (j = 0; j < 256; j++)
			rev[j] = -1;
		for (j = 0; j < 64; j++)
			rev[(unsigned char)b64chars[j]] = (int)j;

		for (j = 0; j < input.len; j++) {
			unsigned char c = (unsigned char)input.b[j];
			int v;

			if (c == '=' )
				break;
			v = rev[c];
			if (v < 0) {
				if (isspace(c) || ignore_garbage)
					continue;
				gnu_error("base64", "invalid input");
				buf_free(&input);
				return 1;
			}
			acc = (acc << 6) | v;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				putchar((acc >> bits) & 0xff);
			}
		}
	}
	buf_free(&input);
	return 0;
}

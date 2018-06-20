/*
 * FRR string processing utilities.
 * Copyright (C) 2018  Cumulus Networks, Inc.
 *                     Quentin Young
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/time.h>

#include "frrstr.h"
#include "memory.h"
#include "vector.h"

void frrstr_split(const char *string, const char *delimiter, char ***result,
		  int *argc)
{
	if (!string)
		return;

	unsigned int sz = 4, idx = 0;
	char *copy, *copystart;
	*result = XCALLOC(MTYPE_TMP, sizeof(char *) * sz);
	copystart = copy = XSTRDUP(MTYPE_TMP, string);
	*argc = 0;

	const char *tok = NULL;

	while (copy) {
		tok = strsep(&copy, delimiter);
		(*result)[idx] = XSTRDUP(MTYPE_TMP, tok);
		if (++idx == sz)
			*result = XREALLOC(MTYPE_TMP, *result,
					   (sz *= 2) * sizeof(char *));
		(*argc)++;
	}

	XFREE(MTYPE_TMP, copystart);
}

vector frrstr_split_vec(const char *string, const char *delimiter)
{
	char **result;
	int argc;

	if (!string)
		return NULL;

	frrstr_split(string, delimiter, &result, &argc);

	vector v = array_to_vector((void **)result, argc);

	XFREE(MTYPE_TMP, result);

	return v;
}

char *frrstr_join(const char **parts, int argc, const char *join)
{
	int i;
	char *str;
	char *p;
	size_t len = 0;
	size_t joinlen = join ? strlen(join) : 0;

	if (!argc)
		return NULL;

	for (i = 0; i < argc; i++)
		len += strlen(parts[i]);
	len += argc * joinlen + 1;

	if (!len)
		return NULL;

	p = str = XMALLOC(MTYPE_TMP, len);

	for (i = 0; i < argc; i++) {
		size_t arglen = strlen(parts[i]);

		memcpy(p, parts[i], arglen);
		p += arglen;
		if (i + 1 != argc && join) {
			memcpy(p, join, joinlen);
			p += joinlen;
		}
	}

	*p = '\0';

	return str;
}

char *frrstr_join_vec(vector v, const char *join)
{
	char **argv;
	int argc;

	vector_to_array(v, (void ***)&argv, &argc);

	char *ret = frrstr_join((const char **)argv, argc, join);

	XFREE(MTYPE_TMP, argv);

	return ret;
}

void frrstr_filter_vec(vector v, regex_t *filter)
{
	regmatch_t ignored[1];

	for (unsigned int i = 0; i < vector_active(v); i++) {
		if (regexec(filter, vector_slot(v, i), 0, ignored, 0)) {
			XFREE(MTYPE_TMP, vector_slot(v, i));
			vector_unset(v, i);
		}
	}
}

void frrstr_strvec_free(vector v)
{
	unsigned int i;
	char *cp;

	if (!v)
		return;

	for (i = 0; i < vector_active(v); i++) {
		cp = vector_slot(v, i);
		XFREE(MTYPE_TMP, cp);
	}

	vector_free(v);
}

bool begins_with(const char *str, const char *prefix)
{
	if (!str || !prefix)
		return 0;

	size_t lenstr = strlen(str);
	size_t lenprefix = strlen(prefix);

	if (lenprefix > lenstr)
		return 0;

	return strncmp(str, prefix, lenprefix) == 0;
}

static const unsigned char itoa64[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void to64(char *s, long v, int n)
{
	while (--n >= 0) {
		*s++ = itoa64[v & 0x3f];
		v >>= 6;
	}
}

char *zencrypt(const char *passwd)
{
	char salt[6];
	struct timeval tv;
	char *crypt(const char *, const char *);

	gettimeofday(&tv, 0);

	to64(&salt[0], random(), 3);
	to64(&salt[3], tv.tv_usec, 3);
	salt[5] = '\0';

	return crypt(passwd, salt);
}

#define MOD(a, b) ((((a) % (b)) + (b)) % (b))

char *caesar(bool encrypt, char *text, const char *key)
{
        size_t kl = strlen(key);
        size_t tl = strlen(text);
        int16_t w[tl + 1];

        for (size_t i = 0; i < tl; ++i)
                if (!(text[i] >= 33 && text[i] <= 126))
                        return NULL;

        for (size_t i = 0; i < kl; ++i)
                if (!(key[i] >= 33 && key[i] <= 126))
                        return NULL;

        for (size_t i = 0; i < tl; ++i) {
                w[i] = text[i];
                w[i] += -33 + (2 * !!encrypt - 1) * key[i % kl];
                w[i] = MOD((w[i]), (127 - 33)) + 33;
        }

        for (size_t i = 0; i < tl; i++)
                text[i] = w[i];

        w[tl] = 0x00;

        return text;
}

int all_digit(const char *str)
{
	for (; *str != '\0'; str++)
		if (!isdigit((int)*str))
			return 0;
	return 1;
}

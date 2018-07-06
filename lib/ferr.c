/*
 * Copyright (c) 2015-16  David Lamparter, for NetDEF, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <inttypes.h>

#include "ferr.h"
#include "vty.h"
#include "jhash.h"
#include "memory.h"
#include "hash.h"
#include "command.h"
#include "json.h"
#include "linklist.h"

DEFINE_MTYPE_STATIC(LIB, ERRINFO, "error information")

/*
 * Thread-specific key for temporary storage of allocated ferr.
 */
static pthread_key_t errkey;

static void ferr_free(void *arg)
{
	XFREE(MTYPE_ERRINFO, arg);
}

static void err_key_init(void) __attribute__((_CONSTRUCTOR(500)));
static void err_key_init(void)
{
	pthread_key_create(&errkey, ferr_free);
}

static void err_key_fini(void) __attribute__((_DESTRUCTOR(500)));
static void err_key_fini(void)
{
	pthread_key_delete(errkey);
}

/*
 * Global shared hash table holding reference text for all defined errors.
 */
pthread_mutex_t refs_mtx = PTHREAD_MUTEX_INITIALIZER;
struct hash *refs;

static int ferr_hash_cmp(const void *a, const void *b)
{
	const struct ferr_ref *f_a = a;
	const struct ferr_ref *f_b = b;

	return f_a->code == f_b->code;
}

static inline unsigned int ferr_hash_key(void *a)
{
	struct ferr_ref *f = a;

	return f->code;
}

void ferr_ref_add(struct ferr_ref *ref)
{
	uint32_t i = 0;

	pthread_mutex_lock(&refs_mtx);
	{
		while (ref[i].code != END_FERR) {
			hash_get(refs, &ref[i], hash_alloc_intern);
			i++;
		}
	}
	pthread_mutex_unlock(&refs_mtx);
}

struct ferr_ref *ferr_ref_get(uint32_t code)
{
	struct ferr_ref holder;
	struct ferr_ref *ref;

	holder.code = code;
	pthread_mutex_lock(&refs_mtx);
	{
		ref = hash_lookup(refs, &holder);
	}
	pthread_mutex_unlock(&refs_mtx);

	return ref;
}

void ferr_ref_display(struct vty *vty, uint32_t code, bool json)
{
	struct ferr_ref *ref;
	struct json_object *top, *obj;
	struct list *errlist;
	struct listnode *ln;

	if (json)
		top = json_object_new_object();

	pthread_mutex_lock(&refs_mtx);
	{
		errlist = code ? list_new() : hash_to_list(refs);
	}
	pthread_mutex_unlock(&refs_mtx);

	if (code) {
		ref = ferr_ref_get(code);
		if (!ref) {
			vty_out(vty, "Code %"PRIu32" - Unknown\n", code);
			return;
		}
		listnode_add(errlist, ref);
	}

	for (ALL_LIST_ELEMENTS_RO(errlist, ln, ref)) {
		if (json) {
			char key[11];

			snprintf(key, sizeof(key), "%"PRIu32, ref->code);
			obj = json_object_new_object();
			json_object_string_add(obj, "title", ref->title);
			json_object_string_add(obj, "description",
					       ref->description);
			json_object_string_add(obj, "suggestion",
					       ref->suggestion);
			json_object_object_add(top, key, obj);
		} else {
			char pbuf[256];
			char ubuf[256];

			snprintf(pbuf, sizeof(pbuf), "\nError %"PRIu32" - %s",
				 code, ref->title);
			memset(ubuf, '=', strlen(pbuf));
			ubuf[sizeof(ubuf) - 1] = '\0';

			vty_out(vty, "%s\n%s\n", pbuf, ubuf);
			vty_out(vty, "Description:\n%s\n\n", ref->description);
			vty_out(vty, "Recommendation:\n%s\n", ref->suggestion);
		}
	}

	if (json) {
		const char *str = json_object_to_json_string_ext(
			top, JSON_C_TO_STRING_PRETTY);
		vty_out(vty, "%s\n", str);
		json_object_free(top);
	}

	list_delete_and_null(&errlist);
}

DEFUN_NOSH(show_error_code,
	   show_error_code_cmd,
	   "show error <(1-4294967296)|all> [json]",
	   SHOW_STR
	   "Information on errors\n"
	   "Error code to get info about\n"
	   "Information on all errors\n"
	   JSON_STR)
{
	bool json = strmatch(argv[argc-1]->text, "json");
	uint32_t arg = 0;

	if (!strmatch(argv[2]->text, "all"))
		arg = strtoul(argv[2]->arg, NULL, 10);

	ferr_ref_display(vty, arg, json);
	return CMD_SUCCESS;
}

void ferr_ref_init(void)
{
	pthread_mutex_lock(&refs_mtx);
	{
		refs = hash_create(ferr_hash_key, ferr_hash_cmp,
				   "Error Reference Texts");
	}
	pthread_mutex_unlock(&refs_mtx);

	install_element(VIEW_NODE, &show_error_code_cmd);
}

void ferr_ref_fini(void)
{
	pthread_mutex_lock(&refs_mtx);
	{
		hash_free(refs);
		refs = NULL;
	}
	pthread_mutex_unlock(&refs_mtx);
}

const struct ferr *ferr_get_last(ferr_r errval)
{
	struct ferr *last_error = pthread_getspecific(errkey);
	if (!last_error || last_error->kind == 0)
		return NULL;
	return last_error;
}

ferr_r ferr_clear(void)
{
	struct ferr *last_error = pthread_getspecific(errkey);
	if (last_error)
		last_error->kind = 0;
	return ferr_ok();
}

static ferr_r ferr_set_va(const char *file, int line, const char *func,
			  enum ferr_kind kind, const char *pathname,
			  int errno_val, const char *text, va_list va)
{
	struct ferr *error = pthread_getspecific(errkey);

	if (!error) {
		error = XCALLOC(MTYPE_ERRINFO, sizeof(*error));

		pthread_setspecific(errkey, error);
	}

	error->file = file;
	error->line = line;
	error->func = func;
	error->kind = kind;

	error->unique_id = jhash(text, strlen(text),
				 jhash(file, strlen(file), 0xd4ed0298));

	error->errno_val = errno_val;
	if (pathname)
		snprintf(error->pathname, sizeof(error->pathname), "%s",
			 pathname);
	else
		error->pathname[0] = '\0';

	vsnprintf(error->message, sizeof(error->message), text, va);
	return -1;
}

ferr_r ferr_set_internal(const char *file, int line, const char *func,
			 enum ferr_kind kind, const char *text, ...)
{
	ferr_r rv;
	va_list va;
	va_start(va, text);
	rv = ferr_set_va(file, line, func, kind, NULL, 0, text, va);
	va_end(va);
	return rv;
}

ferr_r ferr_set_internal_ext(const char *file, int line, const char *func,
			     enum ferr_kind kind, const char *pathname,
			     int errno_val, const char *text, ...)
{
	ferr_r rv;
	va_list va;
	va_start(va, text);
	rv = ferr_set_va(file, line, func, kind, pathname, errno_val, text, va);
	va_end(va);
	return rv;
}

#define REPLACE "$ERR"
void vty_print_error(struct vty *vty, ferr_r err, const char *msg, ...)
{
	char tmpmsg[512], *replacepos;
	const struct ferr *last_error = ferr_get_last(err);

	va_list va;
	va_start(va, msg);
	vsnprintf(tmpmsg, sizeof(tmpmsg), msg, va);
	va_end(va);

	replacepos = strstr(tmpmsg, REPLACE);
	if (!replacepos)
		vty_out(vty, "%s\n", tmpmsg);
	else {
		replacepos[0] = '\0';
		replacepos += sizeof(REPLACE) - 1;
		vty_out(vty, "%s%s%s\n", tmpmsg,
			last_error ? last_error->message : "(no error?)",
			replacepos);
	}
}

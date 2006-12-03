/* Copyright (C) 2005-2006 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "dict.h"
#include "quota-private.h"

#include <stdlib.h>

#define DICT_QUOTA_CURRENT_PATH DICT_PATH_PRIVATE"quota/"
#define DICT_QUOTA_CURRENT_BYTES_PATH DICT_QUOTA_CURRENT_PATH"storage"
#define DICT_QUOTA_CURRENT_COUNT_PATH DICT_QUOTA_CURRENT_PATH"messages"

struct dict_quota_root {
	struct quota_root root;
	struct dict *dict;
};

extern struct quota_backend quota_backend_dict;

static struct quota_root *dict_quota_alloc(void)
{
	struct dict_quota_root *root;

	root = i_new(struct dict_quota_root, 1);
	return &root->root;
}

static int dict_quota_init(struct quota_root *_root, const char *args)
{
	struct dict_quota_root *root = (struct dict_quota_root *)_root;
	const char *username, *p;

	p = args == NULL ? NULL : strchr(args, ':');
	if (p == NULL) {
		i_error("dict quota: URI missing from parameters");
		return -1;
	}

	username = t_strdup_until(args, p);
	args = p+1;

	if (*username == '\0')
		username = getenv("USER");

	if (getenv("DEBUG") != NULL)
		i_info("dict quota: user = %s, uri = %s", username, args);

	/* FIXME: we should use 64bit integer as datatype instead but before
	   it can actually be used don't bother */
	root->dict = dict_init(args, DICT_DATA_TYPE_STRING, username);
	return root->dict != NULL ? 0 : -1;
}

static void dict_quota_deinit(struct quota_root *_root)
{
	struct dict_quota_root *root = (struct dict_quota_root *)_root;

	if (root->dict != NULL)
		dict_deinit(&root->dict);
	i_free(root);
}

static const char *const *
dict_quota_root_get_resources(struct quota_root *root __attr_unused__)
{
	static const char *resources[] = {
		QUOTA_NAME_STORAGE_KILOBYTES, QUOTA_NAME_MESSAGES, NULL
	};

	return resources;
}

static int
dict_quota_count(struct dict_quota_root *root,
		 bool want_bytes, uint64_t *value_r)
{
	struct dict_transaction_context *dt;
	uint64_t bytes, count;

	if (quota_count(root->root.quota, &bytes, &count) < 0)
		return -1;

	t_push();
	dt = dict_transaction_begin(root->dict);
	dict_set(dt, DICT_QUOTA_CURRENT_BYTES_PATH, dec2str(bytes));
	dict_set(dt, DICT_QUOTA_CURRENT_COUNT_PATH, dec2str(count));
	t_pop();

	if (dict_transaction_commit(dt) < 0)
		i_error("dict_quota: Couldn't update quota");

	*value_r = want_bytes ? bytes : count;
	return 1;
}

static int
dict_quota_get_resource(struct quota_root *_root, const char *name,
			uint64_t *value_r, uint64_t *limit __attr_unused__)
{
	struct dict_quota_root *root = (struct dict_quota_root *)_root;
	const char *value;
	bool want_bytes;
	int ret;

	if (strcmp(name, QUOTA_NAME_STORAGE_BYTES) == 0)
		want_bytes = TRUE;
	else if (strcmp(name, QUOTA_NAME_MESSAGES) == 0)
		want_bytes = FALSE;
	else
		return 0;

	t_push();
	ret = dict_lookup(root->dict, unsafe_data_stack_pool,
			  want_bytes ? DICT_QUOTA_CURRENT_BYTES_PATH :
			  DICT_QUOTA_CURRENT_COUNT_PATH, &value);
	if (ret < 0)
		*value_r = 0;
	else if (ret == 0)
		ret = dict_quota_count(root, want_bytes, value_r);
	else
		*value_r = strtoull(value, NULL, 10);

	t_pop();
	return ret;
}

static int
dict_quota_update(struct quota_root *_root, 
		  struct quota_transaction_context *ctx)
{
	struct dict_quota_root *root = (struct dict_quota_root *) _root;
	struct dict_transaction_context *dt;

	dt = dict_transaction_begin(root->dict);
	if (ctx->bytes_used != 0) {
		dict_atomic_inc(dt, DICT_QUOTA_CURRENT_BYTES_PATH,
				ctx->bytes_used);
	}
	if (ctx->count_used != 0) {
		dict_atomic_inc(dt, DICT_QUOTA_CURRENT_COUNT_PATH,
				ctx->count_used);
	}
	
	if (dict_transaction_commit(dt) < 0)
		return -1;
	return 0;
}

struct quota_backend quota_backend_dict = {
	"dict",

	{
		dict_quota_alloc,
		dict_quota_init,
		dict_quota_deinit,
		NULL,
		dict_quota_root_get_resources,
		dict_quota_get_resource,
		dict_quota_update
	}
};

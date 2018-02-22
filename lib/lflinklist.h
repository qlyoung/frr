/* Lock-free linked list.
 * Copyright (C) 2018  Cumulus Networks
 * Quentin Young
 */
#include "frratomic.h"
struct lflistnode {
	_Atomic uintptr_t next;
	void *data;
}

struct lflist {
	struct lflistnode *head;
	struct lflistnode *tail;

	void (*del)(void *val);
	int (*cmp)(void *val1, void *val2);
}

bool lfl_insert(struct lflist *lfl, void *data);
bool lfl_find(struct lflist *lfl, void *data);
bool lfl_del(struct list *lfl, void *data);

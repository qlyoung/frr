/* Lock-free linked list.
 * Copyright (C) 2018  Cumulus Networks
 * Quentin Young
 *
 * Code adapted from Tim Harris's paper, "A Pragmatic Implementation of
 * Non-Blocking Linked Lists"
 */

#include "lflinklist.h"
#include "memory.h"

#define is_marked_reference(ref) (((uintptr_t)(ref)) & (uintptr_t)(0x1))
#define mark_reference(ref) (((uintptr_t)(ref)) &= (uintptr_t)(0x1))
#define unmark_reference(ref) (((uintptr_t)(ref)) &= (uintptr_t)(~0x1))
#define get_unmarked_reference(ref) (((uintptr_t)(ref)) & (uintptr_t)(~0x1))
#define get_marked_reference(ref) (((uintptr_t)(ref)) | (uintptr_t)(0x1))

static struct lflistnode *lfl_listnode_new(void *data)
{
	struct lflistnode *ln = XMALLOC(MTYPE_TMP, sizeof(struct lflistnode));
	assert(!is_marked_reference(ln));
	ln->data = data;
	return ln;
}

/*
 * Searches for left & right nodes such that:
 * - left->key < key <= right->key
 * - !(is_marked_reference(left) || is_marked_reference(right)) == true;
 * - left->next == right
 */
static lfl_search(struct lflist *lfl, void *data, struct lflistnode **left)
{
	struct lflistnode *lnn, *right, *t, *t_next;
	int cmp;

search_again:
	do {
		t = lfl->head;
		t_next = t->next;

		do { /* 1: Find left_node and right_node */
			if (!is_marked_reference(t_next)) {
				(*left) = t;
				lnn = t_next;
			}
			t = get_unmarked_reference(t_next);
			if (t == lfl->tail)
				break;
			t_next = t->next;
			cmp = lfl->cmp(t->data, data);
		} while (is_marked_reference(t_next) || cmp != 1);

		right = t;

		/* 2: Check nodes are adjacent */
		if (lnn == right)
			if ((right != lfl->tail)
			    && is_marked_reference(right->next))
				goto search_again;
			else
				return right;

		/* 3: Remove one or more marked nodes */
		if (atomic_exchange_explicit(&(*left->next), lnn, right)) {
			if ((right != lfl->tail)
			    && is_marked_reference(right->next))
				goto search_again;
			else
				return right;
		}
	} while (true);

	/* not reached */
	return NULL;
}

bool lfl_insert(struct lflist *lfl, void *data)
{
	struct lflistnode *left, *right, *new;
	new = lfl_listnode_new(data);

	do {
		right = search(data, &left);
		if ((right != lfl->tail) && (right->data == data))
			return false;
		new->next = right;
		if (atomic_exchange_explicit(&(left->next), right, new))
			return true;
	} while (true);

	/* not reached */
	return false;
}

bool lfl_find(struct lflist *lfl, void *data);
{
	struct lflistnode *left, *right;
	right = search(data, &left);
	if ((right == lfl->tail) || (right->data != data))
		return false;

	return true;
}


bool lfl_del(struct list *lfl, void *data);
{
	struct lflistnode *left, *right, *rnn;

	do {
		right = search(data, &left);
		if ((right == lfl->tail) || (right->data != data))
			return false;
		rnn = right->next;
		if (!is_marked_reference(rnn))
			if (atomic_exchange_explicit(&right->next, rnn,
						     get_marked_reference(rnn)))
				break;
	} while (true);

	return true;
}

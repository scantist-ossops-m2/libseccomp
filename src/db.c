/**
 * Enhanced Seccomp Filter DB
 *
 * Copyright (c) 2012 Red Hat <pmoore@redhat.com>
 * Author: Paul Moore <pmoore@redhat.com>
 */

/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <seccomp.h>

#include "db.h"

struct db_arg_filter {
	int valid;

	unsigned int arg;
	unsigned int op;
	unsigned long datum;
};

/**
 * Free each item in the DB list
 * @param iter the iterator
 * @param list the list
 *
 * This macro acts as for()/while() conditional and iterates the following
 * statement before freeing the list item.
 *
 */
#define _db_list_foreach_free(iter,list) \
	for (iter = (list); \
	     iter != NULL; \
	     (list) = iter->next, free(iter), iter = (list))

/**
 * Free a syscall filter argument chain tree
 * @param list the argument chain list
 *
 * This function frees a syscall argument chain list.
 *
 */
static void _db_arg_chain_tree_free(struct db_arg_chain_tree *tree)
{
	if (tree == NULL)
		return;

	_db_arg_chain_tree_free(tree->lvl_prv);
	_db_arg_chain_tree_free(tree->lvl_nxt);

	_db_arg_chain_tree_free(tree->nxt_t);
	_db_arg_chain_tree_free(tree->nxt_f);

	free(tree);
}

/**
 * Remove a node from an argument chain tree
 * @param tree the pointer to the tree
 * @param node the node to remove
 *
 * This function searches the tree looking for the node and removes it once
 * found.  The function also removes any other nodes that are no longer needed
 * as a result of removing the given node.
 *
 */
static void _db_arg_chain_tree_remove(struct db_arg_chain_tree **tree,
				      struct db_arg_chain_tree *node)
{
	struct db_arg_chain_tree *c_iter;

	if (tree == NULL || *tree == NULL || node == NULL)
		return;

	c_iter = *tree;
	while (c_iter->lvl_prv != NULL)
		c_iter = c_iter->lvl_prv;

	do {
		/* this is only an issue on the first level */
		if (c_iter == node) {
			/* remove from the tree */
			if (c_iter == *tree) {
				if (c_iter->lvl_prv != NULL)
					*tree = c_iter->lvl_prv;
				else
					*tree = c_iter->lvl_nxt;
			}
			if (c_iter->lvl_prv != NULL)
				c_iter->lvl_prv->lvl_nxt = c_iter->lvl_nxt;
			if (c_iter->lvl_nxt != NULL)
				c_iter->lvl_nxt->lvl_prv = c_iter->lvl_prv;

			/* free and return */
			c_iter->lvl_prv = NULL;
			c_iter->lvl_nxt = NULL;
			_db_arg_chain_tree_free(c_iter);
			return;
		}

		/* check the true sub-tree */
		if (c_iter->nxt_t == node) {
			/* free and return */
			_db_arg_chain_tree_free(c_iter->nxt_t);
			c_iter->nxt_t = NULL;
			return;
		} else
			_db_arg_chain_tree_remove(&(c_iter->nxt_t), node);

		/* check the false sub-tree */
		if (c_iter->nxt_f == node) {
			/* free and return */
			_db_arg_chain_tree_free(c_iter->nxt_f);
			c_iter->nxt_f = NULL;
			return;
		} else
			_db_arg_chain_tree_remove(&(c_iter->nxt_t), node);

		c_iter = c_iter->lvl_nxt;
	} while (c_iter != NULL);
}

/**
 * Intitalize a seccomp filter DB
 * @param def_action the default filter action
 *
 * This function initializes a seccomp filter DB and readies it for use.
 * Returns a pointer to the DB on success, NULL on failure.
 *
 */
struct db_filter *db_new(enum scmp_flt_action def_action)
{
	struct db_filter *db;

	db = malloc(sizeof(*db));
	if (db) {
		memset(db, 0, sizeof(*db));
		db->def_action = def_action;
	}

	return db;
}

/**
 * Destroy a seccomp filter DB
 * @param db the seccomp filter DB
 *
 * This function destroys a seccomp filter DB.  After calling this function,
 * the filter should no longer be referenced.
 *
 */
void db_destroy(struct db_filter *db)
{
	struct db_sys_list *s_iter;

	if (db == NULL)
		return;

	_db_list_foreach_free(s_iter, db->syscalls)
		_db_arg_chain_tree_free(s_iter->chains);
}

/**
 * Add a syscall filter with an optional argument chain
 * @param db the seccomp filter db
 * @param action the filter action
 * @param syscall the syscall number
 * @param chain_len the number of argument filters in the argument filter chain
 * @param chain_list argument filter chain, (uint, enum scmp_compare, ulong)
 *
 * This function adds a new syscall filter to the seccomp filter DB, adding to
 * the existing filters for the syscall, unless no argument specific filters
 * are present (filtering only on the syscall).  When adding new chains, the
 * shortest chain, or most inclusive filter match, will be entered into the
 * filter DB. Returns zero on success, negative values on failure.
 *
 */
int db_add_syscall(struct db_filter *db, enum scmp_flt_action action,
		   unsigned int syscall,
		   unsigned int chain_len, va_list chain_list)
{
	int rc = -ENOMEM;
	unsigned int iter;
	unsigned int arg_num;
	struct db_arg_filter chain[SCMP_ARG_MAX];
	struct db_sys_list *s_new, *s_iter, *s_prev = NULL;
	struct db_arg_chain_tree *c_iter = NULL, *c_prev = NULL;
	struct db_arg_chain_tree *ec_iter;
	unsigned int tf_flag;

	assert(db != NULL);

	if (chain_len > SCMP_ARG_MAX)
		return -EINVAL;

	/* we want to build a chain sorted by argument number to make it easier
	 * to find duplicate chains */
	memset(chain, 0, sizeof(chain));
	for (iter = 0; iter < chain_len; iter++) {
		arg_num = va_arg(chain_list, unsigned int);
		if (chain[arg_num].valid == 1)
			return -EINVAL;
		chain[arg_num].valid = 1;
		chain[arg_num].arg = arg_num;
		chain[arg_num].op = va_arg(chain_list, unsigned int);
		chain[arg_num].datum = va_arg(chain_list, unsigned long);
	}

	/* do all our possible memory allocation up front so we don't have to
	 * worry about failure once we get to the point where we start updating
	 * the filter db */
	s_new = malloc(sizeof(*s_new));
	if (s_new == NULL)
		goto add_free;
	memset(s_new, 0, sizeof(*s_new));
	s_new->num = syscall;
	/* run through the argument chain */
	for (iter = 0; iter < SCMP_ARG_MAX; iter++) {
		if (chain[iter].valid == 0)
			continue;

		c_iter = malloc(sizeof(*c_iter));
		if (c_iter == NULL)
			goto add_free;
		memset(c_iter, 0, sizeof(*c_iter));
		c_iter->arg = chain[iter].arg;
		c_iter->op = chain[iter].op;
		c_iter->datum = chain[iter].datum;
		c_iter->refcnt = 1;
		/* XXX - sanity check the c_iter->num value (SCMP_ARG_MAX) */
		/* XXX - sanity check the c_iter->datum value? */

		/* link in the new node and update the chain */
		if (c_prev != NULL) {
			c_prev->action = 0;
			if (tf_flag)
				c_prev->nxt_t = c_iter;
			else
				c_prev->nxt_f = c_iter;
		} else
			s_new->chains = c_iter;

		/* rewrite the op to reduce the op/datum combos */
		switch (c_iter->op) {
			case SCMP_CMP_NE:
				c_iter->op = SCMP_CMP_EQ;
				tf_flag = 0;
				break;
			case SCMP_CMP_LT:
				c_iter->op = SCMP_CMP_GE;
				tf_flag = 0;
				break;
			case SCMP_CMP_LE:
				c_iter->op = SCMP_CMP_GT;
				tf_flag = 0;
				break;
			default:
				tf_flag = 1;
		}

		c_prev = c_iter;
	}
	if (c_iter != NULL) {
		/* set the leaf node */
		c_iter->action = action;
		c_iter->action_flag = tf_flag;
	}

	/* no more failures allowed after this point that would result in the
	 * stored filter being in an inconsistent state */

	/* find a matching syscall/chain or insert a new one */
	s_iter = db->syscalls;
	while (s_iter != NULL && s_iter->num < syscall) {
		s_prev = s_iter;
		s_iter = s_iter->next;
	}
	if (s_iter == NULL || s_iter->num != syscall) {
		/* new syscall, add before s_iter */
		if (s_prev != NULL)
			s_prev->next = s_new;
		else
			db->syscalls = s_new;
		return 0;
	} else if (s_iter->chains == NULL) {
		/* syscall exists without any chains - existing filter is at
		 * least as large as the new entry so cleanup and exit */
		/* XXX - do we want to indicate that another, larger entry
		 *       already exists? */
		rc = 0;
		goto add_free;
	} else if (s_iter->chains != NULL && s_new->chains == NULL) {
		/* syscall exists with chains but the new filter has no chains
		 * so we need to clear the existing chains and exit */
		_db_arg_chain_tree_free(s_iter->chains);
		s_iter->chains = NULL;
		rc = 0;
		goto add_free;
	}
	/* syscall exists and has at least one existing chain - start at the
	 * top and walk the two chains */
	c_prev = NULL;
	c_iter = s_new->chains;
	ec_iter = s_iter->chains;
	do {
		if (db_chain_eq(c_iter, ec_iter)) {
			/* found a matching node on this chain level */
			ec_iter->refcnt++;
			if (db_chain_leaf(ec_iter) && db_chain_leaf(c_iter)) {
				if (ec_iter->action_flag != c_iter->action_flag)
					/* drop this node entirely as we take
					 * an action regardless of the op's
					 * result (true or false) */
					_db_arg_chain_tree_remove(
							&(s_iter->chains),
							ec_iter);
				rc = 0;
				goto add_free;
			} else if (db_chain_leaf(ec_iter)) {
				if (ec_iter->action_flag) {
					if (c_iter->nxt_t != NULL) {
						/* existing is shorter */
						rc = 0;
						goto add_free;
					}
					ec_iter->nxt_f = c_iter->nxt_f;
					goto add_free_match;
				} else {
					if (c_iter->nxt_f != NULL) {
						/* existing is shorter */
						rc = 0;
						goto add_free;
					}
					ec_iter->nxt_t = c_iter->nxt_t;
					goto add_free_match;
				}
			} else if (db_chain_leaf(c_iter)) {
				/* new is shorter */

				/* now at least a partial leaf node */
				ec_iter->action = action;
				ec_iter->action_flag = c_iter->action_flag;

				/* cleanup and return */
				if (ec_iter->action_flag) {
					_db_arg_chain_tree_free(ec_iter->nxt_t);
					ec_iter->nxt_t = NULL;
				} else {
					_db_arg_chain_tree_free(ec_iter->nxt_f);
					ec_iter->nxt_f = NULL;
				}
				return 0;
			} else if (c_iter->nxt_t != NULL) {
				/* moving down the chain */
				if (ec_iter->nxt_t == NULL) {
					/* add on to the existing */
					ec_iter->nxt_t = c_iter->nxt_t;
					goto add_free_match;
				} else {
					/* jump to the next level */
					c_prev = c_iter;
					c_iter = c_iter->nxt_t;
					ec_iter = ec_iter->nxt_t;
				}
			} else if (c_iter->nxt_f != NULL) {
				/* moving down the chain */
				if (ec_iter->nxt_f == NULL) {
					/* add on to the existing */
					ec_iter->nxt_f = c_iter->nxt_f;
					goto add_free_match;
				} else {
					/* jump to the next level */
					c_prev = c_iter;
					c_iter = c_iter->nxt_f;
					ec_iter = ec_iter->nxt_f;
				}
			} else {
				/* we should never be here! */
				rc = -EFAULT;
				goto add_free;
			}
		} else {
			/* need to check other nodes on this level */
			if (db_chain_lt(c_iter, ec_iter)) {
				if (ec_iter->lvl_prv == NULL) {
					ec_iter->lvl_prv = c_iter;
					goto add_free_match;
				} else
					ec_iter = ec_iter->lvl_prv;
			} else {
				if (ec_iter->lvl_nxt == NULL) {
					ec_iter->lvl_nxt = c_iter;
					goto add_free_match;
				} else
					ec_iter = ec_iter->lvl_nxt;
			}
		}
	} while ((c_iter != NULL) && (ec_iter != NULL));

	/* we should never be here! */
	return -EFAULT;

add_free:
	/* free the new chain and its syscall struct */
	_db_arg_chain_tree_free(s_new->chains);
	free(s_new);
	return rc;
add_free_match:
	/* free the matching portion of new chain */
	if (c_prev != NULL) {
		c_prev->nxt_t = NULL;
		c_prev->nxt_f = NULL;
		_db_arg_chain_tree_free(s_new->chains);
	}
	free(s_new);
	return 0;
}

/**
 * Find a syscall filter in the DB
 * @param db the seccomp filter DB
 * @param syscall the syscall number
 *
 * This function searches the filter DB using the given syscall number and
 * returns a pointer to the syscall filter or NULL if no matching syscall
 * filter exists.
 *
 */
struct db_sys_list *db_find_syscall(const struct db_filter *db,
				    unsigned int syscall)
{
	struct db_sys_list *iter;

	assert(db != NULL);

	iter = db->syscalls;
	while (iter != NULL && iter->num < syscall)
		iter = iter->next;
	if (iter != NULL && iter->num == syscall)
		return iter;

	return NULL;
}

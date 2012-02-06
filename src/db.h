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

#ifndef _FILTER_DB_H
#define _FILTER_DB_H

#include <stdarg.h>

#include <seccomp.h>

/* XXX - need to provide doxygen comments for the types here */

struct db_arg_chain_tree {
	/* argument number (a0 = 0, a1 = 1, etc.) */
	unsigned int arg;

	/* comparison operator */
	enum scmp_compare op;
	/* syscall argument value */
	/* XXX - need something big enough to hold any syscall argument */
	/* XXX - should this just be a void ptr? */
	/* XXX - should we make it a macro or typedef defined at build time? */
	unsigned long datum;

	/* if non-zero, this is the last node and the value is desired action */
	enum scmp_flt_action action;
	unsigned int action_flag;

	/* list of nodes on this level */
	struct db_arg_chain_tree *lvl_prv, *lvl_nxt;

	/* next node in the chain */
	struct db_arg_chain_tree *nxt_t;
	struct db_arg_chain_tree *nxt_f;

	/* number of chains referencing this node */
	unsigned int refcnt;
};
#define db_chain_lt(x,y) \
	(((x)->arg < (y)->arg) || \
	 (((x)->arg == (y)->arg) && ((x)->op < (y)->op)))
#define db_chain_eq(x,y) \
	(((x)->arg == (y)->arg) && \
	 ((x)->op == (y)->op) && ((x)->datum == (y)->datum))
#define db_chain_leaf(x) \
	((x)->action != 0)

struct db_sys_list {
	/* native syscall number */
	unsigned int num;
	/* the argument chain heads */
	struct db_arg_chain_tree *chains;

	struct db_sys_list *next;
};

struct db_filter {
	/* action to take if we don't match an explicit allow/deny */
	enum scmp_flt_action def_action;

	/* syscall filters, kept as a sorted single-linked list */
	struct db_sys_list *syscalls;
};

/**
 * Iterate over each item in the DB list
 * @param iter the iterator
 * @param list the list
 *
 * This macro acts as for()/while() conditional and iterates the following
 * statement for each item in the given list.
 *
 */
#define db_list_foreach(iter,list) \
	for (iter = (list); iter != NULL; iter = iter->next)

struct db_filter *db_new(enum scmp_flt_action def_action);
void db_destroy(struct db_filter *db);

int db_add_syscall(struct db_filter *db, enum scmp_flt_action action,
		   unsigned int syscall,
		   unsigned int chain_len, va_list chain_list);

struct db_sys_list *db_find_syscall(const struct db_filter *db,
				    unsigned int syscall);

#endif

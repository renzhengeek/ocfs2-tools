/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * Copyright (C) 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License, version 2,  as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * --
 *
 * A trivial rbtree that stores a u16 icount indexed by an inode's block
 * number.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include "ocfs2.h"

#include "fsck.h"
#include "icount.h"
#include "util.h"

typedef struct _icount_node {
	struct rb_node	in_node;
	uint64_t	in_blkno;
	uint16_t	in_icount;
} icount_node;

/* XXX this is currently fragile in that it requires that the caller make
 * sure that the node doesn't already exist in the tree.
 */
static void icount_insert(o2fsck_icount *icount, icount_node *in)
{
	struct rb_node ** p = &icount->ic_multiple_tree.rb_node;
	struct rb_node * parent = NULL;
	icount_node *tmp_in;

	while (*p)
	{
		parent = *p;
		tmp_in = rb_entry(parent, icount_node, in_node);

		if (in->in_blkno < tmp_in->in_blkno)
			p = &(*p)->rb_left;
		else if (in->in_blkno > tmp_in->in_blkno)
			p = &(*p)->rb_right;
	}

	rb_link_node(&in->in_node, parent, p);
	rb_insert_color(&in->in_node, &icount->ic_multiple_tree);
}

static icount_node *icount_search(o2fsck_icount *icount, uint64_t blkno)
{
	struct rb_node *node = icount->ic_multiple_tree.rb_node;
	icount_node *in;

	while (node) {
		in = rb_entry(node, icount_node, in_node);

		if (blkno < in->in_blkno)
			node = node->rb_left;
		else if (blkno > in->in_blkno)
			node = node->rb_right;
		else
			return in;
	}
	return NULL;
}

/* keep it simple for now by always updating both data structures */
void o2fsck_icount_set(o2fsck_icount *icount, uint64_t blkno, 
			uint16_t count)
{
	icount_node *in;

	if (count == 1)
		ocfs2_bitmap_set(icount->ic_single_bm, blkno, NULL);
	else
		ocfs2_bitmap_clear(icount->ic_single_bm, blkno, NULL);

	in = icount_search(icount, blkno);
	if (in) {
		if (count < 2) {
			rb_erase(&in->in_node, &icount->ic_multiple_tree);
			free(in);
		} else {
			in->in_icount = count;
		}
	} else if (count > 1) {
		in = calloc(1, sizeof(*in));
		if (in == NULL)
			fatal_error(OCFS2_ET_NO_MEMORY, 
				    "while allocating to track icount");

		in->in_blkno = blkno;
		in->in_icount = count;
		icount_insert(icount, in);
	}
}

uint16_t o2fsck_icount_get(o2fsck_icount *icount, uint64_t blkno)
{
	icount_node *in;
	int was_set;
	uint16_t ret = 0;

	ocfs2_bitmap_test(icount->ic_single_bm, blkno, &was_set);
	if (was_set) {
		ret = 1;
		goto out;
	}

	in = icount_search(icount, blkno);
	if (in)
		ret = in->in_icount;

out:
	return ret;
}

/* again, simple before efficient.  We just find the old value and
 * use _set to make sure that the new value updates both the bitmap
 * and the tree */
void o2fsck_icount_delta(o2fsck_icount *icount, uint64_t blkno, 
			 int delta)
{
	int was_set;
	uint16_t prev_count;
	icount_node *in;

	if (delta == 0)
		return;

	ocfs2_bitmap_test(icount->ic_single_bm, blkno, &was_set);
	if (was_set) {
		prev_count = 1;
	} else {
		in = icount_search(icount, blkno);
		if (in == NULL)
			prev_count = 0;
		else
			prev_count = in->in_icount;
	}

	if (prev_count + delta < 0) 
		fatal_error(OCFS2_ET_INTERNAL_FAILURE, "while droping icount "
			    "from %"PRIu16" bt %d for inode %"PRIu64, 
			    prev_count, delta, blkno);

	o2fsck_icount_set(icount, blkno, prev_count + delta);
}

errcode_t o2fsck_icount_new(ocfs2_filesys *fs, o2fsck_icount **ret)
{
	o2fsck_icount *icount;
	errcode_t err;

	icount = calloc(1, sizeof(*icount));
	if (icount == NULL)
		return OCFS2_ET_NO_MEMORY;

	err = ocfs2_block_bitmap_new(fs, "inodes with single link_count",
				     &icount->ic_single_bm);
	if (err) {
		free(icount);
		com_err("icount", err, "while allocating single link_count bm");
		return err;
	}

	icount->ic_multiple_tree = RB_ROOT;

	*ret = icount;
	return 0;
}

void o2fsck_icount_free(o2fsck_icount *icount)
{
	struct rb_node *node;
	icount_node *in; 

	ocfs2_bitmap_free(icount->ic_single_bm);
	while((node = rb_first(&icount->ic_multiple_tree)) != NULL) {
		in = rb_entry(node, icount_node, in_node);
		rb_erase(node, &icount->ic_multiple_tree);
		free(in);
	}
	free(icount);
}
/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * extent_map.c
 *
 * In-memory extent map for the OCFS2 userspace library.
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
 * Authors: Joel Becker
 */

#define _XOPEN_SOURCE 600 /* Triggers magic in features.h */
#define _LARGEFILE64_SOURCE

#include <string.h>
#include <inttypes.h>

#include "ocfs2.h"

#include "extent_map.h"

struct extent_map_context {
	ocfs2_cached_inode *cinode;
	errcode_t errcode;
};

/*
 * Find an entry in the tree that intersects the region passed in.
 * Note that this will find straddled intervals, it is up to the
 * callers to enforce any boundary conditions.
 *
 * The rb_node garbage lets insertion share the search.  Trivial
 * callers pass NULL.
 */
static ocfs2_extent_map_entry *
ocfs2_extent_map_lookup(ocfs2_extent_map *em,
			uint32_t cpos, uint32_t clusters,
			struct rb_node ***ret_p,
			struct rb_node **ret_parent)
{
	struct rb_node **p = &em->em_extents.rb_node;
	struct rb_node *parent = NULL;
	ocfs2_extent_map_entry *ent = NULL;

	while (*p)
	{
		parent = *p;
		ent = rb_entry(parent, ocfs2_extent_map_entry, e_node);
		if ((cpos + clusters) <= ent->e_rec.e_cpos) {
			p = &(*p)->rb_left;
			ent = NULL;
		} else if (cpos >= (ent->e_rec.e_cpos +
				    ent->e_rec.e_clusters)) {
			p = &(*p)->rb_right;
			ent = NULL;
		} else
			break;
	}

	if (ret_p != NULL)
		*ret_p = p;
	if (ret_parent != NULL)
		*ret_parent = parent;
	return ent;
}

static errcode_t ocfs2_extent_map_insert_entry(ocfs2_extent_map *em,
					       ocfs2_extent_map_entry *ent)
{
	struct rb_node **p, *parent;
	ocfs2_extent_map_entry *old_ent;
	
	old_ent = ocfs2_extent_map_lookup(em, ent->e_rec.e_cpos,
					  ent->e_rec.e_clusters,
					  &p, &parent);
	if (old_ent)
		return OCFS2_ET_INVALID_EXTENT_LOOKUP;

	rb_link_node(&ent->e_node, parent, p);
	rb_insert_color(&ent->e_node, &em->em_extents);

	return 0;
}

errcode_t ocfs2_extent_map_insert(ocfs2_extent_map *em,
				  ocfs2_extent_rec *rec,
				  int tree_depth)
{
	errcode_t ret;
	ocfs2_extent_map_entry *old_ent, *new_ent;
	ocfs2_extent_map_entry *left_ent = NULL, *right_ent = NULL;

	/* FIXME: should we cache i_clusters instead of cinode? */
	if ((rec->e_cpos + rec->e_clusters) >
	    em->em_cinode->ci_inode->i_clusters)
		return OCFS2_ET_INVALID_EXTENT_LOOKUP;

	ret = ocfs2_malloc0(sizeof(struct _ocfs2_extent_map_entry),
			    &new_ent);
	if (ret)
		return ret;

	new_ent->e_rec = *rec;
	new_ent->e_tree_depth = tree_depth;
	ret = ocfs2_extent_map_insert_entry(em, new_ent);
	if (!ret)
		return 0;

	ret = OCFS2_ET_INTERNAL_FAILURE;
	old_ent = ocfs2_extent_map_lookup(em, rec->e_cpos,
					  rec->e_clusters, NULL, NULL);

	if (!old_ent)
		goto out_free;

	ret = OCFS2_ET_INVALID_EXTENT_LOOKUP;
	if (old_ent->e_tree_depth < tree_depth)
		goto out_free;
	if (old_ent->e_tree_depth == tree_depth) {
		if (!memcmp(rec, &old_ent->e_rec,
			    sizeof(ocfs2_extent_rec)))
			ret = 0;  /* Same entry, just skip */
		goto out_free;
	}

	/*
	 * We do it in this order specifically so that malloc failures
	 * do not leave an inconsistent tree.
	 */
	if (rec->e_cpos > old_ent->e_rec.e_cpos) {
		ret = ocfs2_malloc0(sizeof(struct _ocfs2_extent_map_entry),
				    &left_ent);
		if (ret)
			goto out_free;
		*left_ent = *old_ent;
		left_ent->e_rec.e_clusters =
			rec->e_cpos - left_ent->e_rec.e_cpos;
	}
	if ((old_ent->e_rec.e_cpos +
	     old_ent->e_rec.e_clusters) > 
	    (rec->e_cpos + rec->e_clusters)) {
		ret = ocfs2_malloc0(sizeof(struct _ocfs2_extent_map_entry),
				    &right_ent);
		if (ret)
			goto out_free;
		*right_ent = *old_ent;
		right_ent->e_rec.e_cpos =
			rec->e_cpos + rec->e_clusters;
		right_ent->e_rec.e_clusters =
			(old_ent->e_rec.e_cpos +
			 old_ent->e_rec.e_clusters) -
			right_ent->e_rec.e_cpos;
	}

	rb_erase(&old_ent->e_node, &em->em_extents);

	if (left_ent) {
		ret = ocfs2_extent_map_insert_entry(em,
						    left_ent);
		if (ret)
			goto out_free;
	}

	ret = ocfs2_extent_map_insert_entry(em, new_ent);
	if (ret)
		goto out_free;

	if (right_ent) {
		ret = ocfs2_extent_map_insert_entry(em,
						    right_ent);
		if (ret)
			goto out_free;
	}

	ocfs2_free(&old_ent);

	return 0;

out_free:
	if (left_ent)
		ocfs2_free(&left_ent);
	if (right_ent)
		ocfs2_free(&right_ent);
	ocfs2_free(&new_ent);

	return ret;
}


/*
 * Look up the record containing this cluster offset.  This record is
 * part of the extent map.  Do not free it.  Any changes you make to
 * it will reflect in the extent map.  So, if your last extent
 * is (cpos = 10, clusters = 10) and you truncate the file by 5
 * clusters, you want to do:
 *
 * ret = ocfs2_extent_map_get_rec(em, orig_size - 5, &rec);
 * rec->e_clusters -= 5;
 */
errcode_t ocfs2_extent_map_get_rec(ocfs2_extent_map *em,
				   uint32_t cpos,
				   ocfs2_extent_rec **rec)
{
	ocfs2_extent_map_entry *ent;

	*rec = NULL;

	ent = ocfs2_extent_map_lookup(em, cpos, 1, NULL, NULL);
	
	if (ent)
		*rec = &ent->e_rec;

	return 0;
}

errcode_t ocfs2_extent_map_get_clusters(ocfs2_extent_map *em,
					uint32_t v_cpos, int count,
					uint32_t *p_cpos,
					int *ret_count)
{
	uint32_t coff, ccount;
	ocfs2_extent_map_entry *ent;
	ocfs2_filesys *fs = em->em_cinode->ci_fs;

	*p_cpos = ccount = 0;

	if ((v_cpos + count) >= em->em_cinode->ci_inode->i_clusters)
		return OCFS2_ET_INVALID_EXTENT_LOOKUP;

	ent = ocfs2_extent_map_lookup(em, v_cpos, count, NULL, NULL);
	if (ent) {
		/* We should never find ourselves straddling an interval */
		if ((ent->e_rec.e_cpos > v_cpos) ||
		    ((v_cpos + count) >
		     (ent->e_rec.e_cpos + ent->e_rec.e_clusters)))
			return OCFS2_ET_INVALID_EXTENT_LOOKUP;

		coff = v_cpos - ent->e_rec.e_cpos;
		*p_cpos = ocfs2_blocks_to_clusters(fs,
						   ent->e_rec.e_blkno) +
			coff;

		if (ret_count)
			*ret_count = ent->e_rec.e_clusters - coff;

		return 0;
	}


	return OCFS2_ET_EXTENT_NOT_FOUND;
}

errcode_t ocfs2_extent_map_get_blocks(ocfs2_extent_map *em,
				      uint64_t v_blkno, int count,
				      uint64_t *p_blkno, int *ret_count)
{
	uint64_t boff;
	uint32_t cpos, clusters;
	ocfs2_filesys *fs = em->em_cinode->ci_fs;
	int bpc = ocfs2_clusters_to_blocks(fs, 1);
	ocfs2_extent_map_entry *ent;
	ocfs2_extent_rec *rec;

	*p_blkno = 0;

	cpos = ocfs2_blocks_to_clusters(fs, v_blkno);
	clusters = ocfs2_blocks_to_clusters(fs,
					    (uint64_t)count + bpc - 1);
	if ((cpos + clusters) >= em->em_cinode->ci_inode->i_clusters)
		return OCFS2_ET_INVALID_EXTENT_LOOKUP;

	ent = ocfs2_extent_map_lookup(em, cpos, clusters, NULL, NULL);
	if (ent)
	{
		rec = &ent->e_rec;

		/* We should never find ourselves straddling an interval */
		if ((rec->e_cpos > cpos) ||
		    ((cpos + clusters) >
		     (rec->e_cpos + rec->e_clusters)))
			return OCFS2_ET_INVALID_EXTENT_LOOKUP;

		boff = ocfs2_clusters_to_blocks(fs, cpos - rec->e_cpos);
		boff += (v_blkno % bpc);
		*p_blkno = rec->e_blkno + boff;

		if (ret_count) {
			*ret_count = ocfs2_clusters_to_blocks(fs,
							      rec->e_clusters) - boff;
		}

		return 0;
	}

	return OCFS2_ET_EXTENT_NOT_FOUND;
}

errcode_t ocfs2_extent_map_new(ocfs2_filesys *fs,
			       ocfs2_cached_inode *cinode,
			       ocfs2_extent_map **ret_em)
{
	errcode_t ret;
	ocfs2_extent_map *em;

	ret = ocfs2_malloc0(sizeof(struct _ocfs2_extent_map), &em);
	if (ret)
		return ret;

	em->em_cinode = cinode;
	em->em_extents = RB_ROOT;

	*ret_em = em;

	return 0;
}

/*
 * Truncate all entries past new_clusters.
 * If you want to also clip the last extent by some number of clusters,
 * you need to call ocfs2_extent_map_get_rec() and modify the rec
 * you are returned.
 */
errcode_t ocfs2_extent_map_trunc(ocfs2_extent_map *em,
				 uint32_t new_clusters)
{
	errcode_t ret = 0;
	struct rb_node *node, *next;
	ocfs2_extent_map_entry *ent;

	node = rb_last(&em->em_extents);
	while (node)
	{
		next = rb_prev(node);

		ent = rb_entry(node, ocfs2_extent_map_entry, e_node);
		if (ent->e_rec.e_cpos < new_clusters)
			break;

		rb_erase(&ent->e_node, &em->em_extents);
		ocfs2_free(&ent);

		node = next;
	}

	return ret;
}

void ocfs2_extent_map_free(ocfs2_extent_map *em)
{
	ocfs2_extent_map_trunc(em, 0);
	ocfs2_free(&em);
}


static int extent_map_func(ocfs2_filesys *fs,
			   ocfs2_extent_rec *rec,
		  	   int tree_depth,
			   uint32_t ccount,
			   uint64_t ref_blkno,
			   int ref_recno,
			   void *priv_data)
{
	errcode_t ret;
	int iret = 0;
	struct extent_map_context *ctxt = priv_data;

	ret = ocfs2_extent_map_insert(ctxt->cinode->ci_map, rec,
				      tree_depth);
	if (ret) {
		ctxt->errcode = ret;
		iret |= OCFS2_EXTENT_ABORT;
	}

	return iret;
}

errcode_t ocfs2_load_extent_map(ocfs2_filesys *fs,
				ocfs2_cached_inode *cinode)
{
	errcode_t ret;
	struct extent_map_context ctxt;

	if (!cinode)
		return OCFS2_ET_INVALID_ARGUMENT;

	ret = ocfs2_extent_map_new(fs, cinode, &cinode->ci_map);
	if (ret)
		return ret;

	ctxt.cinode = cinode;
	ctxt.errcode = 0;

	ret = ocfs2_extent_iterate(fs, cinode->ci_blkno, 0, NULL,
				   extent_map_func, &ctxt);
	if (ret)
		goto cleanup;

	if (ctxt.errcode) {
		ret = ctxt.errcode;
		goto cleanup;
	}

	return 0;

cleanup:
	ocfs2_drop_extent_map(fs, cinode);

	return ret;
}

errcode_t ocfs2_drop_extent_map(ocfs2_filesys *fs,
				ocfs2_cached_inode *cinode)
{
	ocfs2_extent_map *em;

	if (!cinode || !cinode->ci_map)
		return OCFS2_ET_INVALID_ARGUMENT;

	em = cinode->ci_map;
	ocfs2_extent_map_free(em);
	cinode->ci_map = NULL;

	return 0;
}

#ifdef DEBUG_EXE
#include <stdlib.h>
#include <getopt.h>

static uint64_t read_number(const char *num)
{
	uint64_t val;
	char *ptr;

	val = strtoull(num, &ptr, 0);
	if (!ptr || *ptr)
		return 0;

	return val;
}

static void print_usage(void)
{
	fprintf(stderr,
		"Usage: extent_map -i <inode_blkno> <filename>\n");
}

static int walk_extents_func(ocfs2_filesys *fs,
			     ocfs2_cached_inode *cinode)
{
	ocfs2_extent_map *em;
	struct rb_node *node;
	uint32_t ccount;
	ocfs2_extent_map_entry *ent;

	em = cinode->ci_map;

	fprintf(stdout, "EXTENTS:\n");

	ccount = 0;

	for (node = rb_first(&em->em_extents); node; node = rb_next(node)) {
		ent = rb_entry(node, ocfs2_extent_map_entry, e_node);

		fprintf(stdout, "(%08"PRIu32", %08"PRIu32", %08"PRIu64") |"
				" + %08"PRIu32" = %08"PRIu32" / %08"PRIu32"\n",
			ent->e_rec.e_cpos, ent->e_rec.e_clusters,
			ent->e_rec.e_blkno, ccount,
			ccount + ent->e_rec.e_clusters,
			cinode->ci_inode->i_clusters);

		ccount += ent->e_rec.e_clusters;
	}

	fprintf(stdout, "TOTAL: %"PRIu32"\n", cinode->ci_inode->i_clusters);

	return 0;
}


extern int opterr, optind;
extern char *optarg;

int main(int argc, char *argv[])
{
	errcode_t ret;
	uint64_t blkno;
	int c;
	char *filename;
	ocfs2_filesys *fs;
	ocfs2_cached_inode *cinode;

	blkno = OCFS2_SUPER_BLOCK_BLKNO;

	initialize_ocfs_error_table();

	while ((c = getopt(argc, argv, "i:")) != EOF) {
		switch (c) {
			case 'i':
				blkno = read_number(optarg);
				if (blkno <= OCFS2_SUPER_BLOCK_BLKNO) {
					fprintf(stderr,
						"Invalid inode block: %s\n",
						optarg);
					print_usage();
					return 1;
				}
				break;

			default:
				print_usage();
				return 1;
				break;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Missing filename\n");
		print_usage();
		return 1;
	}
	filename = argv[optind];
	
	ret = ocfs2_open(filename, OCFS2_FLAG_RO, 0, 0, &fs);
	if (ret) {
		com_err(argv[0], ret,
			"while opening file \"%s\"", filename);
		goto out;
	}

	ret = ocfs2_read_cached_inode(fs, blkno, &cinode);
	if (ret) {
		com_err(argv[0], ret, "while reading inode %"PRIu64, blkno);
		goto out_close;
	}

	fprintf(stdout, "OCFS2 inode %"PRIu64" on \"%s\" has depth %"PRId16"\n",
		blkno, filename,
		cinode->ci_inode->id2.i_list.l_tree_depth);

	ret = ocfs2_load_extent_map(fs, cinode);
	if (ret) {
		com_err(argv[0], ret,
			"while loading extents");
		goto out_free;
	}

	walk_extents_func(fs, cinode);

out_free:
	ocfs2_free_cached_inode(fs, cinode);

out_close:
	ret = ocfs2_close(fs);
	if (ret) {
		com_err(argv[0], ret,
			"while closing file \"%s\"", filename);
	}

out:
	return 0;
}
#endif  /* DEBUG_EXE */



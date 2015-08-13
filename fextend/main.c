/*
 * main.c
 *
 * entry point for fswrk
 *
 * Copyright (C) 2006 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
 */

#include "main.h"

const char *progname = NULL;
const char *device = NULL;
int height = 0;

void create_file(ocfs2_filesys *fs, uint64_t blkno, uint64_t *retblkno);
void create_named_directory(ocfs2_filesys *fs, char *dirname, uint64_t *blkno);
void custom_extend_allocation(ocfs2_filesys *fs, uint64_t ino,
				     uint32_t new_clusters);

static void usage(void)
{
	g_print("Usage: %s <-l tree height> <devicename>\n", progname);
	exit(0);
}

static int read_options(int argc, char **argv)
{
	int c;

	progname = basename(argv[0]);

	if (argc < 3)
		usage();

	while (1) {
		c = getopt(argc, argv, "hl:");
		if (c == -1)
			break;

		switch (c) {
		case 'l':
			height =  atoi(optarg);
			if (height < 0 || height > 6) {
				fprintf(stderr, "Bad tree height,should be >0 and <=6.\n");
				exit(-1);
			}
			break;
		default:
			usage();
		}
	}

	if(optind >= argc || !argv[optind]) {
		usage();
		exit(-1);
	}
	device = argv[optind];

	return 0;
}

static void handle_signal(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		exit(1);
	}
}

void create_named_directory(ocfs2_filesys *fs, char *dirname,
                                   uint64_t *blkno)
{
        errcode_t ret;
        struct ocfs2_super_block *sb = OCFS2_RAW_SB(fs->fs_super);

        ret = ocfs2_lookup(fs, sb->s_root_blkno, dirname, strlen(dirname), NULL,
                           blkno);
        if (!ret)
                return;
        else if (ret != OCFS2_ET_FILE_NOT_FOUND)
                FEXTD_COM_FATAL(progname, ret);

        ret  = ocfs2_new_inode(fs, blkno, S_IFDIR | 0755);
        if (ret)
                FEXTD_COM_FATAL(progname, ret);

        ret = ocfs2_init_dir(fs, *blkno, fs->fs_root_blkno);
        if (ret)
                FEXTD_COM_FATAL(progname, ret);

        ret = ocfs2_link(fs, fs->fs_root_blkno, dirname, *blkno, OCFS2_FT_DIR);
        if (ret)
                FEXTD_COM_FATAL(progname, ret);

        return;
}

void create_file(ocfs2_filesys *fs, uint64_t blkno, uint64_t *retblkno)
{
	errcode_t ret;
	uint64_t tmp_blkno = 0;
	char random_name[OCFS2_MAX_FILENAME_LEN];

	memset(random_name, 0, sizeof(random_name));
	sprintf(random_name, "testXXXXXX");

	/* Don't use mkstemp since it will create a file
	 * in the working directory which is no use.
	 * Use mktemp instead Although there is a compiling warning.
	 * mktemp fails to work in some implementations follow BSD 4.3,
	 * but currently ocfs2 will only support linux,
	 * so it will not affect us.
	 */
	if (!mktemp(random_name))
		FEXTD_COM_FATAL(progname, errno);

	ret = ocfs2_check_directory(fs, blkno);
	if (ret)
		FEXTD_COM_FATAL(progname, ret);

	ret = ocfs2_new_inode(fs, &tmp_blkno, S_IFREG | 0755);
	if (ret)
		FEXTD_COM_FATAL(progname, ret);

	ret = ocfs2_link(fs, blkno, random_name, tmp_blkno, OCFS2_FT_REG_FILE);
	if (ret)
		FEXTD_COM_FATAL(progname, ret);

	*retblkno = tmp_blkno;

	return;
}

/*
 * This function is similar to ocfs2_extend_allocation() as both extend files.
 * However, this one ensures that the extent record tree also grows.
 */
void custom_extend_allocation(ocfs2_filesys *fs, uint64_t ino,
				     uint32_t new_clusters)
{
	errcode_t ret;
	uint32_t n_clusters, total_clusters = new_clusters;
	uint32_t i, offset = 0;
	uint64_t blkno;
	uint64_t tmpblk;

	if (!(fs->fs_flags & OCFS2_FLAG_RW))
		FEXTD_FATAL("read-only filesystem");

	while (new_clusters) {
		//g_print("Are we hanging here?\n");
		ret = ocfs2_new_clusters(fs, 1, new_clusters>100 ? 100 : new_clusters, &blkno,
					 &n_clusters);
		if (ret)
			FEXTD_COM_FATAL(progname, ret);
		if (!n_clusters)
			FEXTD_FATAL("ENOSPC");

		new_clusters -= n_clusters;
		g_print("Remaining new_clusters/total_clusters: %d/%d\n", new_clusters, total_clusters);

		/* In order to ensure the extent records are not coalesced,
		 * we insert each cluster in reverse. */
		for(i = n_clusters; i; --i) {
			tmpblk = blkno + ocfs2_clusters_to_blocks(fs, i - 1);
			ret = ocfs2_inode_insert_extent(fs, ino, offset++,
							tmpblk, 1, 0);
			if (ret)
				FEXTD_COM_FATAL(progname, ret);
		}

		g_print("%d extents inserted.\n", n_clusters);
	}

	return;
}

int main(int argc, char **argv)
{
	ocfs2_filesys *fs = NULL;
	errcode_t ret = 0;
	int recs_per_eb, clusters;
	uint64_t blkno, tmpblkno;

	initialize_ocfs_error_table();

#define INSTALL_SIGNAL(sig)					\
	do {							\
		if (signal(sig, handle_signal) == SIG_ERR) {	\
		    fprintf(stderr, "Could not set " #sig "\n");\
		    goto bail;					\
		}						\
	} while (0)

	INSTALL_SIGNAL(SIGTERM);
	INSTALL_SIGNAL(SIGINT);

	ret = read_options(argc, argv);
	if (ret < 0)
		goto bail;

	ret = ocfs2_open(device, OCFS2_FLAG_RW, 0, 0, &fs);
	if (ret) {
		com_err(progname, ret, "while opening \"%s\"", device);
		goto bail;
	}
	g_print("Device(%s) opened.\n", device);

/* cal number of extent needed to create *height* tree */
	/*
	 * n >= t^h
	 * n: extents, t: slots in eb, h: tree height
	 *
	 * Have not minmum degree?
	 */
	recs_per_eb = ocfs2_extent_recs_per_eb(fs->fs_blocksize);
	/*
	 * 1 cluster/extent to fast expand file tree
	 */
	clusters = (int)pow(recs_per_eb, height) + 1;
	g_print("Figure out:\n");
	g_print("\tExtent records per extent block: %d\n", recs_per_eb);
	g_print("\tClusters(%d^%d + 1): %d\n", recs_per_eb, height, clusters);
/* create file */
	create_named_directory(fs, "extent-block", &blkno);
	create_file(fs, blkno, &tmpblkno);
	g_print("Directory extent-block inode#%ld, tmp file inode#%ld created\n",
		blkno, tmpblkno);

/* extend file */
	g_print("Have a good rest! It may take long time.\n");
	g_print("Extending file...\n");
	custom_extend_allocation(fs, tmpblkno, clusters);
	g_print("Done!");

bail:
	if (fs) {
		ret = ocfs2_close(fs);
		if (ret)
			com_err(progname, ret, "while closing \"%s\"", device);
	}

	return ret;
}

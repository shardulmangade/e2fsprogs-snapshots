/*
 * e4send.c --- Program which writes an image file backing up
 * all the _used_ blocks of the snapshot. (Full Backup)
 * 
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"
#include "et/com_err.h"
#include "uuid/uuid.h"
#include "e2p/e2p.h"
#include "ext2fs/e2image.h"
#include "../version.h"
#include "nls-enable.h"

#define MAX 50
#define MNT "/mnt/source"
#define BLOCK_GROUP_OFFSET EXT2_BLOCKS_PER_GROUP(fs->super)*fs->blocksize
const char * program_name = "e4send";

static void usage(void)
{
	fprintf(stderr, _("Usage: %s <device>@<snapshot_name> <image_file>\n"),
		program_name);
	exit (1);
}


/* This function returns 1 if the specified block is all zeros */
static int check_zero_block(char *buf, int blocksize)
{
	char	*cp = buf;
	int	left = blocksize;

	while (left > 0) {
		if (*cp++)
			return 0;
		left--;
	}
	return 1;
}

static void write_block(int fd, char *buf, int sparse_offset,
			int blocksize, blk_t block)
{
	int		count;
	errcode_t	err;

	if (sparse_offset) {
#ifdef HAVE_LSEEK64
		if (lseek64(fd, sparse_offset, SEEK_CUR) < 0)
			perror("lseek");
#else
		if (lseek(fd, sparse_offset, SEEK_CUR) < 0)
			perror("lseek");
#endif
	}
	if (blocksize) {
		count = write(fd, buf, blocksize);
		if (count != blocksize) {
			if (count == -1)
				err = errno;
			else
				err = 0;
			com_err(program_name, err, "error writing block %u",
				block);
			exit(1);
		}
	}
}
/* Create the full backup image file
   fs corresponds to the source snapshot file
   fd is the file descriptor for destination file
 */
static void write_full_image(ext2_filsys fs, int fd)
{
	blk_t	blk;
	int	group = 0;
	blk_t	blocks = 0;
	int	bitmap;
	char	*buf, *zero_buf;
	int	sparse = 0;
        errcode_t retval;

	buf = malloc(fs->blocksize);
	if (!buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
	zero_buf = malloc(fs->blocksize);
	if (!zero_buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
	memset(zero_buf, 0, fs->blocksize);

	for (blk = fs->super->s_first_data_block;
	     blk < fs->super->s_blocks_count; blk++) {
                if(fs->group_desc[group].bg_flags & EXT2_BG_BLOCK_UNINIT){ 
                        group++;
                        blk+=BLOCK_GROUP_OFFSET;
                        if (fd == 1) {
				write_block(fd, zero_buf, 0,
					    BLOCK_GROUP_OFFSET, blk);
				continue;
			}
                        sparse+=BLOCK_GROUP_OFFSET;
                        continue;
                }
                bitmap = ext2fs_fast_test_block_bitmap(fs->block_map, blk);
                if(bitmap) {
			retval = io_channel_read_blk(fs->io, blk, 1, buf);
			if (retval) {
				com_err(program_name, retval,
					"error reading block %u", blk);
			}
                        if ((fd != 1) && check_zero_block(buf, fs->blocksize))
				goto sparse_write;
               		write_block(fd, buf, sparse, fs->blocksize, blk);
			sparse = 0;
      
                }else {       
                sparse_write:
                        if (fd == 1) {
				write_block(fd, zero_buf, 0,
					    fs->blocksize, blk);
				continue;
			}
                        sparse += fs->blocksize;
                }
        }
}

/* Sepearate the names of target device, snapshot and snapshot file
 */
static void get_snapshot_filename(char *device, char *snapshot_name,
				  char *snapshot)
{	int i=0,j=0,temp;
	while(device[i++]!='@');
        temp=i;
	strcpy(snapshot,MNT); 
	strcat(snapshot,"/.snapshots/");
	while(device[i]!=0)
		snapshot_name[j++]=device[i++];
	snapshot_name[j]=device[i];
	strcat(snapshot,snapshot_name);
	device[temp-1]=0;
}

int main (int argc, char ** argv)
{
	errcode_t retval;
	ext2_filsys fs;
	char *image_fn,*device_name;
        char command[MAX],snapshot_file[MAX],snapshot_name[MAX];
	int open_flag = 0;
        int fd = 0;
        int optind=1;
        off_t block_count,block_size;
        
	fprintf (stderr, "e4send %s (%s)\n", E2FSPROGS_VERSION,
		 E2FSPROGS_DATE);
	if (optind != argc - 2 ) 
		usage();

        device_name = argv[optind];
	image_fn = argv[optind+1];
        get_snapshot_filename(device_name,snapshot_name,snapshot_file);
        printf("\nFile Path:%s\nSnapshot:%s\nDevice:%s\n",snapshot_file,snapshot_name,device_name);

        sprintf(command, "snapshot.ext4dev enable %s", snapshot_name);
        retval=system(command);

	retval = ext2fs_open (snapshot_file, open_flag, 0, 0,
			      unix_io_manager, &fs);
        if (retval) {
		com_err (program_name, retval, _("while trying to open %s"),
			 snapshot_file);
		fputs(_("Couldn't find valid filesystem superblock.\n"),
		      stdout);
		exit(1);
	}
        retval= ext2fs_read_bitmaps(fs);
        if (retval) {
		com_err (program_name, retval, "while trying to read bitmap");
		exit(1);
	}
	if (strcmp(image_fn, "-") == 0)
		fd = 1;
	else {
#ifdef HAVE_OPEN64
		fd = open64(image_fn, O_CREAT|O_TRUNC|O_WRONLY, 0600);
#else
		fd = open(image_fn, O_CREAT|O_TRUNC|O_WRONLY, 0600);
#endif
		if (fd < 0) {
			com_err(program_name, errno,
				_("while trying to open %s"), argv[optind+1]);
			exit(1);
		}
	}
        write_full_image(fs, fd);
        block_count=fs->super->s_blocks_count;
        block_size=fs->blocksize;
#ifdef HAVE_OPEN64
        ftruncate64(fd,block_count*block_size);
#else
        ftruncate(fd,block_count*block_size);
#endif
        ext2fs_close (fs);
        close(fd);
        printf("\n Full Backup of %s completed\n\n",snapshot_file);
	exit (0);
}

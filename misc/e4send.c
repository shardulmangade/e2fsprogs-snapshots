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
#define _FILE_OFFSET_BITS 64

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

#define MAX 150
#define MNT "/mnt/source"

#define CHUNK_BLOCK_SIZE 4096
#define CHUNK_BLOCKS_COUNT CHUNK_BLOCK_SIZE/sizeof(blk_t)
#define CHUNK_SIZE CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT + 1)

#define BLOCK_GROUP_OFFSET EXT2_BLOCKS_PER_GROUP(fs->super)*fs->blocksize
const char * program_name = "e4send";

static void usage(void)
{
	fprintf(stderr, "Usage: %s <device>@<snapshot_name> <image_file>\n In case of output to stdout <image-file> is -",
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

/* Retrive mount-point for device if mounted. If the device
 * is not mounted, then mount it and return the mount-point.
 */
static int get_mount_point(char *device, char *mount_point)
{       
        FILE *fp;
        char *line=NULL;
        char *target=NULL,*temp;
        size_t len =0;
        errcode_t err;
        
        fp=fopen("/proc/mounts","r");
        if (!fp) {
                com_err(program_name, errno,
			_("while trying to open /proc/mounts"));
		exit(1);        
        }
        while(target==NULL && getline(&line, &len, fp)!=-1)
                target=strstr(line, device);
        if(target){
                target+=strlen(device)+1;
                temp=strchr(target,' ');
                *temp=0;
                strcpy(mount_point,target);
        }
        else{                   
                if(mkdir(MNT,S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) != 0) {
                        if(errno!=EEXIST){
                                com_err(program_name, errno,
                                        _("while trying to create /mnt/source"));
                                exit(1);
                                
                        }
                }
                strcpy(mount_point,MNT);
                if(mount(device, mount_point, "ext4dev", 0, NULL)){
                        com_err(program_name, errno,
                                _("while trying to mount device"));
                        exit(1);
                }
        }
        fclose(fp);
   
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

/* Create the full backup in terms of chunks.The first block(metadata) of chunk
   stores the offsets and is followed by the data chunk blocks at corresponding offsets.
   fs corresponds to the source snapshot file
   fd is the file descriptor for destination file
*/

static void write_full_image_remote(ext2_filsys fs, int fd)
{
	blk_t	blk;
	int	group = 0;
	blk_t	blocks = 0;
	int	bitmap;
	char	*buf;
	int	sparse = 0;
        errcode_t retval;
        blk_t offset=0;
        int count;
        char *output_buf,*blk_buf;


	buf = malloc(CHUNK_BLOCK_SIZE);
	if (!buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
	output_buf = malloc(CHUNK_SIZE);  	if (!output_buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
        int f=0;
	for (blk = fs->super->s_first_data_block;
	     blk < (fs->super->s_blocks_count); blk++) {
                if(fs->group_desc[group].bg_flags & EXT2_BG_BLOCK_UNINIT){
                        group++;
                        blk+=BLOCK_GROUP_OFFSET;
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
                        if (!check_zero_block(buf, fs->blocksize)){
                                blk_buf=(char *) &sparse;
                                //fprintf(stderr,"\nWriting block %d at offset %d and sparse is %d",blk,offset,sparse);
                                memcpy(output_buf + sizeof(blk_t)*(offset), blk_buf, sizeof(blk_t));
                                sparse=0;
                                memcpy(output_buf + CHUNK_BLOCK_SIZE*(offset+1), buf, CHUNK_BLOCK_SIZE);
                                offset++;
                                if(offset >= CHUNK_BLOCKS_COUNT){
                                        count=write(fd, output_buf, CHUNK_SIZE);
                                        if(count<0)
                                        {
                                                retval=errno;
                                                com_err(program_name, retval, "error writing chunk");
                                                exit(1);
                                        }
                                        offset=0;
                                        
                                }
                                
                        }
                        else
                               sparse+=fs->blocksize;;
                }
        }
        count=write(fd, output_buf, offset*CHUNK_BLOCK_SIZE);
        if(count<0)
        {
                retval=errno;
                com_err(program_name, retval, "error writing chunk");
                exit(1);
        }
        count=write(fd, output_buf, CHUNK_SIZE-(offset*CHUNK_BLOCK_SIZE));
        memset(output_buf, -1, CHUNK_SIZE);
        count=write(fd, output_buf, CHUNK_SIZE);
        if(count<0)
        {
                retval=errno;
                com_err(program_name, retval, "error writing chunk");
                exit(1);
        }
        free(output_buf);
        free(buf);
}



/* Sepearate the names of target device, snapshot and snapshot file
 */
static void get_snapshot_filename(char *device, char *snapshot_name,
				  char *snapshot)
{	int i=0,j=0,temp;
        errcode_t retval;
        char command[MAX];
	while(device[i++]!='@');
        temp=i;
	while(device[i]!=0)
		snapshot_name[j++]=device[i++];
	snapshot_name[j]=device[i];
        device[temp-1]=0;
        get_mount_point(device,snapshot);
        sprintf(command, "snapshot.ext4dev config %s %s", device, snapshot);
        retval=system(command);
        sprintf(command, "snapshot.ext4dev enable %s@%s", snapshot, snapshot_name);
        retval=system(command);
	strcat(snapshot,"/.snapshots/");
	strcat(snapshot,snapshot_name);
	
}

int main (int argc, char ** argv)
{
        int c;
        errcode_t retval;
	ext2_filsys fs;
	char *image_fn,*device_name,*trunc_val;
        char command[MAX],snapshot_file[MAX],snapshot_name[MAX],mount_point[MAX];
	int open_flag = 0,local_flag = 0,incremental_flag=0;
        int fd=0,optind=1,ret;
        off_t block_count,block_size;
        
	fprintf (stderr, "e4send %s (%s)\n", E2FSPROGS_VERSION,
		 E2FSPROGS_DATE);
       	while ((c = getopt (argc, argv, "l")) != EOF)
		switch (c) {
		case 'l':
			local_flag++;
                        optind++;       
			break;
                default:
			usage();
		}
	if (optind != argc - 2 ) 
		usage();

        device_name = argv[optind];
	image_fn = argv[optind+1];
        get_snapshot_filename(device_name,snapshot_name,snapshot_file);
        printf("\nDevice:%s\nSnapshot:%s\nSnapshot file path:%s\n",device_name,snapshot_name,snapshot_file);

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
        if(incremental_flag)
        {       
                        
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
        if(local_flag){
                write_full_image(fs, fd);
                block_count=fs->super->s_blocks_count;
                block_size=fs->blocksize;
#ifdef HAVE_OPEN64
                ret=ftruncate64(fd,block_count*block_size);
#else
                ret=ftruncate(fd,block_count*block_size);
#endif
                if(ret<0){
                        com_err (program_name, retval, _("while trying to truncate %s"),
        			 image_fn);
                }
        }
        else
        {       block_count=fs->super->s_blocks_count;
                block_size=fs->blocksize;
                trunc_val=(char *)&block_count;        
                //fprintf(stderr,"\nHEY  %lu %u\n",block_count,fs->blocksize);
                ret=write(1,trunc_val,sizeof(off_t));
                if(ret<0){
                        com_err (program_name, retval, "while trying to write to destination");
                }
                trunc_val=(char *)&block_size;
                ret=write(1,trunc_val, sizeof(off_t));
                if(ret<0){
                        com_err (program_name, retval, "while trying to write to destination");
                }
                write_full_image_remote(fs,fd);
        }
        ext2fs_close (fs);
        close(fd);
        printf("\nFull Backup of %s completed\n\n",snapshot_file);
	exit (0);
}

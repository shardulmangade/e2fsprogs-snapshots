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
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern char *optarg;
extern int optind;
#endif
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

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
#define SNAPSHOT_SHIFT 0

#define CHUNK_BLOCK_SIZE 4096
#define CHUNK_BLOCKS_COUNT CHUNK_BLOCK_SIZE/sizeof(blk_t)
#define CHUNK_SIZE CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT + 1)

#define BLOCK_GROUP_OFFSET(fs) EXT2_BLOCKS_PER_GROUP(fs->super)*fs->blocksize
const char * program_name = "e4send";

static void usage(void)
{
	printf("Usage: %s\n -F  <device>@<snapshot_name> <image_file> : Full backup to local device\n-F  <device>@<snapshot_name> -                : Full backup to remote device\n -i  <device>@<snapshot2> <device>@<snapshot1> <local_device> : Send deltas over snapshot 2 to snapshot1 to local device",	program_name);
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
                        blk+=BLOCK_GROUP_OFFSET(fs);
                        if (fd == 1) {
				write_block(fd, zero_buf, 0,
					    BLOCK_GROUP_OFFSET(fs), blk);
				continue;
			}
                        sparse+=BLOCK_GROUP_OFFSET(fs);
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
                        blk+=BLOCK_GROUP_OFFSET(fs);
                        sparse+=BLOCK_GROUP_OFFSET(fs);
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
        /* Padding with redundancy */
        if(offset<CHUNK_BLOCKS_COUNT)
                for(;offset<CHUNK_BLOCKS_COUNT;offset++)        
                {       memcpy(output_buf + sizeof(blk_t)*(offset), output_buf, sizeof(blk_t));
                        memcpy(output_buf + CHUNK_BLOCK_SIZE*(offset+1), output_buf + CHUNK_BLOCK_SIZE, CHUNK_BLOCK_SIZE);
                }
        count=write(fd, output_buf, CHUNK_SIZE);
        memset(output_buf, -1, CHUNK_SIZE);
        count=write(fd, output_buf, CHUNK_SIZE);
        if(count<0)
        {
                retval=errno;
                com_err(program_name, retval, "error writing chunk");
                exit(1);
        }
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


/*
 * Input-
 * fd: Open file descriptor to read fiemap from
 *
 * The function applies fiemap ioctl on fd and reads fiemaps
 */
struct fiemap *read_fiemap(int fd)
{
        struct fiemap *fiemap;
        int extents_size;
	unsigned long seek;

        if ((fiemap = (struct fiemap*)malloc(sizeof(struct fiemap))) == NULL) {
                fprintf(stderr, "Out of memory allocating fiemap\n");   
                return NULL;
        }
        //memset(fiemap, 0, sizeof(struct fiemap));

        fiemap->fm_start = 0;
        fiemap->fm_length = ~0;
        fiemap->fm_flags = 0;
        fiemap->fm_extent_count = 0;
        fiemap->fm_mapped_extents = 0;

        if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
                fprintf(stderr, "fiemap ioctl() failed\n");
                return NULL;
                
        }
        
        extents_size = sizeof(struct fiemap_extent) * 
                              (fiemap->fm_mapped_extents);

        if ((fiemap = (struct fiemap*)realloc(fiemap,sizeof(struct fiemap) + 
                                         extents_size)) == NULL) {
                fprintf(stderr, "Out of memory allocating fiemap\n");   
                return NULL;
        }

        memset(fiemap->fm_extents, 0, extents_size);
        fiemap->fm_extent_count = fiemap->fm_mapped_extents;
        fiemap->fm_mapped_extents = 0;

        if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
                fprintf(stderr, "fiemap ioctl() failed\n");
                return NULL;
        }

        return fiemap;
}

/*
 * dumps fiemap to temporary disk image 
 * called only if we are in revert mode
 */
void dump_fiemap(struct fiemap *fiemap2, int snapshot_file, int disk_image,int flag)
{
	int i,j,k;
	void *buf;
	static int full=0;
	static unsigned long offset=0;
        char extents[sizeof(fiemap2->fm_mapped_extents)];
        int ret;
        if(flag)
                write(1,(char *)fiemap2->fm_mapped_extents,fiemap2->fm_mapped_extents);

	for (i=0;i<fiemap2->fm_mapped_extents;i++) {
		if(fiemap2->fm_extents[i].fe_logical - SNAPSHOT_SHIFT !=
		   fiemap2->fm_extents[i].fe_physical) {
			buf = (void *)malloc(fiemap2->fm_extents[i].fe_length);
                        //printf("\nEnterd in for offset %lu",fiemap2->fm_extents[i].fe_logical - SNAPSHOT_SHIFT);
			lseek(snapshot_file, fiemap2->fm_extents[i].fe_logical - SNAPSHOT_SHIFT, SEEK_SET);
			ret=read(snapshot_file, buf, fiemap2->fm_extents[i].fe_length);			

                        if(flag){
                                write(1, &fiemap2->fm_extents[i].fe_logical - SNAPSHOT_SHIFT, sizeof(fiemap2->fm_extents[i].fe_logical));
                                write(1, &fiemap2->fm_extents[i].fe_length, sizeof(fiemap2->fm_extents[i].fe_length));
                                write(disk_image, buf, fiemap2->fm_extents[i].fe_length);      
                                      
                        }
                        else{
                                
                                lseek(disk_image, fiemap2->fm_extents[i].fe_logical - SNAPSHOT_SHIFT, SEEK_SET);
                                ret=write(disk_image, buf, fiemap2->fm_extents[i].fe_length);
                                free(buf);
                        }
		}
	}
      
}

/* Create the incremental backup in terms of deltas from fs1 and fs2.
   The deltas are applied over to fs3.
*/
static void write_incremental_local(ext2_filsys fs1, ext2_filsys fs2, ext2_filsys fs3)
{
	blk_t	blk;
	int	group = 0;
	blk_t	blocks = 0;
	int	bitmap1,bitmap2;
	char	*buf,*zero_buf;
	off_t	sparse = 0;
        errcode_t retval;
        blk_t offset=0;
        int count;
        char *output_buf,*blk_buf;

	buf = malloc(fs1->blocksize);
	if (!buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
	zero_buf = malloc(fs1->blocksize);
	if (!zero_buf) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
	memset(zero_buf, 0, fs1->blocksize);
        for (blk = fs1->super->s_first_data_block;
	     blk < (fs1->super->s_blocks_count); blk++) {
                if(fs1->group_desc[group].bg_flags & EXT2_BG_BLOCK_UNINIT){
                        group++;
                        blk+=BLOCK_GROUP_OFFSET(fs1);
                        sparse+=BLOCK_GROUP_OFFSET(fs1);
                        continue;
                }
                bitmap1 = ext2fs_fast_test_block_bitmap(fs1->block_map, blk);
                bitmap2 = ext2fs_fast_test_block_bitmap(fs2->block_map, blk);

                if(bitmap1 && !bitmap2) {

                        retval = io_channel_read_blk(fs1->io, blk, 1, buf);
			if (retval) {
				com_err(program_name, retval,
					"error reading block %u", blk);
			}
                        if (check_zero_block(buf, fs1->blocksize))
				continue;

                        retval = io_channel_write_blk(fs3->io, blk, 1, buf);
			if (retval) {
				com_err(program_name, retval,
					"error reading block %u", blk);
			}
			
      
                }
        }

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
	ext2_filsys fs,fs2,fs3;
	char *image_fn,*device_name,*device_name2,*trunc_val;
        char command[MAX],snapshot_file[MAX],snapshot_name[MAX],mount_point[MAX];
        char snapshot_file2[MAX],snapshot_name2[MAX],mount_point2[MAX];
	int open_flag = 0,local_flag = 0,incremental_flag=0,full_flag;
        int fd=0,ret,opts=2;
        int fsd1,fsd2,fsd3;
        struct fiemap *fiemap1,*fiemap2;
        off_t block_count,block_size;
        
	fprintf (stderr, "e4send %s (%s)\n ", E2FSPROGS_VERSION,
		 E2FSPROGS_DATE);
       	while ((c = getopt (argc, argv, "ilF")) != EOF)
		switch (c) {
		case 'i':
			incremental_flag++;
                        opts++;       
			break;
                case 'F':
			full_flag++;
			break;
               
                default:
			usage();
		}
	if (optind != argc - opts ) 
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
        block_count=fs->super->s_blocks_count;
        block_size=fs->blocksize;

        if(full_flag)
        {       if(!strcmp(image_fn, "-")){
                        fd=1;
                         trunc_val=(char *)&block_count;        
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
                 else{
               
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
                
                        write_full_image(fs, fd);
                }
                if(strcmp(image_fn, "-")){
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

                close(fd);

        }
        else
        {
                close(fd);
                device_name2 =argv[optind+1];
                image_fn=argv[optind+2];
                get_snapshot_filename(device_name2,snapshot_name2,snapshot_file2);
                printf("\nDevice:%s\nSnapshot:%s\nSnapshot file path:%s\n",device_name2,snapshot_name2,snapshot_file2);

                retval = ext2fs_open (snapshot_file2, open_flag, 0, 0,
			      unix_io_manager, &fs2);
                if (retval) {
        		com_err (program_name, retval, _("while trying to open %s"),
        			 snapshot_file2);
        		fputs(_("Couldn't find valid filesystem superblock.\n"),
        		      stdout);
        		exit(1);
        	}
                retval= ext2fs_read_bitmaps(fs2);
                if (retval) {
        		com_err (program_name, retval, "while trying to read bitmap");
        		exit(1);
        	}       
                        
                fsd1 = open(snapshot_file, O_RDONLY, 0600);
                if(fsd1<0)
                        fprintf(stderr,"Error opening snapshot file");
		fsd2 = open(snapshot_file2, O_RDONLY, 0600);
                if(fsd2<0)
                        fprintf(stderr,"Error opening snapshot file");
                fsd3 = open(image_fn, O_WRONLY, 0600);
                if(fsd3<0)
                        fprintf(stderr,"Error opening target device");
                
                fiemap2=read_fiemap(fsd2);
                dump_fiemap(fiemap2,fsd1,fsd3,0);


#ifdef HAVE_OPEN64
                        ret=ftruncate64(fsd3,block_count*block_size);
#else
                        ret=ftruncate(fsd3,block_count*block_size);
#endif
                        if(ret<0){
                                com_err (program_name, retval, _("while trying to truncate %s"),
                                	 image_fn);
                        }
       
                close(fsd3);
                close(fsd1);
                close(fsd2);

                retval = ext2fs_open (image_fn, EXT2_FLAG_RW, 0, 0,
       			      unix_io_manager, &fs3);
                if (retval) {
               		com_err (program_name, retval, _("while trying to open %s"),
               			 image_fn);
               		fputs(_("Couldn't find valid filesystem superblock.\n"),
               		      stdout);
               		exit(1);
               	}
                write_incremental_local(fs,fs2,fs3);
               ext2fs_close (fs3);
                ext2fs_close (fs2);

        }
        ext2fs_close (fs);
	exit (0);
}

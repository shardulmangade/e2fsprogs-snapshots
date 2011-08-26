/*
 * e4receive.c --- Program which receieves and writes an image file backing up
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

const char * program_name = "e4receive";

static void usage(void)
{
	fprintf(stderr, _("Usage: %s <target_device>\n"),
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

/*Print table : For debugging purpose
 */
static void print_output_table(char * buff)
{       
        unsigned int i;
        static int f=0;
        int ret;       
        blk_t *target_off;
        blk_t blk=0;
        char data1[CHUNK_BLOCK_SIZE];
        memset(data1,0,CHUNK_BLOCK_SIZE);
        for(i=0; i<((CHUNK_BLOCK_SIZE)/sizeof(blk_t)); i++)
        {
                target_off=(blk_t*)(buff + i*(sizeof(blk_t)));
                blk=*target_off; 
                //memcpy(data1, buff+CHUNK_BLOCK_SIZE*(i+1), CHUNK_BLOCK_SIZE);
                fprintf(stdout,"\n%d\t%d\t %u\t",f++,i,blk);
                //ret=write(1,data1,1);
        }
}


/* Create the full backup image file
   fd is the file descriptor for destination file
*/
static void receive_full_image(int fd)
{
	blk_t *target;
        int fin=0;
        char *sector,*buff, data[CHUNK_BLOCK_SIZE], *terminate;
        errcode_t retval;
        int i;        
        ext2_filsys fs;
        char test[MAX]="0";
        off_t offset;
        int ret;

	buff = malloc(CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT+1));
	if (!buff) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
        sector = malloc(CHUNK_BLOCK_SIZE);
	if (!sector) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
        terminate = malloc(CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT+1));
	if (!terminate) {
		com_err(program_name, ENOMEM, "while allocating buffer");
		exit(1);
	}
        memset(terminate, -1, CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT+1));
        memset(sector, 0, CHUNK_BLOCK_SIZE);

        while(1)
        {
                memset(buff, 0, CHUNK_BLOCK_SIZE*(CHUNK_BLOCKS_COUNT+1));
                /* CHUNK_BLOCKS_COUNT + 1, here +1 is for the metadata block of chunk */
                for(i=0;i<CHUNK_BLOCKS_COUNT+1;i++)
                {
                        ret=read(fin, sector, CHUNK_BLOCK_SIZE);
                        if(ret<0)
                        {       
                                retval=errno;
                                com_err(program_name, retval, "error writing chunk");
                                exit(1);
                        }
                        memcpy(buff + i*CHUNK_BLOCK_SIZE, sector, CHUNK_BLOCK_SIZE);
                }
                /* Check if the chunk is the last chunk */
                if(!strcmp(buff,terminate))
                        break;     
                for(i=0; i<CHUNK_BLOCKS_COUNT; i++)
                {       target=(blk_t *)(buff + (i*sizeof(blk_t)));
                        offset=(int)(* target);
                        memcpy(data, buff + CHUNK_BLOCK_SIZE*(i+1), CHUNK_BLOCK_SIZE);
#ifdef HAVE_LSEEK64
        		if (lseek64(fd, offset, SEEK_CUR) < 0){
                		perror("lseek");
                        }
#else
                        if (lseek(fd,offset, SEEK_CUR) < 0){
                                perror("lseek");
                        }
#endif
                        ret= write(fd, data, CHUNK_BLOCK_SIZE);
                        if(ret<0)
                        {       
                                retval=errno;
                                com_err(program_name, retval, "error writing chunk");
                                exit(1);
                        }
                                        
                }
                
        }
        free(sector);
        free(buff);
        free(terminate);

}

static void receive_incremental(char *device)
{
        char *read_data;
        __u64 data,length,logical_off;
        int extents;
        int i,fd;
        void *buf;
        blk_t blk;
        __u32 *mapped,mapped_buf;
        int ret;
        int open_flag=0;
        errcode_t retval;
        ext2_filsys fs;

        fd = open(device, O_WRONLY, 0600);
        if(fd<0)
                fprintf(stderr,"Error opening target device");

        read_data=malloc(sizeof(data));
        read_data=malloc(sizeof(data));

        ret=read(0,read_data,sizeof(mapped_buf));
        mapped_buf=*(__u32 *)read_data;
        /* Debugging */
       
        extents=mapped_buf;

        for(i=0;i<extents;i++)
        {
                
                ret=read(0,read_data,sizeof(data));
                logical_off=*(__u64*)read_data;

        
                ret=read(0,read_data,sizeof(data));
                length=*(__u64*)read_data;

                buf = (void *)malloc(length); 
                ret=read(0,buf,length);

               
                lseek(fd, logical_off - SNAPSHOT_SHIFT, SEEK_SET);
                ret=write(fd, buf, length);
                free(buf);
                
        }
        close(fd);
        
        fprintf(stderr, "Done with mapping");
        retval = ext2fs_open (device, EXT2_FLAG_RW, 0, 0,
			      unix_io_manager, &fs);
        if (retval) {
		com_err (program_name, retval, _("while trying to open %s"),
			 device);
		fputs(_("Couldn't find valid filesystem superblock.\n"),
		      stdout);
		exit(1);
	}
 
        buf=malloc(fs->blocksize);                    
        while(1)
        {
                ret=read(0, read_data, sizeof(blk_t));
                blk=*(blk_t *)read_data;
                if(blk==-1)
                        break;
                ret=read(0, buf, fs->blocksize);
                write(1,buf,fs->blocksize);
                retval = io_channel_write_blk(fs->io, blk, 1, buf);
		if (retval) {
			com_err(program_name, retval,
				"error reading block %u", blk);
		}
                         
                
        }
        free(buf);
        ext2fs_close (fs);
}

int main (int argc, char ** argv)
{
	errcode_t retval;
	ext2_filsys fs;
	char device_name[MAX];
        char command[MAX];
        char temp[CHUNK_BLOCK_SIZE];
	int fd=0,ret;
        off_t block_count,block_size;
        int incremental_flag=0;
        char *values, c;
        values=malloc(sizeof(off_t));
	fprintf (stderr, "e4receive %s (%s)", E2FSPROGS_VERSION,
		 E2FSPROGS_DATE);

       	while ((c = getopt (argc, argv, "i")) != EOF)
		switch (c) {
		case 'i':
			incremental_flag++;
                        break;
              
                default:
			usage();
		}
	if (argc != optind + 1 ) 
		usage();


        strcpy(device_name ,argv[optind]);
        
        /* Initial redundant data */
        ret=read(0, temp, CHUNK_BLOCK_SIZE);
        if(ret<0)
        {       
                retval=errno;
                com_err(program_name, retval, "error writing chunk");
                exit(1);
        }

        if(!incremental_flag){

#ifdef HAVE_OPEN64
        		fd = open64(device_name, O_CREAT|O_TRUNC|O_WRONLY, 0600);
#else
        		fd = open(device_name, O_CREAT|O_TRUNC|O_WRONLY, 0600);
#endif
                	if (fd < 0) {
        			com_err(program_name, errno,
        				_(" while trying to open %s"), device_name);
        			exit(1);
                        }
        
                //write(1, temp, CHUNK_SIZE);    /*  Debugging  */
                /* block count for truncate */
                ret=read(0,values,sizeof(off_t));
                if(ret<0)
                {
                        retval=errno;
                        com_err(program_name, retval, "error writing chunk");
                        exit(1);
                }
                block_count=*(off_t *)values;
                /* block size for truncate */
                ret=read(0,values,sizeof(off_t));
                if(ret<0)
                {
                        retval=errno;
                        com_err(program_name, retval, "error writing chunk");
                        exit(1);
                }
                block_size=*(off_t *)values;
                receive_full_image(fd);
#ifdef HAVE_OPEN64
                ret=ftruncate64(fd,block_count*block_size);
#else
                ret=ftruncate(fd,block_count*block_size);
#endif
                close(fd);

        }
        else{
                /* Initial redundant data */
                ret=read(0, temp, CHUNK_BLOCK_SIZE);      
                if(ret<0)
                {
                        retval=errno;
                        com_err(program_name, retval, "error writing chunk");
                        exit(1);
                }
        
        
                receive_incremental(device_name);
        }

        printf("\nFull Backup completed\n\n");
        exit (0);
}

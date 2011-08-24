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

int main (int argc, char ** argv)
{
	errcode_t retval;
	ext2_filsys fs;
	char device_name[MAX];
        char command[MAX];
        char temp[CHUNK_BLOCK_SIZE];
	int fd=0,optind=1,ret;
        off_t block_count,block_size;
        char *values;
        values=malloc(sizeof(off_t));
	fprintf (stderr, "e4receive %s (%s)\n", E2FSPROGS_VERSION,
		 E2FSPROGS_DATE);
	if (optind != argc - 1 ) 
		usage();

        strcpy(device_name ,argv[optind]);

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
        ret=read(0, temp, CHUNK_BLOCK_SIZE);      /*    Initial redundant data  */
        if(ret<0)
        {       
                retval=errno;
                com_err(program_name, retval, "error writing chunk");
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
        printf("\nFull Backup completed\n\n");
        exit (0);
}

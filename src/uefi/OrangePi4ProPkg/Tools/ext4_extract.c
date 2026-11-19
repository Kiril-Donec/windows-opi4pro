#include <stdio.h>
#include <stdlib.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_err.h>
#include <com_err.h>
static void die(errcode_t e,const char*w){if(e){fprintf(stderr,"%s: %s\n",w,error_message(e));exit(1);}}
int main(int c,char**v){if(c!=4)return 2;initialize_ext2_error_table();ext2_filsys fs=0;die(ext2fs_open(v[1],0,0,0,unix_io_manager,&fs),"open");ext2_ino_t ino=0;die(ext2fs_namei_follow(fs,EXT2_ROOT_INO,EXT2_ROOT_INO,v[2],&ino),"namei");struct ext2_inode inode;die(ext2fs_read_inode(fs,ino,&inode),"inode");ext2_off64_t size=EXT2_I_SIZE(&inode);ext2_file_t f=0;die(ext2fs_file_open(fs,ino,0,&f),"file open");FILE*out=fopen(v[3],"wb");if(!out){perror(v[3]);return 1;}char*b=malloc(1<<20);ext2_off64_t done=0;while(done<size){unsigned int want=(unsigned int)((size-done)>(1<<20)?(1<<20):(size-done)),got=0;die(ext2fs_file_read(f,b,want,&got),"read");if(!got)break;fwrite(b,1,got,out);done+=got;}fclose(out);free(b);die(ext2fs_file_close(f),"file close");die(ext2fs_close(fs),"fs close");printf("extracted %lld bytes\n",(long long)done);return done==size?0:1;}

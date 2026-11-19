#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_err.h>
#include <com_err.h>

static void die(errcode_t err, const char *what) {
    if (err) {
        fprintf(stderr, "%s: %s\n", what, error_message(err));
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s filesystem.img /path/in/fs local-file\n", argv[0]);
        return 2;
    }
    initialize_ext2_error_table();
    FILE *in = fopen(argv[3], "rb");
    if (!in) { perror(argv[3]); return 1; }
    if (fseeko(in, 0, SEEK_END) != 0) { perror("fseeko"); return 1; }
    off_t sz = ftello(in);
    if (sz < 0) { perror("ftello"); return 1; }
    rewind(in);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { perror("malloc"); return 1; }
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz) { perror("fread"); return 1; }
    fclose(in);

    ext2_filsys fs = NULL;
    errcode_t err = ext2fs_open(argv[1], EXT2_FLAG_RW, 0, 0, unix_io_manager, &fs);
    die(err, "ext2fs_open");
    die(ext2fs_read_inode_bitmap(fs), "read inode bitmap");
    die(ext2fs_read_block_bitmap(fs), "read block bitmap");

    ext2_ino_t ino = 0;
    err = ext2fs_namei_follow(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, argv[2], &ino);
    die(err, "find destination file");

    ext2_file_t file = NULL;
    die(ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file), "open destination file");
    die(ext2fs_file_set_size2(file, 0), "truncate destination file");
    die(ext2fs_file_set_size2(file, (ext2_off64_t)sz), "resize destination file");
    unsigned int written = 0;
    size_t done = 0;
    while (done < (size_t)sz) {
        unsigned int chunk = (unsigned int)(((size_t)sz - done) > (1U<<20) ? (1U<<20) : ((size_t)sz - done));
        written = 0;
        die(ext2fs_file_write(file, buf + done, chunk, &written), "write destination file");
        if (written == 0) { fprintf(stderr, "short write at %zu\n", done); return 1; }
        done += written;
    }
    die(ext2fs_file_flush(file), "flush destination file");
    die(ext2fs_file_close(file), "close destination file");
    die(ext2fs_flush(fs), "flush filesystem");
    die(ext2fs_close(fs), "close filesystem");
    free(buf);
    printf("replaced %s with %s (%lld bytes)\n", argv[2], argv[3], (long long)sz);
    return 0;
}

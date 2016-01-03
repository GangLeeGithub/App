#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sha.h>
#include <bootimg.h>
#include <bootimg_tools.h>

#include <android/log.h>

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "ImageFactory-JNI", __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG , "ImageFactory-JNI", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO  , "ImageFactory-JNI", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN  , "ImageFactory-JNI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR  , "ImageFactory-JNI", __VA_ARGS__)

static void *load_file(const char *fn, unsigned *_sz)
{
    char *data;
    int sz;
    int fd;
    data = 0;
    fd = open(fn, O_RDONLY);
    if(fd < 0) return 0;

    sz = lseek(fd, 0, SEEK_END);
    if(sz < 0) goto oops;

    if(lseek(fd, 0, SEEK_SET) != 0) goto oops;

    data = (char*) malloc(sz);
    if(data == 0) goto oops;

    if(read(fd, data, sz) != sz) goto oops;
    close(fd);

    if(_sz) *_sz = sz;
    return data;

oops:
    close(fd);
    if(data != 0) free(data);
    return 0;
}

int __inline repack_usage(void)
{
    fprintf(stderr,"\n语法:--repackbootimg [附加参数]"
            "\n              --kernel"
            "\n              --ramdisk"
            "\n              --cmdline"
            "\n              --base"
            "\n              --pagesize"
            "\n              --dt"
            "\n              --output\n"
            );
    return 1;
}



static unsigned char padding[4096] = { 0, };

int write_padding(int fd, unsigned pagesize, unsigned itemsize)
{
    unsigned pagemask = pagesize - 1;
    unsigned count;

    if((itemsize & pagemask) == 0) {
        return 0;
    }

    count = pagesize - (itemsize & pagemask);

    if(write(fd, padding, count) != (signed)count) {
        return -1;
    } else {
        return 0;
    }
}

int repackbootimg_main(int argc, char **argv)
{
    boot_img_hdr hdr;

    char *kernel_fn = 0;
    void *kernel_data = 0;
    char *ramdisk_fn = 0;
    void *ramdisk_data = 0;
    char *second_fn = 0;
    void *second_data = 0;
    char *cmdline = "";
    char *bootimg = 0;
    char *board = "";
    char *dt_fn = 0;
    void *dt_data = 0;
    unsigned pagesize = 2048;
    int fd;
    SHA_CTX ctx;
    const uint8_t* sha;

    argc--;
    argv++;

    memset(&hdr, 0, sizeof(hdr));

        /* default load addresses */
    hdr.kernel_addr =  0x10008000;
    hdr.ramdisk_addr = 0x11000000;
    hdr.second_addr =  0x10F00000;
    hdr.tags_addr =    0x10000100;

    while(argc > 0){
        char *arg = argv[0];
        char *val = argv[1];
        if(argc < 2) {
            return repack_usage();
        }
        argc -= 2;
        argv += 2;
        if(!strcmp(arg, "--output") || !strcmp(arg, "-o")) {
            bootimg = val;
        } else if(!strcmp(arg, "--kernel")) {
            kernel_fn = val;
        } else if(!strcmp(arg, "--ramdisk")) {
            ramdisk_fn = val;
        } else if(!strcmp(arg, "--second")) {
            second_fn = val;
        } else if(!strcmp(arg, "--cmdline")) {
            cmdline = val;
        } else if(!strcmp(arg, "--base")) {
            unsigned base = strtoul(val, 0, 16);
            hdr.kernel_addr =  base + 0x00008000;
            hdr.ramdisk_addr = base + 0x01000000;
            hdr.second_addr =  base + 0x00F00000;
            hdr.tags_addr =    base + 0x00000100;
        } else if(!strcmp(arg, "--ramdisk_offset")) {
            hdr.ramdisk_addr = strtoul(val, 0, 16);
        } else if(!strcmp(arg, "--board")) {
            board = val;
        } else if(!strcmp(arg,"--pagesize")) {
            pagesize = strtoul(val, 0, 10);
            if ((pagesize != 2048) && (pagesize != 4096)) {
                fprintf(stderr,"error: unsupported page size %d\n", pagesize);
                return -1;
            }
        } else if(!strcmp(arg, "--dt")) {
            dt_fn = val;
        } else {
            return repack_usage();
        }
    }
    hdr.page_size = pagesize;


    if(bootimg == 0) {
        fprintf(stderr,"error: no output filename specified\n");
        return repack_usage();
    }

    if(kernel_fn == 0) {
        fprintf(stderr,"error: no kernel image specified\n");
        return repack_usage();
    }

    if(ramdisk_fn == 0) {
        fprintf(stderr,"error: no ramdisk image specified\n");
        return repack_usage();
    }

    if(strlen(board) >= BOOT_NAME_SIZE) {
        fprintf(stderr,"error: board name too large\n");
        return repack_usage();
    }

    strcpy((char *)hdr.name, board);

    memcpy(hdr.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE);

    if(strlen(cmdline) > (BOOT_ARGS_SIZE - 1)) {
        fprintf(stderr,"error: kernel commandline too large\n");
        return 1;
    }
    strcpy((char*)hdr.cmdline, cmdline);

    kernel_data = load_file(kernel_fn, &hdr.kernel_size);
    if(kernel_data == 0) {
        fprintf(stderr,"error: could not load kernel '%s'\n", kernel_fn);
        return 1;
    }

    if(!strcmp(ramdisk_fn,"NONE")) {
        ramdisk_data = 0;
        hdr.ramdisk_size = 0;
    } else {
        ramdisk_data = load_file(ramdisk_fn, &hdr.ramdisk_size);
        if(ramdisk_data == 0) {
            fprintf(stderr,"error: could not load ramdisk '%s'\n", ramdisk_fn);
            return 1;
        }
    }

    if(second_fn) {
        second_data = load_file(second_fn, &hdr.second_size);
        if(second_data == 0) {
            fprintf(stderr,"error: could not load secondstage '%s'\n", second_fn);
            return 1;
        }
    }

    if(dt_fn) {
        dt_data = load_file(dt_fn, &hdr.dt_size);
        if (dt_data == 0) {
            fprintf(stderr,"error: could not load device tree image '%s'\n", dt_fn);
            return 1;
        }
    }

    /* put a hash of the contents in the header so boot images can be
     * differentiated based on their first 2k.
     */
    SHA_init(&ctx);
    SHA_update(&ctx, kernel_data, (int)hdr.kernel_size);
    SHA_update(&ctx, &hdr.kernel_size, (int)sizeof(hdr.kernel_size));
    SHA_update(&ctx, ramdisk_data, hdr.ramdisk_size);
    SHA_update(&ctx, &hdr.ramdisk_size, (int)sizeof(hdr.ramdisk_size));
    SHA_update(&ctx, second_data, (int)hdr.second_size);
    SHA_update(&ctx, &hdr.second_size, (int)sizeof(hdr.second_size));
    if(dt_data) {
        SHA_update(&ctx, dt_data, hdr.dt_size);
        SHA_update(&ctx, &hdr.dt_size, sizeof(hdr.dt_size));
    }
    sha = SHA_final(&ctx);
    memcpy(hdr.id, sha,
           SHA_DIGEST_SIZE > sizeof(hdr.id) ? sizeof(hdr.id) : SHA_DIGEST_SIZE);

    fd = open(bootimg, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if(fd < 0) {
        fprintf(stderr,"error: could not create '%s'\n", bootimg);
        return 1;
    }

    if(write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) goto fail;
    if(write_padding(fd, pagesize, sizeof(hdr))) goto fail;

    if(write(fd, kernel_data, hdr.kernel_size) != (signed)hdr.kernel_size) goto fail;
    if(write_padding(fd, pagesize, hdr.kernel_size)) goto fail;

    if(write(fd, ramdisk_data, hdr.ramdisk_size) != (signed)hdr.ramdisk_size) goto fail;
    if(write_padding(fd, pagesize, hdr.ramdisk_size)) goto fail;

    if(second_data) {
        if(write(fd, second_data, hdr.second_size) != (signed)hdr.second_size) goto fail;
        if(write_padding(fd, pagesize, hdr.ramdisk_size)) goto fail;
    }

    if(dt_data) {
        if(write(fd, dt_data, hdr.dt_size) != hdr.dt_size) goto fail;
        if(write_padding(fd, pagesize, hdr.dt_size)) goto fail;
    }

    return 0;

fail:
    unlink(bootimg);
    close(fd);
    fprintf(stderr,"error: failed writing '%s': %s\n", bootimg,
            strerror(errno));
    return 1;
}
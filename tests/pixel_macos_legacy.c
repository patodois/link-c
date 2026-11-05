/* A low deployment target must never call the unavailable preadv symbol. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <AvailabilityMacros.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 110000
#error This test must be compiled for a macOS deployment target older than 11.0
#endif
static unsigned vector_calls;
/* Simulate the absent entry point without calling through a null weak import. */
static ssize_t unavailable_preadv(int fd, const struct iovec* vectors, int count, off_t offset) {
    (void)fd; (void)vectors; (void)count; (void)offset;
    vector_calls++; errno=ENOSYS; return -1;
}
#define preadv unavailable_preadv
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
int main(void) {
    rbl_pixel_writer* writer; rbl_pixel_reader* reader;
    uint8_t pixels[8*9*4], got[8*9*4];float depth[8*9], copied[8*9];uint64_t number;int i,rc;
    rbl_frame frame;
    /* Retain the test stub even when the production vector path is compiled out. */
    (void)unavailable_preadv;
    for(i=0;i<(int)sizeof pixels;i++) pixels[i]=(uint8_t)(i*37);
    for(i=0;i<8*9;i++) depth[i]=(float)i+0.5f;
    if(rbl_pixel_writer_create_temp("legacy-macos",16,16,1,1,&writer)) return 2;
    if(rbl_pixel_writer_write(writer,pixels,sizeof pixels,depth,8*9,8,9,0,1,&number)) return 3;
    if(rbl_pixel_reader_open(rbl_pixel_writer_path(writer),&reader)) return 4;
    memset(&frame,0,sizeof frame);frame.frame=number;frame.width=8;frame.height=9;
    rc=rbl_pixel_reader_read_latest(reader,&frame,got,sizeof got,copied,8*9,NULL);
    printf("deployment=%d vector_calls=%u read=%d\n",MAC_OS_X_VERSION_MIN_REQUIRED,vector_calls,rc);
    rbl_pixel_reader_close(reader);rbl_pixel_writer_close(writer);
    if(rc || vector_calls || memcmp(pixels,got,sizeof pixels) || memcmp(depth,copied,sizeof depth)) {
        puts("FAIL: low-target cropped colour and depth must use scalar positional I/O");return 1;
    }
    puts("legacy target: every colour and depth pixel matches, no preadv call");return 0;
}

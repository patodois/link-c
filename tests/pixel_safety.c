#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int truncate_fd = -1, armed;
static int checked_fstat(int fd, struct stat* st) {
    int rc = fstat(fd, st);
    if (!rc && armed) {
        armed = 0;
        if (ftruncate(truncate_fd, 0)) abort();
        puts("truncated after successful fstat"); fflush(stdout);
    }
    return rc;
}
static off_t truncate_at = -1;
static unsigned copy_calls;
static ssize_t checked_pread(int fd, void* dst, size_t len, off_t offset) {
    copy_calls++;
    if (offset == truncate_at) {
        truncate_at = -1;
        if (ftruncate(truncate_fd, offset + 64)) abort();
    }
    return pread(fd,dst,len,offset);
}
#include <sys/uio.h>
static ssize_t checked_preadv(int fd,const struct iovec* vectors,int count,off_t offset) {
    copy_calls++;
    if(offset==truncate_at) {truncate_at=-1;if(ftruncate(truncate_fd,offset+64)) abort();}
    return preadv(fd,vectors,count,offset);
}
#define preadv checked_preadv
#define pread checked_pread
#define fstat checked_fstat
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"
#undef fstat
static int child(const char* mode) {
    rbl_pixel_writer* w; rbl_pixel_reader* r = NULL;
    uint8_t px[64*64*4] = {7}; uint64_t no;
    rbl_frame f = {1,0,0,64,64};
    if (rbl_pixel_writer_create_temp("truncate",64,64,1,0,&w)) return 2;
    if (rbl_pixel_writer_write(w,px,sizeof px,NULL,0,64,64,0,1,&no)) return 3;
    truncate_fd = open(rbl_pixel_writer_path(w),O_WRONLY);
    if (truncate_fd < 0) return 4;
    if (!strcmp(mode,"open")) {
        armed = 1;
        if (rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r) == RBL_OK) return 5;
    } else {
        if (rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r)) return 6;
        armed = 1;
        if (rbl_pixel_reader_read_frame(r,&f,px,sizeof px,NULL,0,NULL) != RBL_MISS_GONE) return 7;
    }
    rbl_pixel_reader_close(r); close(truncate_fd); rbl_pixel_writer_close(w);
    return 0;
}
static int plane_test(int depth) {
    rbl_pixel_writer* w; rbl_pixel_reader* r;
    uint8_t px[128]={7}; float d[32]={2}; uint64_t no; int rc;
    rbl_frame f={1,0,0,8,4}; rbl_pixel_info info;
    if(rbl_pixel_writer_create_temp("plane",16,8,1,1,&w)) return 2;
    if(rbl_pixel_writer_write(w,px,sizeof px,d,32,8,4,0,1,&no)) return 3;
    if(rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r)) return 4;
    rbl_pixel_reader_info(r,&info);
    truncate_fd=open(rbl_pixel_writer_path(w),O_WRONLY);
    truncate_at=(off_t)(64+(depth?info.depth_offset:64));
    rc=rbl_pixel_reader_read_frame(r,&f,px,sizeof px,depth?d:NULL,depth?32:0,NULL);
    close(truncate_fd); truncate_fd=-1; rbl_pixel_reader_close(r);rbl_pixel_writer_close(w);
    printf("partial %s: rc=%d expected=%d\n",depth?"depth":"colour",rc,RBL_MISS_GONE);
    return rc==RBL_MISS_GONE?0:1;
}
static int order_test(void) {
    rbl_pixel_writer *a,*b; rbl_pixel_reader *ar,*br;
    uint8_t px[24]={7}; float depth[6]={2}; uint64_t no; int i,fail=0;
    rbl_frame old={5,2,0,2,2},first={1,1,0,3,2}; rbl_pixel_stats before,after; rbl_pixel_frame info;
    if(rbl_pixel_writer_create_temp("two-a",2,2,3,0,&a)||rbl_pixel_writer_create_temp("two-b",3,2,3,1,&b)) return 2;
    for(i=0;i<5;i++) if(rbl_pixel_writer_write(a,px,16,NULL,0,2,2,0,1,&no)) return 3;
    if(rbl_pixel_writer_write(b,px,24,depth,6,3,2,0,1,&no)) return 4;
    if(rbl_pixel_reader_open(rbl_pixel_writer_path(a),&ar)||rbl_pixel_reader_open(rbl_pixel_writer_path(b),&br)) return 5;
    fail |= rbl_pixel_reader_read_latest(ar,&old,px,sizeof px,NULL,0,NULL)!=RBL_OK;
    fail |= rbl_pixel_reader_read_latest(br,&old,px,sizeof px,NULL,0,NULL)==RBL_OK;
    fail |= rbl_pixel_reader_read_latest(br,&first,px,sizeof px,NULL,0,&info)!=RBL_OK || info.has_depth;
    fail |= rbl_pixel_reader_read_latest(br,&first,px,sizeof px,NULL,0,NULL)==RBL_OK;
    rbl_pixel_reader_stats(br,&before);
    fail |= before.stale!=1;
    fail |= rbl_pixel_reader_read_latest(br,NULL,px,sizeof px,NULL,0,NULL)!=RBL_ERR_ARGS;
    rbl_pixel_reader_stats(br,&after);fail |= memcmp(&before,&after,sizeof before)!=0;
    rbl_pixel_reader_close(ar);rbl_pixel_reader_close(br);rbl_pixel_writer_close(a);rbl_pixel_writer_close(b);
    printf("two-file presentation order: %s\n",fail?"FAIL":"ok");return fail;
}
static int gather_test(void) {
    rbl_pixel_writer* w; rbl_pixel_reader* r; uint64_t no;
    uint8_t px[128]={7};float depth[32]={2};rbl_frame f={1,0,0,8,4};int rc;
    if(rbl_pixel_writer_create_temp("gather",16,8,1,1,&w)) return 2;
    if(rbl_pixel_writer_write(w,px,128,depth,32,8,4,0,1,&no)) return 3;
    if(rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r)) return 4;
    copy_calls=0;rc=rbl_pixel_reader_read_frame(r,&f,px,128,depth,32,NULL);
    printf("cropped plane I/O: %u calls, rc=%d\n",copy_calls,rc);
    rbl_pixel_reader_close(r);rbl_pixel_writer_close(w);return rc!=RBL_OK||copy_calls!=4;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1000000.0; }
static int compare(const void* a,const void* b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static int bench(void) {
    const uint32_t sizes[][4] = {{1280,720,1280,720},{1920,1080,1920,1080},{3840,2160,3840,2160},{8192,8192,8192,8192},{16384,1,16384,1},{16384,4096,16384,4096},{1920,1080,1280,720},{3840,2160,1920,1080},{16384,4096,8192,4096},{16384,4096,1,4096}};
    size_t i;
    for (i=0;i<sizeof sizes/sizeof sizes[0];i++) {
        int dep;
        for(dep=0;dep<2;dep++) {
            uint32_t fw=sizes[i][0],fh=sizes[i][1],width=sizes[i][2],height=sizes[i][3]; size_t pixels=(size_t)width*height;
            uint8_t* px;
            if(getenv("RBL_BENCH_CROPPED_ONLY") && width==fw) continue;
            px=(uint8_t*)malloc(pixels*4); float* d=dep?(float*)malloc(pixels*4):NULL;
            rbl_pixel_writer* w; rbl_pixel_reader* r; uint64_t no; int j,n=pixels>10000000?5:30;
            rbl_frame f={1,0,0,width,height}; double times[30];
            if(!px || (dep&&!d)) return 2;
            memset(px,7,pixels*4); if(d) { size_t k; for(k=0;k<pixels;k++) d[k]=2; }
            if(rbl_pixel_writer_create_temp("bench",fw,fh,1,dep,&w)) return 3;
            if(rbl_pixel_writer_write(w,px,pixels*4,d,dep?pixels:0,width,height,0,1,&no)) return 4;
            if(rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r)) return 5;
            for(j=0;j<n+2;j++) {
                double start=now_ms();
                if(rbl_pixel_reader_read_frame(r,&f,px,pixels*4,d,dep?pixels:0,NULL)) return 6;
                if(j>=2) times[j-2]=now_ms()-start;
            }
            qsort(times,(size_t)n,sizeof(double),compare);
            printf("c,%u,%u,%u,%u,%s,%d,%.6f,%.6f,%.6f\n",fw,fh,width,height,dep?"true":"false",n,times[n/10],times[n/2],times[n*95/100]); fflush(stdout);
            rbl_pixel_reader_close(r); rbl_pixel_writer_close(w); free(px); free(d);
        }
    }
    return 0;
}
int main(int argc,char** argv) {
    if(argc>1 && !strcmp(argv[1],"bench")) return bench();
    if(argc>1 && !strcmp(argv[1],"colour")) return plane_test(0);
    if(argc>1 && !strcmp(argv[1],"depth")) return plane_test(1);
    if(argc>1 && !strcmp(argv[1],"order")) return order_test();
    if(argc>1 && !strcmp(argv[1],"gather")) return gather_test();
    if(argc>1) return child(argv[1]);
    if(plane_test(0)||plane_test(1)||order_test()||gather_test()) return 1;
    {
        const char* modes[]={"open","read"}; int i,failed=0;
        for(i=0;i<2;i++) {
            int status; pid_t pid; fflush(NULL); pid=fork();
            if(pid==0) _exit(child(modes[i]));
            if(pid<0 || waitpid(pid,&status,0)<0) return 2;
            printf("%s: status=%d signal=%d exit=%d\n",modes[i],status,WIFSIGNALED(status)?WTERMSIG(status):0,WIFEXITED(status)?WEXITSTATUS(status):-1);
            if(!WIFEXITED(status)||WEXITSTATUS(status)!=0) failed=1;
        }
        return failed;
    }
}

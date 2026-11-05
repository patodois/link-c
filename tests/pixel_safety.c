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
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1000000.0; }
static int compare(const void* a,const void* b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static int bench(void) {
    const uint32_t sizes[][2] = {{1280,720},{1920,1080},{3840,2160},{8192,8192},{16384,1},{16384,4096}};
    size_t i;
    for (i=0;i<sizeof sizes/sizeof sizes[0];i++) {
        int dep;
        for(dep=0;dep<2;dep++) {
            uint32_t width=sizes[i][0],height=sizes[i][1]; size_t pixels=(size_t)width*height;
            uint8_t* px=(uint8_t*)malloc(pixels*4); float* d=dep?(float*)malloc(pixels*4):NULL;
            rbl_pixel_writer* w; rbl_pixel_reader* r; uint64_t no; int j,n=pixels>10000000?5:30;
            rbl_frame f={1,0,0,width,height}; double times[30];
            if(!px || (dep&&!d)) return 2;
            memset(px,7,pixels*4); if(d) { size_t k; for(k=0;k<pixels;k++) d[k]=2; }
            if(rbl_pixel_writer_create_temp("bench",width,height,1,dep,&w)) return 3;
            if(rbl_pixel_writer_write(w,px,pixels*4,d,dep?pixels:0,width,height,0,1,&no)) return 4;
            if(rbl_pixel_reader_open(rbl_pixel_writer_path(w),&r)) return 5;
            for(j=0;j<n+2;j++) {
                double start=now_ms();
                if(rbl_pixel_reader_read_frame(r,&f,px,pixels*4,d,dep?pixels:0,NULL)) return 6;
                if(j>=2) times[j-2]=now_ms()-start;
            }
            qsort(times,(size_t)n,sizeof(double),compare);
            printf("c,%u,%u,%s,%d,%.6f,%.6f\n",width,height,dep?"true":"false",n,times[n/2],times[n*95/100]); fflush(stdout);
            rbl_pixel_reader_close(r); rbl_pixel_writer_close(w); free(px); free(d);
        }
    }
    return 0;
}
int main(int argc,char** argv) {
    if(argc>1 && !strcmp(argv[1],"bench")) return bench();
    if(argc>1) return child(argv[1]);
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

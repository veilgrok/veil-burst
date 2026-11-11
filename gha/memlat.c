// Pointer-chase memory latency: random cyclic permutation over 64-byte lines.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(){
  size_t sizes[]={16<<10,256<<10,2<<20,8<<20,32<<20,128<<20,512<<20,1024UL<<20};
  for(int s=0;s<8;s++){
    size_t n=sizes[s]/64; char*buf=aligned_alloc(64,n*64); size_t*idx=malloc(n*sizeof(size_t));
    for(size_t i=0;i<n;i++)idx[i]=i;
    uint64_t r=88172645463325252ULL;
    for(size_t i=n-1;i>0;i--){r^=r<<13;r^=r>>7;r^=r<<17;size_t j=r%i;size_t t=idx[i];idx[i]=idx[j];idx[j]=t;}
    for(size_t i=0;i<n;i++)*(void**)(buf+idx[i]*64)=buf+idx[(i+1)%n]*64;
    void**p=(void**)(buf+idx[0]*64); long iters=20000000;
    for(long i=0;i<n && i<1000000;i++)p=(void**)*p;
    double t=now(); for(long i=0;i<iters;i++)p=(void**)*p; double dt=now()-t;
    printf("LAT %zu %.2f %p\n",sizes[s]>>10,dt/iters*1e9,(void*)p);
    free(buf);free(idx);
  }
}

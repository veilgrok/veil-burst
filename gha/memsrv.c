// Remote page server for the uffd shim: holds the remote-backed part of guest RAM (zero-initialised).
// Protocol over TCP: 'G' u64 off -> 4096 B page ; 'P' u64 off + 4096 B -> store. Usage: memsrv PORT SIZE_MB
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
static int xfer(int fd, void *b, size_t n, int wr){ char *p=b; while(n){ ssize_t r = wr? write(fd,p,n): read(fd,p,n); if(r<=0) return -1; p+=r; n-=r;} return 0; }
int main(int c, char **v){ int port=atoi(v[1]); size_t sz=(size_t)atol(v[2])<<20; char *m=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); unsigned char *wr=calloc(sz/4096,1); unsigned long sent=0;
  int ls=socket(AF_INET,SOCK_STREAM,0),one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one); struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)}; bind(ls,(void*)&a,sizeof a); listen(ls,4);
  fprintf(stderr,"MEMSRV listening %d size=%zuMiB\n",port,sz>>20);
  for(;;){ int s=accept(ls,0,0); madvise(m,sz,MADV_DONTNEED); memset(wr,0,sz/4096); sent=0; /* fresh guest = fresh zeroed remote memory */ setsockopt(s,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one); unsigned long g=0,p=0; time_t last=time(0);
    for(;;){ char op; uint64_t off; if(xfer(s,&op,1,0)||xfer(s,&off,8,0)) break; if(off+4096>sz){ fprintf(stderr,"bad off\n"); break; }
      if(op=='G'){ if(xfer(s,m+off,4096,1)) break; g++; } else if(op=='Q'){ uint32_t n; if(xfer(s,&n,4,0)||n>64||off+(uint64_t)n*4096>sz) break; uint64_t mask=0; size_t pg=off/4096;
        for(uint32_t i=0;i<n;i++) if(wr[pg+i]) mask|=1ull<<i;  /* never-written pages are known zero: send only the bitmap bit */
        if(xfer(s,&off,8,1)||xfer(s,&n,4,1)||xfer(s,&mask,8,1)) break; int bad=0; for(uint32_t i=0;i<n;i++) if(mask>>i&1){ if(xfer(s,m+off+(size_t)i*4096,4096,1)){bad=1;break;} sent++; } if(bad) break; g+=n; }
      else if(op=='W'){ uint32_t n; if(xfer(s,&n,4,0)||n>64||off+(uint64_t)n*4096>sz) break; if(xfer(s,m+off,(size_t)n*4096,0)) break; for(uint32_t i=0;i<n;i++){ char *q=m+off+(size_t)i*4096; wr[off/4096+i]= !(q[0]==0 && !memcmp(q,q+1,4095)); } p+=n; } else if(op=='R'){ uint32_t n; if(xfer(s,&n,4,0)||off+(uint64_t)n*4096>sz) break; if(xfer(s,m+off,(size_t)n*4096,1)) break; g+=n; } else if(op=='P'){ if(xfer(s,m+off,4096,0)) break; wr[off/4096]=1; p++; } else break;
      if(time(0)-last>=5){ last=time(0); fprintf(stderr,"MEMSRV gets=%lu puts=%lu\n",g,p);} }
    fprintf(stderr,"MEMSRV client closed gets=%lu puts=%lu data_pages_sent=%lu\n",g,p,sent); close(s); } }

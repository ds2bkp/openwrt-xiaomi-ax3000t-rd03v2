/* SPDX-License-Identifier: ISC
 * QCA MCS netlink ABI follows mc_api.h (Qualcomm Innovation Center).
 * Maintain the bridge flood table on membership events, without polling.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

#define QCA_MC 23
#define BUFFER_SIZE 65536
#define GROUP_MAX 256
#define PORT_MAX 16
#define BRIDGE_MAX 8
enum { SET_ENABLE=0, SET_POLICY=2, SET_RETAG=4, SET_ROUTER_PORT=5,
       SET_CONVERT_ALL=8, SET_FLOOD=14, SET_EVENT_PID=15, GET_MDB=17,
       MDB_UPDATED=100 };
/* QCA leaves the filter at zero for IGMPv1/v2 and MLDv1 ASM reports. */
enum { FILTER_LEGACY_ASM=0, FILTER_INCLUDE=1, FILTER_EXCLUDE=2 };
struct mc_header {
 char name[16];
 uint32_t buf_len, offset, status, written, needed;
} __attribute__((packed));
struct mc_group { uint32_t protocol; unsigned char address[16]; };
struct mc_member {
 struct mc_group group;
 uint32_t port, sources;
 unsigned char source[64];
 uint32_t age;
 uint8_t filter, aged_out, mac[6];
} __attribute__((packed));
struct mc_flood {
 struct mc_group group;
 uint32_t count, port[PORT_MAX];
} __attribute__((packed));
_Static_assert(sizeof(struct mc_header)==36, "QCA header ABI");
_Static_assert(sizeof(struct mc_member)==104, "QCA MDB ABI");
_Static_assert(sizeof(struct mc_flood)==88, "QCA flood ABI");
static unsigned char txbuf[BUFFER_SIZE], rxbuf[BUFFER_SIZE], snapshot[BUFFER_SIZE];
static struct mc_flood flood[GROUP_MAX];
struct bridge_state {
 char name[IFNAMSIZ];
 struct mc_flood previous[GROUP_MAX];
 unsigned int previous_count, ifindex;
 bool event_pending, attached, previous_valid;
};
static struct bridge_state bridges[BRIDGE_MAX], *active;
static unsigned int bridge_count;
#define bridge (active->name)
#define previous (active->previous)
#define previous_count (active->previous_count)
#define pending (active->event_pending)

static uint32_t serial;
static volatile sig_atomic_t stopping, reconfigure;
static int sock=-1;
static unsigned int group_count;
static void on_signal(int sig) {
 if (sig==SIGHUP) reconfigure=1; else stopping=1;
}
static int64_t milliseconds(void) {
 struct timespec t;
 if (clock_gettime(CLOCK_MONOTONIC,&t)<0) return -1;
 return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int receive(int timeout, uint32_t wanted) {
 int64_t deadline=timeout<0?0:milliseconds()+timeout;
 struct pollfd pfd={.fd=sock,.events=POLLIN};
 for (;;) {
  if (timeout>=0) {
   int64_t left=deadline-milliseconds();
   if (left<=0) { errno=ETIMEDOUT; return -1; }
   timeout=left>INT_MAX?INT_MAX:(int)left;
  }
  int rc=poll(&pfd,1,timeout);
  if (rc<0) { if (errno==EINTR) return -2; return -1; }
  if (!rc) { errno=ETIMEDOUT; return -1; }
  struct sockaddr_nl from;
  socklen_t len=sizeof(from);
  ssize_t n=recvfrom(sock,rxbuf,sizeof(rxbuf),0,(struct sockaddr *)&from,&len);
  if (n<0) { if (errno==EINTR) continue; return -1; }
  if (from.nl_pid!=0 || n<(ssize_t)NLMSG_LENGTH(sizeof(struct mc_header))) continue;
  struct nlmsghdr *h=(void *)rxbuf;
  if (h->nlmsg_len>(unsigned int)n || h->nlmsg_len<NLMSG_LENGTH(sizeof(struct mc_header))) continue;
  struct mc_header *m=(void *)NLMSG_DATA(h);
  if (h->nlmsg_type==MDB_UPDATED) {
   for (unsigned int b=0;b<bridge_count;b++)
    if (!strncmp(m->name,bridges[b].name,sizeof(m->name))) bridges[b].event_pending=true;
   if (!wanted) return 0;
   continue;
  }
  if (wanted && h->nlmsg_seq==wanted) {
   if (strncmp(m->name,bridge,sizeof(m->name))) { errno=EPROTO; return -1; }
   return (int)n;
  }
 }
}
static int request(unsigned int type,const void *data,size_t size,size_t capacity) {
 size_t payload=size>capacity?size:capacity;
 if (payload>BUFFER_SIZE-NLMSG_LENGTH(sizeof(struct mc_header))) { errno=E2BIG; return -1; }
 size_t len=NLMSG_LENGTH(sizeof(struct mc_header)+payload);
 memset(txbuf,0,len);
 struct nlmsghdr *h=(void *)txbuf;
 struct mc_header *m=(void *)NLMSG_DATA(h);
 h->nlmsg_len=len;h->nlmsg_type=type;h->nlmsg_pid=getpid();h->nlmsg_seq=++serial;
 snprintf(m->name,sizeof(m->name),"%s",bridge);m->buf_len=payload;
 if (size) memcpy((unsigned char *)m+sizeof(*m),data,size);
 struct sockaddr_nl to={.nl_family=AF_NETLINK};
 if (sendto(sock,txbuf,len,0,(struct sockaddr *)&to,sizeof(to))!=(ssize_t)len) return -1;
 int n=receive(3000,serial);
 if (n<0) return -1;
 h=(void *)rxbuf;m=(void *)NLMSG_DATA(h);
 if (h->nlmsg_type!=type) { errno=EPROTO; return -1; }
 if (m->status) { errno=m->status==4?EOVERFLOW:EIO; return -1; }
 if (m->written>h->nlmsg_len-NLMSG_LENGTH(sizeof(*m))) { errno=EPROTO; return -1; }
 return n;
}
static int value(unsigned int type,uint32_t v) { return request(type,&v,sizeof(v),0); }
static int configure(void) {
 active->ifindex=if_nametoindex(bridge);
 if (!active->ifindex) { errno=ENODEV; return -1; }
 if (value(SET_ENABLE,1)<0) return -1;
 active->attached=true;
 /* Retain software forwarding until snooping has learned all listeners. */
 if (value(SET_POLICY,1)<0 || value(SET_CONVERT_ALL,0)<0) return -1;
 uint32_t retag[2]={0,0}, rp[2]={0,0};
 if (request(SET_RETAG,retag,sizeof(retag),0)<0 ||
     request(SET_ROUTER_PORT,rp,sizeof(rp),0)<0) return -1;
 previous_count=0;active->previous_valid=false;pending=true;
 return 0;
}
static int compare_group(const void *a,const void *b) {
 return memcmp(&((const struct mc_flood *)a)->group,&((const struct mc_flood *)b)->group,sizeof(struct mc_group));
}
static int compare_port(const void *a,const void *b) {
 uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;
 return (x>y)-(x<y);
}
static int collect(const void *data,size_t bytes) {
 if (bytes%sizeof(struct mc_member)) { errno=EPROTO; return -1; }
 memset(flood,0,sizeof(flood));group_count=0;
 for (size_t pos=0;pos<bytes;pos+=sizeof(struct mc_member)) {
  struct mc_member member;memcpy(&member,(const unsigned char *)data+pos,sizeof(member));
  if (!member.port || member.aged_out) continue;
  if (member.filter==FILTER_INCLUDE && !member.sources) continue;
  if (member.group.protocol!=htons(0x0800) && member.group.protocol!=htons(0x86dd)) continue;
  if (member.group.protocol==htons(0x0800)) memset(member.group.address+4,0,12);
  if (member.filter==FILTER_LEGACY_ASM) {
   /* Legacy reports have no source list; zero does not mean no listener. */
   if (member.sources) continue;
  } else if (member.filter!=FILTER_INCLUDE && member.filter!=FILTER_EXCLUDE) {
   continue;
  }
  unsigned int g;
  for (g=0;g<group_count;g++) if (!memcmp(&member.group,&flood[g].group,sizeof(member.group))) break;
  if (g==group_count) {
   if (group_count==GROUP_MAX) { errno=E2BIG; return -1; }
   flood[g].group=member.group;group_count++;
  }
  unsigned int j;
  for (j=0;j<flood[g].count;j++) if (flood[g].port[j]==member.port) break;
  if (j==flood[g].count) {
   if (flood[g].count==PORT_MAX) { errno=E2BIG; return -1; }
   flood[g].port[flood[g].count++]=member.port;
  }
 }
 for (unsigned int g=0;g<group_count;g++) qsort(flood[g].port,flood[g].count,sizeof(uint32_t),compare_port);
 qsort(flood,group_count,sizeof(*flood),compare_group);
 return 0;
}
static int update(bool dump) {
 int n=request(GET_MDB,NULL,0,BUFFER_SIZE-NLMSG_LENGTH(sizeof(struct mc_header)));
 if (n<0) return -1;
 struct mc_header *m=(void *)NLMSG_DATA((struct nlmsghdr *)rxbuf);
 size_t bytes=m->written;
 memcpy(snapshot,(unsigned char *)m+sizeof(*m),bytes);
 if (bytes%sizeof(struct mc_member)) { errno=EPROTO; return -1; }
 for (size_t pos=0;pos<bytes;pos+=sizeof(struct mc_member)) {
  struct mc_member *entry=(void *)(snapshot+pos);
  char port_name[IFNAMSIZ], path[128], master[256];
  if (!if_indextoname(entry->port,port_name)) { entry->port=0; continue; }
  snprintf(path,sizeof(path),"/sys/class/net/%s/master",port_name);
  ssize_t len=readlink(path,master,sizeof(master)-1);
  if (len<0) { entry->port=0; continue; }
  master[len]=0;
  char *name=strrchr(master,'/');
  if (!name || strcmp(name+1,bridge)) entry->port=0;
 }
 if (collect(snapshot,bytes)<0) return -1;
 if (dump) {
  printf("bridge=%s groups=%u listeners=%zu\n",bridge,group_count,bytes/sizeof(struct mc_member));
  for (unsigned int g=0;g<group_count;g++) {
   char addr[INET6_ADDRSTRLEN];
   inet_ntop(flood[g].group.protocol==htons(0x0800)?AF_INET:AF_INET6,flood[g].group.address,addr,sizeof(addr));
   printf("%s",addr);
   for (unsigned int j=0;j<flood[g].count;j++) {
    char name[IFNAMSIZ];
    printf(" %s",if_indextoname(flood[g].port[j],name)?name:"unknown");
   }
   puts("");
  }
  return 0;
 }
 if (active->previous_valid && group_count==previous_count && !memcmp(flood,previous,group_count*sizeof(*flood))) return 0;
 if (request(SET_FLOOD,flood,group_count*sizeof(*flood),0)<0) return -1;
 memcpy(previous,flood,group_count*sizeof(*flood));previous_count=group_count;active->previous_valid=true;
 syslog(LOG_INFO,"%s: updated %u multicast groups from membership event",bridge,group_count);
 return 0;
}
static int selftest(void) {
 struct mc_member members[5]={0};
 for (int i=0;i<5;i++) {
  members[i].group.protocol=htons(0x0800);
  inet_pton(AF_INET,"233.18.145.80",members[i].group.address);
  members[i].port=(i==1?4:12);members[i].filter=2;
 }
 members[3].aged_out=1;members[3].port=3;
 members[4].filter=1;members[4].sources=0;members[4].port=2;
 if (collect(members,sizeof(members))<0 || group_count!=1 || flood[0].count!=2 || flood[0].port[0]!=4 || flood[0].port[1]!=12) return 1;
 if (collect(members,sizeof(members)-1)==0) return 1;
 members[0].group.address[8]=42;
 if (collect(members,sizeof(members))<0 || group_count!=1) return 1;
 if (collect(NULL,0)<0 || group_count) return 1;
 struct mc_member many[GROUP_MAX+1];
 memset(many,0,sizeof(many));
 for (unsigned int i=0;i<GROUP_MAX+1;i++) {
  many[i].group.protocol=htons(0x0800);many[i].group.address[0]=233;
  many[i].group.address[2]=i>>8;many[i].group.address[3]=i;
  many[i].port=10;many[i].filter=2;
 }
 if (collect(many,sizeof(many))==0 || errno!=E2BIG) return 1;
 for (unsigned int i=0;i<PORT_MAX+1;i++) {
  many[i]=members[0];many[i].port=i+1;
 }
 if (collect(many,(PORT_MAX+1)*sizeof(*many))==0 || errno!=E2BIG) return 1;
 struct mc_member ip6={0};
 ip6.group.protocol=htons(0x86dd);ip6.port=2;ip6.filter=1;ip6.sources=1;
 inet_pton(AF_INET6,"ff3e::1234",ip6.group.address);
 if (collect(&ip6,sizeof(ip6))<0 || group_count!=1 || flood[0].count!=1) return 1;
 /* Regression: real QCA MDB records from the IPTV set-top box.
  * Valid legacy ASM entries must survive; empty v3 INCLUDE means leave.
  */
 struct mc_member legacy[8]={0};
 for (unsigned int i=0;i<8;i++) {
  legacy[i].group.protocol=htons(0x0800);
  inet_pton(AF_INET,"233.14.202.11",legacy[i].group.address);
  legacy[i].port=i+4;legacy[i].filter=FILTER_LEGACY_ASM;
 }
 legacy[1].port=legacy[0].port; /* duplicate receiver on the same port */
 legacy[2].filter=FILTER_INCLUDE; /* empty include / v3 leave */
 legacy[3].aged_out=1;
 legacy[4].filter=99; /* invalid filter */
 legacy[5].sources=1; /* impossible legacy source list */
 legacy[6].port=0;
 legacy[7].filter=FILTER_INCLUDE;legacy[7].sources=1;
 if (collect(legacy,sizeof(legacy))<0 || group_count!=1 ||
     flood[0].count!=2 || flood[0].port[0]!=4 || flood[0].port[1]!=11) return 1;
 legacy[0].group.protocol=htons(0x86dd);
 inet_pton(AF_INET6,"ff3e::1234",legacy[0].group.address);
 if (collect(legacy,sizeof(*legacy))<0 || group_count!=1 || flood[0].count!=1) return 1;
 puts("PASS: ABI, dedup, IPv4 padding, IPv6 include, legacy IGMP/MLD ASM, empty INCLUDE leave, aged/invalid legacy entries, malformed/empty table, group/port overflow");
 return 0;
}
int main(int argc,char **argv) {
 if (argc==2 && !strcmp(argv[1],"--selftest")) return selftest();
 bool dump=argc==3 && !strcmp(argv[1],"--status");
 if ((!dump && (argc<2 || argc>BRIDGE_MAX+1))) {
  fprintf(stderr,"usage: %s [--status] BRIDGE [BRIDGE...]\n",argv[0]);return 2;
 }
 for (int i=dump?2:1;i<argc;i++) {
  const char *name=argv[i];
  if (!*name || strlen(name)>=IFNAMSIZ || strchr(name,'/')) {
   fprintf(stderr,"invalid bridge name\n");return 2;
  }
  unsigned int j;
  for (j=0;j<bridge_count;j++) if (!strcmp(bridges[j].name,name)) break;
  if (j==bridge_count) strcpy(bridges[bridge_count++].name,name);
 }
 active=&bridges[0];
 openlog("nss-mcastd",LOG_PID,LOG_DAEMON);
 struct sockaddr_nl local={.nl_family=AF_NETLINK,.nl_pid=getpid()};
 sock=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,QCA_MC);
 if (sock<0 || bind(sock,(struct sockaddr *)&local,sizeof(local))<0) { perror("QCA MCS netlink");return 1; }
 if (dump) {
  int rc=update(true);
  if (rc<0) perror("QCA MCS status");
  close(sock);return rc<0;
 }
 int lock=open("/var/run/nss-mcastd.lock",O_CREAT|O_CLOEXEC|O_RDWR,0600);
 if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)<0) { perror("nss-mcastd lock");return 1; }
 struct sigaction sa={.sa_handler=on_signal};sigemptyset(&sa.sa_mask);
 sigaction(SIGTERM,&sa,NULL);sigaction(SIGINT,&sa,NULL);sigaction(SIGHUP,&sa,NULL);
 int rc=1;
 for (unsigned int b=0;b<bridge_count;b++) {
  active=&bridges[b];
  if (configure()<0) { syslog(LOG_ERR,"configure %s: %m",bridge);goto done; }
 }
 if (value(SET_EVENT_PID,getpid())<0) goto done;
 syslog(LOG_INFO,"snooping %u bridges; waiting for membership events",bridge_count);
 while (!stopping) {
  if (reconfigure) { errno=EAGAIN; break; }
  bool dirty=false;
  for (unsigned int b=0;b<bridge_count;b++) {
   active=&bridges[b];
   if (!pending) continue;
   pending=false;
   if (if_nametoindex(bridge)!=active->ifindex || update(false)<0) {
    syslog(LOG_ERR,"membership update %s: %m",bridge);goto done;
   }
  }
  for (unsigned int b=0;b<bridge_count;b++) dirty|=bridges[b].event_pending;
  if (!dirty && receive(-1,0)<0 && errno!=EINTR && !stopping) goto done;
 }
 rc=stopping?0:1;
done:
 for (unsigned int b=0;b<bridge_count;b++) {
  active=&bridges[b];
  if (active->attached) {
   request(SET_FLOOD,NULL,0,0);
   value(SET_ENABLE,0);
  }
 }
 value(SET_EVENT_PID,UINT32_MAX);
 close(sock);close(lock);closelog();return rc;
}

#include "status_server.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Socket;
#define BAD INVALID_SOCKET
#define close_socket closesocket
static int blocked(void) { return WSAGetLastError()==WSAEWOULDBLOCK; }
static int nonblock(Socket s) { u_long mode=1; return ioctlsocket(s,FIONBIO,&mode)==0; }
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int Socket;
#define BAD (-1)
#define close_socket close
static int blocked(void) { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR; }
static int nonblock(Socket s) { return fcntl(s,F_SETFL,O_NONBLOCK)==0; }
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CLIENTS 8
#define RESPONSE 8192
typedef struct {
    Socket fd;
    Uint32 since;
    size_t used, length, sent;
    char request[2048], response[RESPONSE];
} Client;
struct StatusServer { Socket listener; Client clients[CLIENTS]; };
void status_server_close(StatusServer *s)
{
    if(!s) return;
    for(int i=0;i<CLIENTS;i++) if(s->clients[i].fd!=BAD) close_socket(s->clients[i].fd);
    if(s->listener!=BAD) close_socket(s->listener);
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}
StatusServer *status_server_open(const char *ip,int port)
{
#ifdef _WIN32
    WSADATA data;
    if(WSAStartup(MAKEWORD(2,2),&data)) return NULL;
#endif
    StatusServer *s=calloc(1,sizeof(*s));
    if(!s) {
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    s->listener=BAD;
    for(int i=0;i<CLIENTS;i++) s->clients[i].fd=BAD;
    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET; addr.sin_port=htons((unsigned short)port);
    if(!ip || port<1 || port>65535 || inet_pton(AF_INET,ip,&addr.sin_addr)!=1) goto fail;
    s->listener=socket(AF_INET,SOCK_STREAM,0);
    if(s->listener==BAD || !nonblock(s->listener)) goto fail;
    int one=1;
#ifdef _WIN32
    setsockopt(s->listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&one,sizeof(one));
#else
    setsockopt(s->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#endif
    if(bind(s->listener,(struct sockaddr *)&addr,sizeof(addr)) || listen(s->listener,CLIENTS)) goto fail;
    fprintf(stderr,"Status API: http://%s:%d/status (read-only)\n",ip,port);
    return s;
fail:
    fprintf(stderr,"Cannot start status API on %s:%d (check IPv4 address, port and whether it is in use)\n",ip?ip:"",port);
    status_server_close(s); return NULL;
}
static void reply(Client *c,int code,const char *reason,const char *body)
{
    int n=snprintf(c->response,sizeof(c->response),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s",code,reason,strlen(body),body);
    c->length=n>0 && n<(int)sizeof(c->response)?(size_t)n:0;
}
void status_server_poll(StatusServer *s,StatusJSON json,void *opaque)
{
    if(!s) return;
    /* At most eight accepts and one read/write per client per UI iteration. */
    for(int i=0;i<CLIENTS;i++) {
        if(s->clients[i].fd!=BAD) continue;
        Socket fd=accept(s->listener,NULL,NULL);
        if(fd==BAD) break;
        if(!nonblock(fd)) { close_socket(fd); continue; }
        s->clients[i]=(Client){.fd=fd,.since=SDL_GetTicks()};
    }
    for(int i=0;i<CLIENTS;i++) {
        Client *c=&s->clients[i];
        if(c->fd==BAD) continue;
        if(SDL_GetTicks()-c->since>2000) goto drop;
        if(!c->length) {
            int n=recv(c->fd,c->request+c->used,(int)(sizeof(c->request)-1-c->used),0);
            if(n==0 || (n<0 && !blocked())) goto drop;
            if(n<0) continue;
            c->used+=(size_t)n; c->request[c->used]=0;
            if(strstr(c->request,"\r\n\r\n")) {
                char method[16],path[256],version[16];
                if(sscanf(c->request,"%15s %255s %15s",method,path,version)!=3 ||
                   (strcmp(version,"HTTP/1.0") && strcmp(version,"HTTP/1.1")))
                    reply(c,400,"Bad Request","{\"error\":\"bad request\"}\n");
                else if(strcmp(method,"GET")) reply(c,405,"Method Not Allowed","{\"error\":\"GET only\"}\n");
                else if(strcmp(path,"/status")) reply(c,404,"Not Found","{\"error\":\"use /status\"}\n");
                else {
                    char body[RESPONSE-512];
                    int size=json(opaque,body,sizeof(body));
                    if(size<0 || size>=(int)sizeof(body)) reply(c,500,"Internal Server Error","{\"error\":\"status unavailable\"}\n");
                    else reply(c,200,"OK",body);
                }
            } else if(c->used==sizeof(c->request)-1) reply(c,431,"Request Header Fields Too Large","{\"error\":\"request too large\"}\n");
        }
        if(c->length) {
#ifdef _WIN32
            int flags=0;
#else
            int flags=MSG_NOSIGNAL;
#endif
            int n=send(c->fd,c->response+c->sent,(int)(c->length-c->sent),flags);
            if(n<0) { if(blocked()) continue; goto drop; }
            c->sent+=(size_t)n;
            if(!n || c->sent==c->length) goto drop;
        }
        continue;
drop:
        close_socket(c->fd); c->fd=BAD;
    }
}

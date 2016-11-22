#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <sys/epoll.h>
#include <pthread.h>

#define IP "127.0.0.1"
#define PORT 3000
#define MAX_CLIENT 1024
#define MAX_DATA 1024
#define MAX_EVENTS 50

static struct termios term_old;
struct epoll_event ev, events[MAX_EVENTS];
int client_sock, nfds, epoll_fd;
pthread_mutex_t mutx;

void initTermios(void);
void resetTermios(void);

int launch_chat(void);
int launch_clients(int num_client);
int launch_server(void);
int get_server_status(void);
 
int
main(int argc, char *argv[])
{
    int ret = -1;
    int num_client;

    if ((argc != 2) && (argc != 3)) {
usage:  fprintf(stderr, "usage: %s a|m|s|c num_client\n", argv[0]);
        goto leave;
    }
    if ((strlen(argv[1]) != 1))
        goto usage;
    switch (argv[1][0]) {
      case 'a': if (argc != 3)
                    goto usage;
                if (sscanf(argv[2], "%d", &num_client) != 1)
                    goto usage;
                // Launch Automatic Clients
                ret = launch_clients(num_client);
                break;
      case 's': // Launch Server
                ret = launch_server();
                break;
      case 'm': // Read Server Status
                ret = get_server_status();
                break;
      case 'c': // Start_Interactive Chatting Session
                ret = launch_chat();
                break;
      default:
                goto usage;
    }
leave:
    return ret;
}

int
launch_chat(void)
{
    int clientSock;
    struct sockaddr_in serverAddr;
    fd_set rfds, wfds, efds;
    int ret = -1;
    char rdata[MAX_DATA];
    int i = 1;
    struct timeval tm;

    if ((ret = clientSock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        goto leave;
    }
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(IP);
    serverAddr.sin_port = htons(PORT);

    if ((ret = connect(clientSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)))) {
        perror("connect");
        goto leave1;
    }
    printf("[CLIENT] Connected to %s\n", inet_ntoa(*(struct in_addr *)&serverAddr.sin_addr));

    initTermios();

    // start select version of chatting ...
    i = 1;
    ioctl(0, FIONBIO, (unsigned long *)&i);
    if ((ret = ioctl(clientSock, FIONBIO, (unsigned long *)&i))) {
        perror("ioctlsocket");
        goto leave1;
    }

    tm.tv_sec = 0; tm.tv_usec = 1000;
    while (1) {
        FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
        //FD_SET(clientSock, &wfds);
        FD_SET(clientSock, &rfds);
        FD_SET(clientSock, &efds);
        FD_SET(0, &rfds); //키보드 입력용

        if ((ret = select(clientSock + 1, &rfds, &wfds, &efds, &tm)) < 0) {
            perror("select");
            goto leave1;
        } else if (!ret)	// nothing happened within tm
            continue;
        if (FD_ISSET(clientSock, &efds)) {
            printf("Connection closed\n");
            goto leave1;
        }
        if (FD_ISSET(clientSock, &rfds)) {
            if (!(ret = recv(clientSock, rdata, MAX_DATA, 0))) {
                printf("Connection closed by remote host\n");
                goto leave1;
            } else if (ret > 0) {
                for (i = 0; i < ret; i++) {
                    printf("%c", rdata[i]);
                }
                fflush(stdout);
            } else
                break;
        }
        if (FD_ISSET(0, &rfds)) { //키보드 입력 데이터를 전송
            int ch = getchar();
            if ((ret = send(clientSock, &ch, 1, 0)) < 0)
                goto leave1;
        }
    }
leave1:
    resetTermios();
    close(clientSock);
leave:
    return -1;
}

int
launch_server(void)
{
    int serverSock, acceptedSock;
    struct sockaddr_in Addr, client_Addr;
    socklen_t AddrSize = sizeof(Addr);
    socklen_t C_AddrSize = sizeof(client_Addr);
    char data[MAX_DATA], *p;
    int ret, count, i = 1;
    pthread_t thread;

    /*if(pthread_mutex_init(&mutx,NULL)){
        perror("mutex init error");
        goto leave;
    } */

    if ((ret = serverSock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        goto leave;
    }

    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (void *)&i, sizeof(i));

    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = INADDR_ANY;
    Addr.sin_port = htons(PORT);
    if ((ret = bind(serverSock, (struct sockaddr *)&Addr,sizeof(Addr)))) {
        perror("bind");
        goto error;
    }

    if ((ret = listen(serverSock, 1))) {
        perror("listen");
        goto error;
    }
    // epoll 이벤트 설정 및 활성화
    if (epoll_fd = epoll_create(10) < 0) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN; 
    ev.data.fd = serverSock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serverSock, &ev) == -1) {
        perror("epoll_ctl: serverSock");
        exit(EXIT_FAILURE);
    } 
    
    if ((acceptedSock = accept(serverSock, (struct sockaddr*)&Addr, &AddrSize)) < 0) {
        perror("accept");
        ret = -1;
        goto error;
    }

    printf("[SERVER] Connected to %s\n", inet_ntoa(*(struct in_addr *)&Addr.sin_addr));
    //close(serverSock);

    while (1) {
        //epoll event 대기 (클라이언트의 반응(접속, send) 대기)
        if ((nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1)) == -1) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }/*
        if (!(ret = count = recv(acceptedSock, data, MAX_DATA, 0))) {
            fprintf(stderr, "Connect Closed by Client\n");
            break;
        }
        if (ret < 0) {
            perror("recv");
            break;
        }*/
        //printf("[%d]", count); fflush(stdout);
        for (i = 0; i < nfds; i++){ //epoll 이벤트 받은 만큼 반복
            printf("%c", data[i]);
            if(events[i].data.fd == serverSock){ //이벤트 들어온 소켓이 연결 요청일 때
                client_sock = accept(serverSock, (struct sockaddr *) &client_Addr, &C_AddrSize);
                if(client_sock == -1){
                    perror("accept");
                    exit(EXIT_FAILURE);
                }
                setnonblocking(client_sock); //non-blocking mode set
                ev.events = EPOLLIN;
                ev.data.fd = client_sock;
                if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev) == -1){
                    perror("epoll_ctl: client_sock");
                    exit(EXIT_FAILURE);
                }
            } else{
                do_use_fd(events[i].data.fd);
            }
            
        }
        fflush(stdout);
        p = data;
        while (count) {
            if ((ret = send(acceptedSock, p, count, 0)) < 0) {
                perror("send");
                break;
            }
            count -= ret;
            p = p + ret;
        }
    }

    close(acceptedSock);
error:
    close(serverSock);
leave:
    return ret;
}

int
launch_clients(int num_client)
{
    return 0;
}

int
get_server_status(void)
{
    return 0;
}

/* Initialize new terminal i/o settings */
void
initTermios(void) 
{
    struct termios term_new;

    tcgetattr(0, &term_old); /* grab old terminal i/o settings */
    term_new = term_old; /* make new settings same as old settings */
    term_new.c_lflag &= ~ICANON; /* disable buffered i/o */
    term_new.c_lflag &= ~ECHO;   /* set no echo mode */
    tcsetattr(0, TCSANOW, &term_new); /* use these new terminal i/o settings now */
}

/* Restore old terminal i/o settings */
void
resetTermios(void) 
{
    tcsetattr(0, TCSANOW, &term_old);
}

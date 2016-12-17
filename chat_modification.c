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
#define DELIM "\n"

static struct termios term_old;
struct epoll_event ev, events[MAX_EVENTS];
int nfds, epoll_fd;
pthread_mutex_t mutx;
int client_number = 0; //연결된 client 갯수 저장 변수
int client_socket[MAX_CLIENT]; //연결된 모든 client에 메세지 전송을 위한 client 소켓 정보 변수
char p[3] = {'$', '%'};

void initTermios(void);
void resetTermios(void);

int launch_chat(void);
int launch_clients(int num_client);
int launch_server(void);
int get_server_status(void);

void *client_connection(void *arg);
void server_send_message(char *message, int len);
void *client_send_message(void *arg);
void *recv_message(void *arg);

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

int setnonblocking(int fd)
{
    int flags;

    
#if defined(O_NONBLOCK)
    
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}     

int
launch_server(void)
{
    int serverSock, clientSock;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t C_AddrSize = sizeof(clientAddr);
    socklen_t S_AddrSize = sizeof(serverAddr);
    int ret, count = 0, i = 1;
    struct timeval before, after;
    int duration;
    pthread_t thread;
    void *thread_return;

    gettimeofday(&before, NULL);

    if(pthread_mutex_init(&mutx,NULL)){
        perror("mutex init error");
        goto leave;
    } 

    if ((ret = serverSock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        goto leave;
    }

    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (void *)&i, sizeof(i));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);
    
    if ((ret = bind(serverSock, (struct sockaddr *)&serverAddr,sizeof(serverAddr)))) {
        perror("bind");
        goto error;
    }

    if ((ret = listen(serverSock, 1))) {
        perror("listen");
        goto error;
    }
    // epoll 이벤트 설정 및 활성화
    if (epoll_fd = epoll_create(20) < 0) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN; 
    ev.data.fd = serverSock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serverSock, &ev) == -1) {
        perror("epoll_ctl: serverSock");
        exit(EXIT_FAILURE);
    } 

    printf("[SERVER] Connected to %s\n", inet_ntoa(*(struct in_addr *)&serverAddr.sin_addr));
    //close(serverSock);

    while (1) {
        //epoll event 대기 (클라이언트의 반응(접속, send) 대기)
        if ((nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1)) == -1) {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }
        //이벤트 발생 수 만큼 반복
        for (i = 0; i < nfds; i++){
            //이벤트가 발생한 소켓이 서버 소켓이라면 연결 소켓을 생성한다.
            if(events[i].data.fd == serverSock){
                if ((clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &C_AddrSize)) < 0){
                perror("accept");
                ret = -1;
                goto error;
                }
                count++;
                if(count == 20){
                    server_send_message(p,1); //client에 '$' 전송
                }
                setnonblocking(clientSock);
                ev.events = EPOLLIN || EPOLLET;
                ev.data.fd = clientSock;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, clientSock, &ev) == -1) {
                    perror("epoll_ctl: clientSock");
                    exit(EXIT_FAILURE);
                }
            }
            //이벤트가 발생한 소켓이 연결 소켓이라면 데이터를 읽어온다.
            else{
                pthread_mutex_lock(&mutx);
                client_socket[client_number++] = clientSock;
                pthread_mutex_unlock(&mutx);
                pthread_create(&thread, NULL, client_connection, (void *)clientSock);
                printf("New client connected");
            }
        }
    }

    pthread_join(thread, &thread_return);
    pthread_mutex_destroy(&mutx);

    gettimeofday(&after, NULL);

    duration = (after.tv_sec - before.tv_sec) * 1000000 + (after.tv_usec - before.tv_usec);
    printf("Processing time = %d.%06d sec\n", duration / 1000000, duration % 1000000);

    //close(clientSock);
error:
    close(serverSock);
leave:
    return ret;
}

void *client_connection(void *arg)
{
    int clientSock = (int)arg;
    int str_len = 0;
    char message[MAX_DATA];
    int i = 0, count = 0;

    //수신된 메세지의 길이가 0이거나 %를 수신받는 경우에는 client의 연결이 종료된다.
    while(1){
        if((str_len = read(clientSock, message, sizeof(message))) != 0) 
            break;
        if(message[str_len-1] == '@'){
            count++;
            if(count == 20){ //모든 client로부터 '@'를 수신하면
                server_send_message((p+1), 1); //모든 client에 '%' 송신
                break;
            }
        }
        else
            server_send_message(message, str_len); //수신된 메세지 모든 client에 전송
    }
        pthread_mutex_lock(&mutx);
        for(i=0;i<client_number;i++){
            if(clientSock == client_socket[i]){
                for(;i<client_number-1;i++)
                    client_socket[i] = client_socket[i+1];
                break;
            }
        }
        client_number--;
        pthread_mutex_unlock(&mutx);
        
        close(clientSock);
        return 0;
}

void server_send_message(char *message, int len)
{
    int i = 0;

    pthread_mutex_lock(&mutx);
    for(i=0;i<client_number;i++){
        write(client_socket[i],message,len);
    }
    pthread_mutex_unlock(&mutx);
}

int
launch_clients(int num_client)
{
    char message[MAX_DATA];
    int clientSock;
    struct sockaddr_in serverAddr;
    pthread_t send_thread, recv_thread;
    
    void *thread_return;

    if ((clientSock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(IP);
    serverAddr.sin_port = htons(PORT);

    if ((connect(clientSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)))) {
        perror("connect");
        exit(1);
    }
    printf("[CLIENT] Connected to %s\n", inet_ntoa(*(struct in_addr *)&serverAddr.sin_addr));

    initTermios();

    //메세지 전송 & 수신 thread 생성과 호출
    pthread_create(&send_thread, NULL, client_send_message, (void *)clientSock);
    pthread_create(&recv_thread, NULL, recv_message, (void *)clientSock);
    
    //thread가 종료될 때까지 대기
    pthread_join(send_thread, &thread_return);
    pthread_join(recv_thread, &thread_return);

    close(clientSock);
    return 0;
}

void *client_send_message(void *arg)
{
    FILE *file;
    char *buff, *token, *ptr[2];
    long f_size;
    size_t tmp;
    char p[3] = {'\n', '@'};

    int socket = (int)arg;
    if(file = fopen("/home/pi/adv-sys-programming/tmp/file_", "r") == NULL){
        perror("fopen");
        exit(1);
    }
    //파일의 크기를 f_size에 저장한다.
    fseek(file, 0, SEEK_END); 
    f_size = ftell(file);
    rewind(file);
    //버퍼의 메모리를 할당한다.
    buff = (char *)malloc(sizeof(char)*f_size);
    if (buff == NULL){
        fputs ("error, buff_malloc\n", stderr);
        free(buff);
        return 1;
    }
    //파일을 읽어온다.
    tmp = fread(buff, f_size, 1, file);
    //파일을 파싱한다.
    token = strtok_r(buff, DELIM, &ptr[0]);

    while(token != NULL){
        //'$'을 받으면 메세지 전송
        write(socket, token, strlen(token));
        write(socket, p, 1);
        token = strtok_r(NULL, DELIM, &ptr[0]);
    }
    //입력할 값이 NULL이 아닐때까지 - NULL이면 '@' 송신
    write(socket, (p+1), 1);
    close(file);
}

void *recv_message(void *arg)
{
    FILE *fout;
    char filename[100];

    int socket = (int)arg;
    char message[MAX_DATA];
    int str_len;
    int thread_return;

    sprintf(filename, "fout_%d.txt", arg);
    fout = fopen(filename, "a+");

    while(1){
        if(str_len = read(socket, message, MAX_DATA-1) == -1){
            thread_return = 1;
            return (void *)thread_return;
        }
        message[str_len] = 0; //수신된 메세지의 마지막에 NULL을 지정한다.
        fputs(message, stdout);
        fwrite(message, str_len, 1, fout);
    }
    close(fout);
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

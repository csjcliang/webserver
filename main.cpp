#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "noa_timer.h"
#include "http_conn.h"
#include "locker.h"
#include "sql_connection_pool.h"
#include "threadpool.h"

#define MAX_FD 65536            // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5              // 超时时间 

// http_conn中定义
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

// 设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 信号处理函数，仅发送信号值给主循环，不做对应逻辑处理
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    // 将信号值从管道写端写入
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// 为信号设置处理函数
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);  // 在信号处理函数中屏蔽所有信号
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 调用任务处理函数tick()处理链表中的定时器，接着重新定时以不断触发SIGALRM信号
void timer_handler() {
    timer_lst.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活跃连接在socket上的注册事件，并关闭
void cb_func(client_timer *user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    // info
    // printf("A nonactive connection closed.\n");
}

int main(int argc, char* argv[]) {
    if (argc <= 1) {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    // 注册信号捕捉，因为一端断开后另一端还继续写数据会产生SIGPIPE信号，默认会终止进程，这里选择忽略
    addsig(SIGPIPE, SIG_IGN);

    // 数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "rjgc", "rjgc123", "WebServerDB", 3306, 8);

    // 线程池
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>(connPool);
    } catch (...) {
        return 1;
    }
    printf("Thread pool created.\n");

    // 保存客户端信息
    http_conn* users = new http_conn[MAX_FD];
    // 初始化数据库读取表
    users->initmysql_result(connPool);

    // lfd
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定监听
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    ret = listen(listenfd, 5);

    // 创建epoll对象
    int epollfd = epoll_create(5);
    // 创建监听的事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    
    // 将要监听事件的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 定时器相关
    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);
    bool stop_server = false;
    // 
    client_timer* users_timer = new client_timer[MAX_FD];
    // 是否超时
    bool timeout = false;
    // 每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);


    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        // 因为是阻塞的，有可能因为信号捕捉后不阻塞返回-1，产生EINTR
        if ((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历epoll返回的事件数组
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            // lfd，表示有客户端连接进来了
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                // cfd
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                // 连接数已满
                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }
                // 初始化客户信息，放进数组
                users[connfd].init(connfd, client_address);

                // 初始化client_timer数据
                // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                // 回调函数
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                // 设置超时时间为5倍TIMESLOT
                timer->expire = cur + 5 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
                // info
                // printf("A connection comes.\n");
            }
            // 检测错误事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 服务器端关闭连接，移除对应的定时器
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer) {
                    timer_lst.del_timer(timer);
                }
                // users[sockfd].close_conn();
            }
            // 管道读端对应文件描述符发生读事件，则处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                // 从管道读端读出信号值，成功返回字节数
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                } else if (ret == 0) {
                    continue;
                } else {
                    for (int i = 0; i < ret; ++i) {
                        switch (signals[i]) {
                            // 超时
                            case SIGALRM: {
                                timeout = true;
                                // info
                                // printf("SIGALRM reached!\n");
                                break;
                            }
                            // 进程被杀死
                            case SIGTERM: {
                                stop_server = true;
                                // info
                                // printf("SIGTERM reached!\n");
                            }
                        }
                    }
                }
            }
            // cfd上有读事件
            else if (events[i].events & EPOLLIN) {
                // 该连接对应的定时器
                util_timer* timer = users_timer[sockfd].timer;
                if (users[sockfd].read()) {
                    // 若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    // 更新定时器在链表的位置
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 5 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    // 服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    // users[sockfd].close_conn();
                }
            } 
            // cfd上有写事件
            else if (events[i].events & EPOLLOUT) {
                util_timer* timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) {
                    // 更新定时器在链表的位置
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    // 服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                    // users[sockfd].close_conn();
                }
            }
        }
        // 收到信号并不是立马处理，完成读写事件后，再进行处理
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }
    
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}

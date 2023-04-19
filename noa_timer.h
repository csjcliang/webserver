#ifndef NOA_TIMER_H
#define NOA_TIMER_H

#include <time.h>
#include <netinet/in.h>

class util_timer;
// 封装用户连接信息和定时器
struct client_timer {
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

// 定时器(双向链表)
class util_timer {
   public:
    util_timer() : prev(NULL), next(NULL) {}

   public:
    // 超时时间 
    time_t expire;
    // 回调函数
    void (*cb_func)(client_timer*);
    // 用户连接信息
    client_timer* user_data;
    util_timer* prev;
    util_timer* next;
};

// 定时器容器类
class sort_timer_lst {
   public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    // 销毁链表
    ~sort_timer_lst() {
        util_timer* tmp = head;
        while (tmp) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    // 添加定时器
    void add_timer(util_timer* timer) {
        if (!timer) {
            return;
        }
        if (!head) {
            head = tail = timer;
            return;
        }
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }
    // 调整定时器
    void adjust_timer(util_timer* timer) {
        if (!timer) {
            return;
        }
        util_timer* tmp = timer->next;
        if (!tmp || (timer->expire < tmp->expire)) {
            return;
        }
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        } else {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    // 删除定时器
    void del_timer(util_timer* timer) {
        if (!timer) {
            return;
        }
        if ((timer == head) && (timer == tail)) {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if (timer == tail) {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    // 定时任务处理函数，删除超时的定时器
    void tick() {
        if (!head) {
            return;
        }
        // printf( "timer tick\n" );
        time_t cur = time(NULL);
        util_timer* tmp = head;
        // 遍历链表
        while (tmp) {
            // 当前时间小于定时器的超时时间，后面的定时器也没有到期
            if (cur < tmp->expire) {
                break;
            }
            // 当前定时器到期，则调用回调函数删除非活动连接在socket上的注册事件，并关闭
            tmp->cb_func(tmp->user_data);
            head = tmp->next;
            if (head) {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

   private:
    // 将定时器插入链表内部
    void add_timer(util_timer* timer, util_timer* lst_head) {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        while (tmp) {
            if (timer->expire < tmp->expire) {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        if (!tmp) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

   private:
    util_timer* head;
    util_timer* tail;
};

#endif

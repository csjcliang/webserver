#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <cstdio>
#include <exception>
#include <list>
#include "locker.h"
#include "sql_connection_pool.h"

// 线程池类
template <typename T>
class threadpool {
   public:
    // connPool: 数据库连接池
    // thread_number：线程池中线程数量，
    // max_requests：请求队列中最多允许的、等待处理的请求的数量
    threadpool(connection_pool* connPool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

   private:
    // 工作线程运行的函数，不断从请求队列中取出任务并执行
    static void* worker(void* arg);
    void run();

   private:
    // 线程数
    int m_thread_number;

    // 线程池数组
    pthread_t* m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量
    int m_max_requests;

    // 请求队列，双向链表
    std::list<T*> m_workqueue;

    // 请求队列的互斥锁
    locker m_queuelocker;

    // 信号量表示是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;

    // 数据库
    connection_pool* m_connPool;
};

// 构造函数
template <typename T>
threadpool<T>::threadpool(connection_pool* connPool, int thread_number, int max_requests)
    : m_thread_number(thread_number),
      m_max_requests(max_requests),
      m_stop(false),
      m_threads(NULL),
      m_connPool(connPool) {
    
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建thread_number个线程
    for (int i = 0; i < thread_number; ++i) {
        // printf("create the %dth thread\n", i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        // 设置为detach，自动回收
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数
template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

// 向请求队列增加一个任务
template <typename T>
bool threadpool<T>::append(T* request) {
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 工作线程函数
template <typename T>
void* threadpool<T>::worker(void* arg) {
    // 将参数强转为线程池类(当前线程池)，调用成员方法
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

// 从请求队列取出任务并执行
template <typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }

        connectionRAII mysqlcon(&request->mysql, m_connPool);
        request->process();
    }
}

#endif

#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <error.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <list>
#include <string>
#include "locker.h"

using namespace std;

class connection_pool {
   public:
    MYSQL* GetConnection();               // 获取数据库连接
    bool ReleaseConnection(MYSQL* conn);  // 释放连接
    int GetFreeConn();                    // 获取空闲连接数
    void DestroyPool();                   // 销毁所有连接

    // 单例模式(局部静态变量懒汉模式)
    static connection_pool* GetInstance();

    void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn);

    connection_pool();
    ~connection_pool();

   private:
    unsigned int MaxConn;   // 最大连接数
    unsigned int CurConn;   // 当前已使用的连接数
    unsigned int FreeConn;  // 当前空闲的连接数

   private:
    locker lock;            // 互斥锁
    list<MYSQL*> connList;  // 连接池，链表形式
    sem reserve;            // 信号量初始化为数据库的连接总数

   private:
    string url;           // 主机地址
    string Port;          // 数据库端口号
    string User;          // 登陆数据库用户名
    string PassWord;      // 登陆数据库密码
    string DatabaseName;  // 使用数据库名
};

// RAII，在构造函数中申请分配资源，在析构函数中释放资源
class connectionRAII {
   public:
    connectionRAII(MYSQL** con, connection_pool* connPool);
    ~connectionRAII();

   private:
    MYSQL* conRAII;             // 连接
    connection_pool* poolRAII;  // 连接池
};

#endif

#include "sql_connection_pool.h"
#include <mysql/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <list>
#include <string>

using namespace std;

connection_pool::connection_pool() {
    this->CurConn = 0;
    this->FreeConn = 0;
}

// 获取数据库实例
connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn) {
    // 初始化数据库信息
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DatabaseName = DBName;

    lock.lock();
    // 创建MaxConn条数据库连接
    for (int i = 0; i < MaxConn; i++) {
        MYSQL* con = NULL;
        con = mysql_init(con);

        if (con == NULL) {
            cout << "Error:" << mysql_error(con);
            exit(1);
        }
        // 创建mysql连接
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
        if (con == NULL) {
            cout << "Error: " << mysql_error(con);
            exit(1);
        }
        // 将连接放进连接池，更新空闲连接数
        connList.push_back(con);
        ++FreeConn;
    }
    // 信号量初始化为连接总数
    reserve = sem(FreeConn);

    this->MaxConn = FreeConn;

    lock.unlock();
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_pool::GetConnection() {
    MYSQL* con = NULL;
    if (0 == connList.size())
        return NULL;
    // 取出连接，信号量-1，为0则阻塞
    reserve.wait();

    lock.lock();

    con = connList.front();
    connList.pop_front();

    --FreeConn;
    ++CurConn;

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL* con) {
    if (NULL == con)
        return false;

    lock.lock();

    connList.push_back(con);
    ++FreeConn;
    --CurConn;

    lock.unlock();
    // 释放连接，信号量+1
    reserve.post();
    return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        list<MYSQL*>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            MYSQL* con = *it;
            mysql_close(con);
        }
        CurConn = 0;
        FreeConn = 0;
        connList.clear();

        lock.unlock();
    }

    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn() {
    return this->FreeConn;
}

// 析构函数
connection_pool::~connection_pool() {
    DestroyPool();
}

// RAII 在构造函数中申请分配资源，在析构函数中释放资源
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
    *SQL = connPool->GetConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}
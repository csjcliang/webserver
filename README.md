# Linux系统下的web服务器
## 主要实现
1. 基于线程池及epoll多路复用，Proactor事件处理模式；
2. 支持客户端的HTTP请求(GET/POST)；
3. 定时器模块，对非活跃的客户连接进行定时清理；
4. 登录、注册模块，客户数据存储于MySQL数据库中；
5. 简单的前端页面设计（登录、注册页面）。

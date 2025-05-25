# Common

Common 是 WebServer 项目所有程序的公用组件。

该组件库用于支持 WebServer 多进程架构中各功能进程之间的通信、状态同步与数据结构操作，具有轻量级、通用性强的特点。目前包含以下几个核心模块：

---

## 1. 链表模块（list.h）

提供一个基于 Linux 内核风格的双向循环链表实现，适用于各种队列管理场景。

* 结点结构为 `LIST_NODE`，可嵌入任意结构体中；
* 支持头插、尾插、删除、判空、遍历等操作；
* 所有操作线程无关，需配合互斥锁手动加锁使用。

---

## 2. 文件描述符传输模块（transfer\_fd.h）

封装了基于 `UNIX 域 socket` 的文件描述符传输功能，支持独立进程之间通过 socketpair 实现高效通信。

### 核心概念：

* 使用 `socketpair(AF_UNIX, SOCK_DGRAM, 0, ...)` 创建本地通信通道；
* 支持将已打开的 `fd`（如 TCP 连接）发送给另一个进程；
* 使用 `sendmsg()` + `SCM_RIGHTS` 实现真正的文件描述符跨进程共享；
* 每个 `SOCKETPAIR` 通信对是全双工的，进程双方可互相读写；
* 支持状态消息结构体 `StatusMessage` 的统一封装和收发。

### 常用函数：

* `socketpair_create()`：创建通信对；
* `fd_send()` / `fd_recv()`：发送/接收文件描述符；
* `send_status_message()` / `recv_status_message()`：进程状态心跳通信。

---

## 3. 事件通知模块（event\_notify.h）

封装 Linux 的 `eventfd` 机制，用于跨线程或进程之间的轻量级事件通知。

* `eventfd_create()` 创建事件描述符；
* `eventfd_notify()` 发送通知（内部写入 1）；
* `eventfd_wait()` 等待并消费通知。

**适用于：**

* 有限资源控制、计数器；
* 线程或进程之间的事件同步。

---

## 关于 UNIX 域 socket 与 socketpair 的使用设计

本项目采用 `UNIX domain socket`（本地 socket）与 `socketpair` 实现进程间通信，有以下优势：

1. **全双工通信**：`socketpair` 天然支持双向通信，每一对 socket 可以像管道一样互相收发数据；
2. **传输文件描述符**：通过控制消息可传递任意有效文件描述符，真正实现“零拷贝”地共享 TCP 连接等资源；
3. **进程隔离但资源共享**：不同进程不必共享内存，仅通过 socket 通信完成任务交接；
4. **连接重用机制**：守护进程持有一端 socket，可在功能进程崩溃时重新 fork 新进程并复用该通信通道，增强系统可靠性；
5. **通信抽象简洁**：接口统一，使用方式简明，降低了跨进程通信复杂度。

---

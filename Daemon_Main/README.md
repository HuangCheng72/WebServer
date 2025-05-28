# Daemon_Main

Daemon_Main 是 WebServer 的作为守护进程和主进程的程序。

## 1. 守护进程与功能进程之间的关系示意（ASCII图）

```
                +------------------------+
                |     Daemon Process     | 
                |  (Monitors & Manages)  |
                +------------------------+
                        |
              +---------+---------+
              |                   |
     +----------------+    +------------------+
     | Listener Proc  |    |   Manager Proc   |
     | (Accepts TCP   |    | (Manages Worker  |
     | Connections)   |    |  Pool & Task     |
     +----------------+    |  Distribution)   |
              |            +------------------+
              |                   |
              +-------------------+--------------------+
                                      |
                              +------------------------+
                              |  Worker Process (Pool) |
                              | (Handles HTTP Requests)|
                              +------------------------+

```



---

## 2. 具体设计

### 2.1 socketpair

**概念简介：**
 `socketpair` 是 Linux 提供的一种基于 UNIX 域的全双工通信机制，常用于进程之间的数据交换。调用 `socketpair(AF_UNIX, SOCK_DGRAM, 0, sv)` 会返回两个文件描述符 `sv[0]` 和 `sv[1]`，它们之间可以互相读写，功能对等。

与匿名管道不同，`socketpair` 支持双向通信，并允许通过 `sendmsg()` 和 `SCM_RIGHTS` 携带额外的文件描述符进行传输。

**思路说明：**
 守护进程负责预先创建好所有通信用的 `SOCKETPAIR`，并在启动功能进程（Listener、Manager）时，把其中一端通过 `fd_send()` 传给目标进程。守护进程永远持有另一端，确保通信通道不会因子进程崩溃而丢失。

```
              [Daemon]
              /      \
        fd_send     fd_send
          ↓            ↓
     [Listener] <===> [Manager]
        ↑                ↑
     sock1            sock2
```

其中：

- `daemon_listener` 和 `daemon_manager`：分别用于守护进程与 Listener、Manager 的心跳通信；
- `listener_send_manager_recv` 和 `listener_recv_manager_send`：用于 Listener 和 Manager 之间的双向数据传输。

**项目实现：**

```c
socketpair_create(&daemon_listener);
socketpair_create(&daemon_manager);
socketpair_create(&listener_send_manager_recv);
socketpair_create(&listener_recv_manager_send);

// fork前发给目标进程
fd_send(daemon_sock, listener_send_manager_recv.sock1);
fd_send(daemon_sock, listener_recv_manager_send.sock1);
```

------

### 2.2 fork+exec

**概念简介：**
 `fork()` 会复制当前进程，生成子进程；`exec()` 系列函数用于在子进程中加载并执行一个新的可执行文件，替换当前进程空间。二者配合可实现“守护进程 fork 子进程 + exec 启动实际服务进程”的经典进程管理模式。

**思路说明：**
 守护进程调用 `fork()` 创建子进程，在子进程中使用 `execl()` 启动目标程序（如 Listener、Manager），并通过参数传递通信 socket 的文件描述符编号。

这种模式下，守护进程只负责资源配置与监控，不介入功能逻辑。

```
  [Daemon Process]
        |
      fork()
        |
        +--> [Child: ./Listener]
        |         |
        |     execl()
        |         |
        |  Receives: 
        |    - daemon_listener.sock2
        |    - listener_send_manager_recv.sock1
        |    - listener_recv_manager_send.sock1
        |
        +--> [Child: ./Manager]
                  |
              execl()
                  |
          Receives:
            - daemon_manager.sock2
            - listener_recv_manager_send.sock2
            - listener_send_manager_recv.sock2

```

**项目实现：**

```c
char fd_str[32];
snprintf(fd_str, sizeof(fd_str), "%d", listener_sock);
execl("./Listener", "WebServer_Listener", fd_str, NULL);
```

父进程部分：

```c
pid = fork();
if (pid == 0) {
    // 执行子进程
    ...
} else {
    // 更新状态
    listener_status.pid = pid;
}
```

------

### 2.3 心跳机制

**概念简介：**
 功能进程定期通过 socket 向守护进程发送心跳包（结构体 `StatusMessage`），守护进程接收并更新时间戳，从而判断进程是否健康。如果超过一段时间未收到心跳包，守护进程将发出超时告警或触发重启。

**思路说明：**
 每个功能进程对应一个 `socketpair` 通道和一个监听线程，接收状态包后记录 `last_heartbeat_time`。

```c
recv_status_message(daemon_sock, &tmp);
clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
```

超时检查逻辑每 200ms 执行一次，若心跳超时达到一定次数（如 3 次），则视为功能进程已失效，触发重启标志位。

```
[Daemon Monitor Thread]
        |
  detects: module == -10
        |
  send shutdown message
        |
  waitpid (graceful exit attempt)
        |
  if timeout → kill(SIGKILL)
        |
     fork()
        |
     exec() → restart Listener or Manager
        |
  fd_send() the same SOCKETPAIR again

```

**项目实现：**

```c
long delta_ms = (now.tv_sec - last_heartbeat_time.tv_sec) * 1000 +
                (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1000000;

if (delta_ms > heartbeat_timeout_ms) {
    if (++timeout_warn_count == max_warn_times) {
        listener_status.module = -10; // 触发重启标志
    }
}
```

------

### 2.4 出错重启

**概念简介：**
 当功能进程因为崩溃、卡死或通信中断被守护进程判定为不可用时，守护进程负责主动重启。包括发送关闭信号（自定义指令或 kill），调用 `waitpid` 等待退出，然后重新 fork + exec 启动。

**思路说明：**
 守护进程持有所有通信资源，因此可以在功能进程崩溃后复用原有通信 socketpair，再次传给新进程，避免重新建立通道。

```
[Daemon Monitor Thread]
       |
 detect module == -10
       |
 shutdown → waitpid → kill(SIGKILL)
       |
 fork + exec to restart process
       |
 fd_send the same socketpair again

```

**项目实现：**

```c
// 主动关闭旧进程
send_status_message(daemon_sock, &shutdown_msg);
waitpid(listener_status.pid, NULL, WNOHANG);
kill(listener_status.pid, SIGKILL);

// fork 启动新进程
fd_send(daemon_sock, listener_send_manager_recv.sock1);
fd_send(daemon_sock, listener_recv_manager_send.sock1);
pid = fork();
execl("./Listener", "WebServer_Listener", fd_str, NULL);
```
## 3. 守护进程的工作流

```
                                +-----------------------+
                                |     Daemon Process    |
                                |  (Monitors & Manages) |
                                +-----------------------+
                                          |
            +-----------------------------+-----------------------------+
            |                             |                             |
+-------------------+        +------------------------+       +--------------------+
|  Listener Proc    |        |   Manager Proc         |       |    Worker Proc     |
|  (TCP Listener)   |        |  (Task & Worker Mgmt)  |       |  (Handles Requests)|
+-------------------+        +------------------------+       +--------------------+
            |                             |                             |
    +-------------------+          +------------------+          +------------------+
    |Listener-to-Manager| <------> |Manager-to-Worker | <------> |Worker-to-Listener|
    |Socket Pair        |          | Socket Pair      |          | Socket Pair      |
    +-------------------+          +------------------+          +------------------+
            |                             |
            +-----------------------------+
                        |
            +----------------------------+
            |   Heartbeat Monitoring     |
            |    (Daemon Monitors State) |
            +----------------------------+
                        |
            +----------------------------+
            |  Restart Process if Timeout |
            +----------------------------+

```

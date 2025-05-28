# WebServer
一个简单的Linux网络服务器

## 1. 项目各模块间关系
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

## 2. 各模块工作流

### 2.1 守护进程
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

### 2.2 监听进程

```
                                  +--------------------------+
                                  |      Listener Process    |
                                  |  (Accepts TCP Connections)|
                                  +--------------------------+
                                              |
                +-----------------------------+-------------------------------+
                |                                                         |
   +------------------------+                                +-------------------------+
   | Accept Queue           |                                | Release Queue           |
   | (Waiting for TCP Conn) |                                | (Waiting to Send to Mgr)|
   +------------------------+                                +-------------------------+
                |                                                         |
        +---------------+                                            +---------------------+
        | Add to Queue  |                                            | Remove from Queue   |
        | (accept())    |                                            | (send to Manager)   |
        +---------------+                                            +---------------------+
                |                                                         |
        +---------------+                                    +------------------------+
        | Select Loop   |                                    | Send to Manager        |
        | (Check for    |                                    | (Send completed        |
        | Ready FD)     |                                    | TCP connections        |
        +---------------+                                    | via Socket)            |
                |                                            +------------------------+
                +------------------------------------------------------+
                                |
                    +----------------------------+
                    |  Heartbeat to Daemon       |
                    |  (Send Status Info)        |
                    +----------------------------+
                                |
                +-----------------------------+
                |     Handle Shutdown Signal  |
                | (Exit on Shutdown Request)  |
                +-----------------------------+

```

### 2.3 管理进程

```
                                      +-------------------------+
                                      |    Manager Process      |
                                      |  (Manages Worker Pool)  |
                                      +-------------------------+
                                                   |
             +-------------------------------------+-----------------------------------+
             |                                     |                                   |
    +-----------------------+             +---------------------------+    +-----------------------+
    |  Wait Queue           |             |  Worker Pool              |    |  Socket Communication |
    | (TCP Connections)     |             |  (Manage Workers)         |    | (Recv from Listener)  |
    +-----------------------+             +---------------------------+    +-----------------------+
             |                                     |                                   |
   +---------------------------+         +----------------------------+         +--------------------------+
   | Add TCP Connections       |         |  Add/Remove Workers        |         | Receive Worker Heartbeats|
   | to Wait Queue             |         |  Monitor Worker Pool       |         | & Task Information       |
   +---------------------------+         +----------------------------+         +--------------------------+
             |                                     |                                   |
    +---------------------------+       +-----------------------------+        +-------------------------+
    |  Send to Worker           |       |  Manage Worker Tasks        |        | Send Heartbeat to Daemon|
    |  (Distribute Tasks)       |       | (Assign Tasks to Workers)   |        |  (Monitor & Report)     |
    +---------------------------+       +-----------------------------+        +-------------------------+
             |                                     |                                   |
    +---------------------------+         +-----------------------------+        +-------------------------+
    | Monitor & Adjust Workers  |         | Worker Shutdown/Creation    |        | Manage Worker Lifecycle |
    | Based on Load/Workload    |         |  (Scale Worker Pool)        |        | (Terminate Dead Workers)|
    +---------------------------+         +-----------------------------+        +-------------------------+
                                                   |
                                     +-----------------------------+
                                     |     Send Status to Daemon   |
                                     |  (Heartbeat & Process Info) |
                                     +-----------------------------+
                                                   |
                                  +-------------------------------+
                                  |    Handle Shutdown Request    |
                                  |    (Graceful Shutdown)        |
                                  +-------------------------------+

```

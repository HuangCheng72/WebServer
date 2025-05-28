# Manager

Manager 是 WebServer 的工作进程池管理程序。

## 1.管理进程工作流
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

## 2. 进程池设计

### 2.1 数据结构

```
  +---------------------------------------------------+
  |                  WorkerPool                       |
  |---------------------------------------------------|
  | count           | Number of workers in the pool   |
  | min_count       | Minimum number of workers       |
  | size            | Maximum number of workers       |
  | max_workload    | Max workload per worker         |
  |---------------------------------------------------|
  | List of Workers | Dynamically growing list        |
  |---------------------------------------------------|
  | WorkerInfo (Linked List of Workers)               |
  +---------------------------------------------------+
                        |
                        |
                        v
                +--------------------------+
                |  WorkerInfo              |
                |--------------------------|
                | pid                      | (PID of the worker process)
                | monitor                  | (Responsible thread for worker)
                | workload                 | (Current workload assigned to worker)
                | flags                    | (Status flags: 0=running, 1=kill, -1=killed)
                | wait                     | (Queue of tasks waiting for this worker)
                | manager_worker           | (Manager to Worker communication socket)
                | manager_send_worker_recv | (Communication socket for task assignment)
                +--------------------------+
                        |
                        v
                +---------------------+
                |     Wait Queue      | (Queue of TCP connections to process)
                +---------------------+
                        |
                        v
             +---------------------------+
             |  SocketInfo               |
             |---------------------------|
             | client_socket             | (TCP connection descriptor)
             +---------------------------+

```

### 2.2 进程池工作过程

```
+---------------------+                    +---------------------+
|   Listener Process  |                    |    Manager Process  |
+---------------------+                    +---------------------+
|                     |                    |                     |
| 1. Listens for TCP  |                    | 1. Receives task    |
|    connections      |                    |    requests from    |
|    (client sockets) | ---------------->  |    Listener         |
|                     |                    |    Process          |
+---------------------+                    |                     |
                                           | 2. Assigns task to  |
                                           |    Worker Pool      |
                                           |                     |
                                           | 3. Monitors worker  |
                                           |    health and task  |
                                           |    completion       |
                                           |                     |
                                           +---------------------+
                                                        |
                                                        v
                                           +-----------------------------------+
                                           |  Worker Pool (Manages Workers)    |
                                           +-----------------------------------+
                                           |                                   |
                                           | 4. Worker pool size dynamically   |
                                           |    adjusts based on workload      |
                                           |    (creates/removes workers)      |
                                           |                                   |
                                           | 5. Tasks are assigned to          |
                                           |    available workers              |
                                           +-----------------------------------+
                                                        |
                                                        v
                                           +------------------------------+
                                           |  Worker Process (Worker Info)|
                                           +------------------------------+
                                           |                              |
                                           | 6. Receives tasks            |
                                           |    from Manager              |
                                           |    via socket                |
                                           | 7. Process tasks             |
                                           |    (e.g., handle TCP         |
                                           |    connections)              |
                                           | 8. Sends heartbeat           |
                                           |    to Manager                |
                                           | 9. Reports status            |
                                           |    to Manager                |
                                           +------------------------------+
                                                        |
                                                        v
                                           +-----------------------------------+
                                           |  Manager Process (Monitors)       |
                                           +-----------------------------------+
                                           |                                   |
                                           | 10. Monitors worker status        |
                                           |     and health (via heartbeat)    |
                                           | 11. Scales worker pool based      |
                                           |     on workload and worker status |
                                           | 12. Destroys failed workers       |
                                           |     (if needed)                   |
                                           +-----------------------------------+
                                                        |
                                                        v
                                           +----------------------------------+
                                           |  Worker Pool (Manages Workers)   |
                                           +----------------------------------+
                                           |                                  |
                                           | 13. Releases resources if worker |
                                           |     is failed or not needed      |
                                           |     (removes worker from pool)   |
                                           +----------------------------------+

```

# Listener

Listener 是 WebServer 的监听进程，接受 TCP 连接，以及对 TCP 连接进行管理。



## 1. TCP连接池

### 1.1 数据结构

```
+--------------------------------------------------------------------------------------+
|                                  TCP Connection Pool                                 |
+--------------------------------------------------------------------------------------+
| count:                   current number of active connections                        |
| size:                    allocated capacity of the heap                              |
| array_tcpinfo[]:         pointer array used as a min-heap (1-based, [0] is sentinel) |
| hash_socket_tcpinfo[]:   direct-mapping table: socket_fd → index in array_tcpinfo    |
| head:                    doubly linked list of all active tcpinfo nodes              |
| epoll_fd:                epoll instance file descriptor                              |
| mutex:                   mutex lock for thread safety                                |
+--------------------------------------------------------------------------------------+

                +----------------------------+
                |      Min-Heap Array        |
                |     array_tcpinfo[]        |
                +----------------------------+
                | [0] = DummyNode (timeout=0)|  ← sentinel node
                | [1] = tcpinfo* (timeout=12)|  ← root of min-heap
                | [2] = tcpinfo* (timeout=20)|
                | [3] = tcpinfo* (timeout=30)|
                |   ...                      |
                +----------------------------+

                    ↓ Each entry points to →  tcpinfo structs

     +-------------------------------------------------------------------------------------+
     |                                    tcpinfo                                          |
     +-------------------------------------------------------------------------------------+
     | index:         index in array_tcpinfo[]                                             |
     | client_socket: TCP socket fd                                                        |
     | timeout:       seconds left before forced close                                     |
     | node:          LIST_NODE (for doubly linked list)                                   |
     +-------------------------------------------------------------------------------------+

    Doubly Linked List Traversal:

        head (DummyNode)
            ↓
       +-----------+ <-> +-----------+ <-> +-----------+ <-> ...
       | tcpinfo 1 |     | tcpinfo 2 |     | tcpinfo 3 |
       +-----------+     +-----------+     +-----------+

    Used for timeout scanning (every 1 second): timeout-- if > 0

    Heap used to remove earliest timeout connection efficiently.

        ↓ fast lookup when closing or removing connection

+----------------------------------------------+
| hash_socket_tcpinfo[socket_fd] = heap index  |
+----------------------------------------------+

    Provides O(1) mapping from socket to tcpinfo in heap.
    Ensures no need to search entire heap during epoll removal.


```



### 1.2 工作流

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


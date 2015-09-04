# Socket-MPI
参照MPI实现的并行框架的多节点通信函数(实现原理：通信双方双向认证。使用了socket编程，引入了epoll来实现IO的多路复用，
打破传统socket编程的保存的客户端数目上限FD_SETSIZE宏（2048）的限制。
常用的打破FD_SETSIZE宏限制的方法有：修改宏的大小，会带来网络效率的下降；Apache服务器的PPC多进程连接的方案，但是进程间需要同步操作；多线程方案，线程同步虽然比进程同步开销小，但是浪费效率；selec/poll方案依然还有限制最大连接数，调用次数多会导致性能线性下降；
epoll则打破这个限制，用空间换时间，IO多路复用，相对比来说效率较高)。
实现了几个简单的通信函数（send/recv，barrier，broadcast，reduction）


文件夹内文件信息：

MPI.c   实现的fork出新进程的程序，编译命令: gcc -std=c99 -o main MPI.c

WAVE.h  自定义的分布式通信框架头文件
WAVE.c  自定义的分布式通信框架的实现

n.c     测试分布式通信框架的代码

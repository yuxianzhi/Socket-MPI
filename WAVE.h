/* Author: cos
*  date: 2015.8
*  参照MPI实现的分布式计算框架(实现原理：通信双方双向认证。使用了socket编程，引入了epoll来实现IO的多路复用，
*  打破传统socket编程的保存的客户端数目上限FD_SETSIZE宏（2048）的限制。
*  常用的打破FD_SETSIZE宏限制的方法有：修改宏的大小，会带来网络效率的下降；Apache服务器的PPC多进程连接的方案，但是进程间需要同步操作；
*  多线程方案，线程同步虽然比进程同步开销小，但是浪费效率；selec/poll方案依然还有限制最大连接数，调用次数多会导致性能线性下降；
*  epoll则打破这个限制，用空间换时间，IO多路复用，相对比来说效率较高)
*  实现了几个简单的通信函数（send/recv，barrier，broadcast，reduction）
*  
*/
#ifndef __WAVE__
#define __WAVE__


//任意发送方
#define WAVE_ANY_SOURCE -100

//字符
#define WAVE_CHAR 1000
//有符号整形
#define WAVE_INT 1001
//无符号整形
#define WAVE_UINT 1002
//有符号长整形
#define WAVE_LONG 1003
//无符号长整形
#define WAVE_ULONG 1004
//单精度浮点数
#define WAVE_FLOAT 1005
//双精度浮点数
#define WAVE_DOUBLE 1006

//广播标签
#define WAVE_TAG_BCAST 2001
//归约标签
#define WAVE_TAG_REDUC 2002
//同步标签
#define WAVE_TAG_BARRIER 2003
//任意标签
#define WAVE_TAG_ANY 2004

//归约加运算
#define WAVE_ADD 3001
//归约乘运算
#define WAVE_MUL 3002
//归约且运算
#define WAVE_AND 3003
//归约或运算
#define WAVE_OR 3004

//客户端原始状态
#define WAVE_CLIENT_ORI 4000
//客户端初始化状态
#define WAVE_CLIENT_START 4001
//客户端第一次读状态
#define WAVE_CLIENT_READ1 4002
//客户端第二次读状态
#define WAVE_CLIENT_READ2 4003
//客户端第一次写状态
#define WAVE_CLIENT_WRITE1 4004
//客户端第二次写状态
#define WAVE_CLIENT_WRITE2 4005
//客户端结束
#define WAVE_CLIENT_END 4006

//通信状态信息
typedef struct WAVE_sendRecvInfo{
	int source; //发送源
	int tag;     //发送标签
}WAVE_Status;




//初始化函数
int WAVE_Init(int& argc, char *argv[]);

//收尾函数
int WAVE_Finalize();

//返回所有进程数目
int WAVE_Size(int *size);

//返回进程全局编号
int WAVE_Rank(int *rank);

//发送消息函数
int WAVE_Send(void *message, size_t length, int type, int dest, size_t tag);

//接收消息函数
int WAVE_Recv(void *message, size_t length, int type, int src, size_t tag, WAVE_Status *status);

//普通广播函数
int WAVE_Bcast(void *orimessage, int length, int type, int root);

//同步函数
int WAVE_Barrier();

//普通归约函数
int WAVE_Reduce(void *orimessage, void *recvbuf, size_t length, int type, int op, int root);

#endif














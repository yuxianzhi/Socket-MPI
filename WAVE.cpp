/* Author: cos
*  Date: 2015.8
*  参照MPI实现的分布式计算框架(实现原理：通信双方双向认证。使用了socket编程，引入了epoll来实现IO的多路复用，
*  打破传统socket编程的保存的客户端数目上限FD_SETSIZE宏（2048）的限制。
*  常用的打破FD_SETSIZE宏限制的方法有：修改宏的大小，会带来网络效率的下降；Apache服务器的PPC多进程连接的方案，但是进程间需要同步操作；
*  多线程方案，线程同步虽然比进程同步开销小，但是浪费效率；selec/poll方案依然还有限制最大连接数，调用次数多会导致性能线性下降；
*  epoll则打破这个限制，用空间换时间，IO多路复用，相对比来说效率较高)
*  实现了几个简单的通信函数（send/recv，barrier，broadcast，reduction）
*  
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include "WAVE.h"

//打开输出调试信息
//#define DEBUG
//打开输出节点IP和端口信息
//#define INFO

//检查系统位数,__LP64__是linux预定义的64位宏
#ifdef __LP64__
	#define WAVE_WORD_64
#else
	#define WAVE_WORD_32
#endif
#define WAVE_BAG_SIZE 16384   //IP头所能表示的窗口大小2^16/4（实验结果）




//保存每一个节点绑定的IP和端口信息
struct WAVE_node_info{
	char *ip;
	unsigned int port;
};

//自定义的epoll_data
struct WAVE_epoll_data{
	int fd;   //客户端套接字标示符
	int state; //客户端状态标示符
}; 


//进程全局编号
int WAVE_RANK=-1;
//所有进程数目
int WAVE_SIZE=-1;
//所有进程的IP和端口信息
struct WAVE_node_info *WAVE_NODES = NULL;
//认证消息长度
int WAVE_ACMESSAGE_LENGTH = 60;
//等待时钟
int WAVE_CLOCK = 1;
//服务器套接口
int WAVE_SERVER_SOCKET; 
//服务器监听队列长度
int WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH = 1000; 
//服务器epoll events数量
int WAVE_EPOLL_EVENTS_NUM = 1000; 
//服务器网络地址            
struct sockaddr_in WAVE_SERVER_IN; 


//返回数组中某一值所对应的下标
int WAVE_IndexOfValue(int *array, int length, int value){
	int i;
	for(i=0; i<length; i++){
		if(array[i] == value)
			return i;
	}
	return -1;
}


//将字符串恢复成其他种类的信息
int WAVE_CharToOther(char *message, size_t length, int type, void *newmessage){
	size_t i;
	char *message1;
	char *message2;
	switch (type)
	{
		//有符号整形
		case WAVE_INT:
		{
			int *temp = (int *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = (int)strtol(message1, &message2, 10);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		//无符号整形
		case WAVE_UINT:
		{
			unsigned int *temp = (unsigned int *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = (unsigned int)strtoul(message1, &message2, 10);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		//有符号长整形
		case WAVE_LONG:
		{
			long *temp = (long *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = strtol(message1, &message2, 10);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		//无符号长整形
		case WAVE_ULONG:
		{
			unsigned long *temp = (unsigned long *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = strtoul(message1, &message2, 10);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		//单精度浮点数
		case WAVE_FLOAT:
		{
			float *temp = (float *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = (float)strtod(message1, &message2);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		//双精度浮点数
		case WAVE_DOUBLE:
		{
			double *temp = (double *)newmessage;
			message1  = message;
			for(i=0; i<length; i++){
				temp[i] = strtod(message1, &message2);
				message1 = (char *)(message2 + 1);
			}
			break;
		}
		default :
		{
			return 0;
		}
	}
	return 1;
}


//设置端口连接方式是非阻塞
int WAVE_SetNonBlocking(int sockid)
 {
    int opts;
    opts = fcntl(sockid,F_GETFL);
    if(opts < 0)
    {
        perror("fcntl(sockid, GETFL)");
        return 0;
    }
    opts = opts|O_NONBLOCK;
    if(fcntl(sockid, F_SETFL, opts)<0)
    {
        perror("fcntl(sockid,SETFL, opts)");
        return 0;
    }
    return 1;
}


//关闭连接的客户端端口和epoll事件
void WAVE_CloseAndDisable(int sockid, struct epoll_event event)
{
    struct WAVE_epoll_data *temp = (struct WAVE_epoll_data *)(event.data.ptr);
    close(sockid);
	close((*temp).fd);
    (*temp).fd = -1;
    (*temp).state = WAVE_CLIENT_END;
}


//检查数据类型是否合法
int WAVE_IsTypeLegal(int type){
	if(type==WAVE_CHAR||type==WAVE_INT||type==WAVE_UINT||type==WAVE_LONG||type==WAVE_ULONG||type==WAVE_FLOAT||type==WAVE_DOUBLE)
		return 1;
	else
		return 0;
}


//检查运算符是否合法
int WAVE_IsOpLegal(int op){
	if(op==WAVE_ADD||op==WAVE_MUL||op==WAVE_AND||op==WAVE_OR)
		return 1;
	else
		return 0;
}


//自定义的归约运算符
int WAVE_Operation(void *base, void *message, size_t length, int type, int op){
	size_t i;
	switch (type)
	{
		case WAVE_CHAR :
		{
			char *temp1 = (char *)base;
			char *temp2 = (char *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			break;
		}
		//有符号整形
		case WAVE_INT:
		{
			int *temp1 = (int *)base;
			int *temp2 = (int *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			break;
		}
		//无符号整形
		case WAVE_UINT:
		{
			unsigned int *temp1 = (unsigned int *)base;
			unsigned int *temp2 = (unsigned int *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			break;
		}
		//有符号长整形
		case WAVE_LONG:
		{
			long *temp1 = (long *)base;
			long *temp2 = (long *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			break;
		}
		//无符号长整形
		case WAVE_ULONG:
		{
			unsigned long *temp1 = (unsigned long *)base;
			unsigned long *temp2 = (unsigned long *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			break;
		}
		//单精度浮点数
		case WAVE_FLOAT:
		{
			float *temp1 = (float *)base;
			float *temp2 = (float *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				default :
				{
					return 0;
				}
			}
			break;
		}
		//双精度浮点数
		case WAVE_DOUBLE:
		{
			double *temp1 = (double *)base;
			double *temp2 = (double *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				default :
				{
					return 0;
				}
				
			}
			break;
		}
		default :
		{
			char *temp1 = (char *)base;
			char *temp2 = (char *)message;
			switch (op)
			{
				case WAVE_ADD:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] + temp2[i];
					}
					break;
				}
				case WAVE_MUL:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] * temp2[i];
					}
					break;
				}
				case WAVE_AND:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] & temp2[i];
					}
					break;
				}
				case WAVE_OR:
				{
					for(i=0; i<length; i++){
						temp1[i] = temp1[i] | temp2[i];
					}
					break;
				}
			}
			return 0;
		}
	}
	return 1;
}


//返回数据类型一个所占内存大小
int WAVE_SizeOf(int type){
	switch(type)
	{
		case WAVE_CHAR:
		{
			return sizeof(char);
		}
		case WAVE_INT:
		{
			return sizeof(int);
		}
		case WAVE_UINT:
		{
			return sizeof(unsigned int);
		}
		case WAVE_LONG:
		{
			return sizeof(long);
		}
		case WAVE_ULONG:
		{
			return sizeof(unsigned long);
		}
		case WAVE_FLOAT:
		{
			return sizeof(float);
		}
		case WAVE_DOUBLE:
		{
			return sizeof(double);
		}
	}
	return sizeof(size_t);
}


//将一条消息分成多个WAVE_BAG_SIZE发送
size_t WAVE_SendMessage(int socket, void *buf, size_t length){
	size_t i = 0, size;
	while(i < length){
		size = length - i;
		if(size > WAVE_BAG_SIZE)
			size = WAVE_BAG_SIZE;
		if(send(socket, (void *)(buf + i), size, 0) == -1){
			return 0;
		}
		i += WAVE_BAG_SIZE;
	}
	return length;
}


//将一条消息分成多个WAVE_BAG_SIZE接收
size_t WAVE_RecvMessage(int socket, void *buf, size_t length){
	size_t i = 0, size;
	while(i < length){
		size = length - i;
		if(size > WAVE_BAG_SIZE)
			size = WAVE_BAG_SIZE;
		if(recv(socket, (void *)(buf + i), size, MSG_WAITALL) == -1){
		//if(recv(socket, (void *)(buf + i), size, 0) == -1){
			return 0;
		}
		i += WAVE_BAG_SIZE;
	}
	return length;
}


//同步函数
int WAVE_Barrier(){
	//使用0号进程充当server,其余进程向它发消息协议如何同步
	int root = 0;
	//假设发送的消息是int类型
	int type = WAVE_INT;
	//假设发送的消息长度是0
	size_t length = 0;
	//发送进程
	if(WAVE_RANK == root){
		int barrierSize = WAVE_SIZE - 1;   //除root进程之外同步进程数目
		int client_socket = -1;						//客户端套接口
		socklen_t client_size = sizeof(struct sockaddr);         //客户端网络地址长度
		struct sockaddr_in client_in;  //客户端网络地址
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度
		char *buf2;
		char *buf3;
		size_t i, j, k, l, m;
		int nfds;
		size_t client_num=0; //已连接的客户端数
		size_t client_end=0;  //已结束广播的客户端数
		size_t client_accept = 0;   //建立连接的客户端数目
		int error = 0;
		
		//监听网络请求
		if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
			printf("root %d Barrier call to listen fail\n", WAVE_RANK);
			exit(0);
		}
		
		//声明epoll_event结构体的变量,ev用于注册事件,数组用于回传要处理的事件
		struct epoll_event ev;
		struct epoll_event *events =(struct epoll_event *)malloc(sizeof(struct epoll_event)*WAVE_EPOLL_EVENTS_NUM);
		//生成用于处理accept的epoll专用的文件描述符
		int epfd = epoll_create(WAVE_SIZE);
		//设置与要处理的事件相关的文件描述符
		ev.data.fd = WAVE_SERVER_SOCKET;
		//设置要处理的事件类型
		ev.events = EPOLLIN;
		//注册epoll事件
		epoll_ctl(epfd, EPOLL_CTL_ADD, WAVE_SERVER_SOCKET, &ev);
		
#ifdef DEBUG
	printf("node %d Barrier accepting other node connections ...\n", WAVE_RANK);
#endif
		while(client_end < barrierSize){
			//等待epoll事件的发生
			nfds = epoll_wait(epfd, events, WAVE_EPOLL_EVENTS_NUM, -1);
			
			//处理所发生的所有事件
			for(i=0; i<nfds; ++i)
			{
				//如果新监测到一个SOCKET用户连接到了绑定的SOCKET端口，建立新的连接。
				if(client_accept < barrierSize && events[i].data.fd == WAVE_SERVER_SOCKET)
				{
					//接收客户请求
					if((client_socket = accept(WAVE_SERVER_SOCKET, (struct sockaddr*)&client_in, &client_size))==-1){
						printf("root %d Barrier call to accept fail \n", WAVE_RANK);
						exit(0);
					}
#ifdef DEBUG
					char *str = inet_ntoa(client_in.sin_addr);
					printf("root %d Barrier call to accept a connection from %s\n", WAVE_RANK, str);
#endif
					//非阻塞方式
					WAVE_SetNonBlocking(client_socket);
					//设置用于读操作的文件描述符
					ev.data.fd = client_socket;
					//设置用于注册的读操作事件
					ev.events=EPOLLIN;
					//注册ev
					epoll_ctl(epfd, EPOLL_CTL_ADD, client_socket, &ev);
					client_accept++;
				}
				//如果是已经连接的用户，读入认证消息
				else if(events[i].events & EPOLLIN)
				{
					client_socket = events[i].data.fd;
					//获取客户发送的数据
					if(recv(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
						printf("root %d Barrier call to recv ac message fail \n", WAVE_RANK);
						exit(0);
					}
					
					j = strtol(buf1, &buf2, 10);              //连接进程的rank
					k = strtol((char *)(buf2+1), &buf3, 10);  //连接进程想要的消息tag
					l = strtol((char *)(buf3+1), &buf2, 10);  //连接进程想要的消息类型
					m = strtol((char *)(buf2+1), NULL, 10);   //连接进程想要的消息长度
#ifdef DEBUG
					printf("root %d Barrier received ac message from client %d: %s\n", WAVE_RANK, j, buf1);
#endif
				
					//信息类型和长度不匹配说明程序有错
					if(k != WAVE_TAG_BARRIER || l != type || m != length){
						error = 1;
					}

					//设置用于注测的写操作事件
					ev.data.fd = client_socket;
					//修改client_socket上要处理的事件为EPOLLOUT
					ev.events = EPOLLOUT;
					epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);
					client_num++;
				}
				else if(client_num == barrierSize && (events[i].events & EPOLLOUT))// 如果有数据发送
				{
					//发送对认证消息的确认
					client_socket = events[i].data.fd;
					//程序员的程序有错
					if(error){
						memcpy(buf1, "Fail_barrier", 13);
						//将信息类型和长度不匹配警告广播给所有进程
						if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
							printf("root %d Barrier call to send ac fail message fail\n", WAVE_RANK);
							exit(0);
						}
#ifdef DEBUG
	printf("root %d Barrier call to send ac fail message: %s\n", WAVE_RANK, buf1);
#endif
					}
					else{
						memcpy(buf1, "Success_barrier", 16);
						//将认证成功广播给所有进程
						if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
							printf("root %d Barrier call to send ac success message fail\n", WAVE_RANK);
							exit(0);
						}
#ifdef DEBUG
	printf("root %d Barrier call to send ac success message: %s\n", WAVE_RANK, buf1);
#endif
					}
					close(client_socket);
					//close(events[i].data.fd);
					client_end++;
				}//end if out
			}//end for 
		}//end while(1)
		//释放申请的认证信息缓冲区
#ifdef DEBUG
	printf("node %d reach Barrier\n", WAVE_RANK);
#endif
		close(epfd);
		free(buf1);
		free(events);
		if(error){
			printf("root %d Barrier fail because error in your program(not all node can reach this Barrier).\n", WAVE_RANK);
			exit(0);
		}
		else
			return 1;
	}
	else{
		int server_socket;				 //服务器套接口
		struct sockaddr_in server_in;    //服务器网络地址
		struct hostent *server_name;
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度

		//转换服务器域名或 IP 地址为 IP 结构体
		if((server_name = gethostbyname(WAVE_NODES[root].ip)) == 0){
			printf("node %d barrier. error resolving host %s\n", WAVE_RANK, WAVE_NODES[root].ip);
			exit(0);
		}
		//初始化 IP 地址结构
		bzero(&server_in,sizeof(server_in));
		server_in.sin_family = AF_INET;
		server_in.sin_addr.s_addr = htonl(INADDR_ANY);
		server_in.sin_addr.s_addr = ((struct in_addr*)(server_name->h_addr))->s_addr;
		server_in.sin_port = htons(WAVE_NODES[root].port);

		while(1){
			//获取远程服务器套接口描述符
			if((server_socket = socket(AF_INET,SOCK_STREAM,0)) == -1){
				printf("node %d barrier call to socket fail\n", WAVE_RANK);
				exit(0);
			}

#ifdef DEBUG
	printf("node %d barrier start connect node %d(ip %s port %d)\n", WAVE_RANK, root, WAVE_NODES[root].ip, WAVE_NODES[root].port);
#endif
			//发送连接请求到服务器
			while(connect(server_socket, (struct sockaddr *)&server_in, sizeof(server_in)) == -1){
				usleep(WAVE_CLOCK);
			}
#ifdef DEBUG
	printf("node %d barrier  start connect node %d success\n", WAVE_RANK, root);
#endif
		
			//创建认证信息
#ifdef WAVE_WORD_32
			sprintf(buf1, "%d %d %d %d", WAVE_RANK, WAVE_TAG_BARRIER, type, length);
#else
			sprintf(buf1, "%d %ld %d %ld", WAVE_RANK, WAVE_TAG_BARRIER, type, length);
#endif
			//发送认证消息给同步方
			if(send(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d barrier(认证步骤1：发送自己的rank,tag,type,length失败)\n", WAVE_RANK);
				exit(0);
			}
			//从同步方接收认证消息的确认
			if(recv(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d barrier(认证步骤2：接收提示消息失败)\n", WAVE_RANK);
				exit(0);
			}
		
			//同步成功
			if(strcmp(buf1, "Success_barrier") == 0){
#ifdef DEBUG
	printf("node %d reach Barrier\n", WAVE_RANK);
#endif
				//断开连接
				close(server_socket);
				break;
			}
			else{//同步认证失败，身份不匹配
				//同步进程此时不是在同步，而在做一对一通信，此时还不想给自己发信息，需要不断再请求
				if(strcmp(buf1, "Fail_conflict") == 0){
					//断开连接
					close(server_socket);
					usleep(WAVE_CLOCK);
					continue;
				}
				//同步不匹配，说明程序有错
				if(strcmp(buf1, "Fail_barrier") == 0){
					printf("root %d crash.error in your program(call barrier)\n", root);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
			}
		}
		free(buf1);
		return 1;
	}
}


//普通归约函数
int WAVE_Reduce(void *orimessage, void *recvbuf, size_t length, int type, int op, int root){
	size_t reducMessageSize = WAVE_SizeOf(type)*length;  //归约信息所占内存大小
	//root进程
	if(WAVE_RANK == root){
		int reducSize = WAVE_SIZE - 1;   //归约的目标进程数
		int client_socket;						//客户端套接口
		struct WAVE_epoll_data *clients = (struct WAVE_epoll_data *)malloc(sizeof(struct WAVE_epoll_data)*reducSize); //连接的客户端状态
		socklen_t client_size = sizeof(struct sockaddr);         //客户端网络地址长度
		struct sockaddr_in client_in;  //客户端网络地址
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度
		char *buf2;
		char *buf3;
		void *temp = (void *)malloc(reducMessageSize);
		size_t i, j, k, l, m;
		int nfds;
		size_t client_num=0; //已连接的客户端数
		size_t client_end=0;  //已结束广播的客户端数
		size_t client_accept = 0;  //已建立的连接数
		int error = 0;
		
		//初始化归约结果
		memcpy(recvbuf, orimessage, reducMessageSize);
		
		//监听网络请求
		if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
			printf("root %d Reduc call to listen fail\n", WAVE_RANK);
			exit(0);
		}
		
		//声明epoll_event结构体的变量,ev用于注册事件,数组用于回传要处理的事件
		struct epoll_event ev;
		struct epoll_event *events =(struct epoll_event *)malloc(sizeof(struct epoll_event)*WAVE_EPOLL_EVENTS_NUM);
		//生成用于处理accept的epoll专用的文件描述符
		int epfd = epoll_create(WAVE_SIZE);
		//设置与要处理的事件相关的文件描述符
		struct WAVE_epoll_data epoll_data_temp0;
		epoll_data_temp0.fd = WAVE_SERVER_SOCKET;
		epoll_data_temp0.state = WAVE_CLIENT_ORI;
		ev.data.ptr = &epoll_data_temp0;
		//设置要处理的事件类型
		ev.events = EPOLLIN;
		//注册epoll事件
		epoll_ctl(epfd, EPOLL_CTL_ADD, WAVE_SERVER_SOCKET, &ev);
#ifdef DEBUG
	printf("root %d Reduc accepting other node connections ...\n", WAVE_RANK);
#endif
		
		while(client_end < reducSize){
			//等待epoll事件的发生
			nfds = epoll_wait(epfd, events, WAVE_EPOLL_EVENTS_NUM, -1);
			
			//处理所发生的所有事件
			for(i=0; i<nfds; ++i)
			{
				struct WAVE_epoll_data *epoll_data_temp1 = (struct WAVE_epoll_data *)(events[i].data.ptr);
				//如果新监测到一个SOCKET用户连接到了绑定的SOCKET端口，建立新的连接。
				if(client_accept<reducSize && (*epoll_data_temp1).fd == WAVE_SERVER_SOCKET)
				{
					//接收客户请求
					if((client_socket = accept(WAVE_SERVER_SOCKET, (struct sockaddr*)&client_in, &client_size))==-1){
						printf("root %d Reduc call to accept fail \n", WAVE_RANK);
						exit(0);
					}
#ifdef DEBUG
					char *str = inet_ntoa(client_in.sin_addr);
					printf("root %d Reduc accapt a connection from %s\n", WAVE_RANK, str);
#endif

					//非阻塞方式
					WAVE_SetNonBlocking(client_socket);
					//设置用于读操作的文件描述符
					struct WAVE_epoll_data *epoll_data_temp2 = (struct WAVE_epoll_data *)(clients + client_accept);
					(*epoll_data_temp2).fd = client_socket;
					(*epoll_data_temp2).state = WAVE_CLIENT_START;
					ev.data.ptr = epoll_data_temp2;
					//设置用于注册的读操作事件
					ev.events=EPOLLIN;
					//注册ev
					epoll_ctl(epfd, EPOLL_CTL_ADD, client_socket, &ev);
					client_accept++;
				}
				//如果是已经连接的用户，读入认证消息
				else if(events[i].events & EPOLLIN)
				{
					client_socket = (*epoll_data_temp1).fd;
					if((*epoll_data_temp1).state == WAVE_CLIENT_START){
						//获取客户发送的数据
						if(recv(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
							printf("root %d Reduc call to recv ac message fail \n", WAVE_RANK);
							exit(0);
						}
						
						j = strtol(buf1, &buf2, 10);              //连接进程的rank
						k = strtol((char *)(buf2+1), &buf3, 10);  //连接进程想要的消息tag
						l = strtol((char *)(buf3+1), &buf2, 10);  //连接进程想要的消息类型
						m = strtol((char *)(buf2+1), &buf3, 10);  //连接进程想要的消息长度
#ifdef DEBUG
						printf("root %d reduc received ac message from client%d: %s\n", WAVE_RANK, j, buf1);
#endif
						
						//信息类型和长度不匹配说明程序有错
						if(k != WAVE_TAG_REDUC || l != type || m != length){
							error = 1;
						}
						
						//设置用于注测的写操作事件
						(*epoll_data_temp1).state = WAVE_CLIENT_WRITE1;
						ev.data.ptr = epoll_data_temp1;
						//修改client_socket上要处理的事件为EPOLLOUT
						ev.events = EPOLLOUT;
						epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);	
						client_num++;
						continue;
					}
					if(client_num == reducSize && (*epoll_data_temp1).state == WAVE_CLIENT_READ1){
						//获取客户发送的数据
						if(WAVE_RecvMessage(client_socket, temp, reducMessageSize) == 0){
							printf("root %d Reduc call to recv message fail \n", WAVE_RANK);
							exit(0);
						}
						//做运算，recvbuf= recvbuf (op) temp；
						WAVE_Operation(recvbuf, temp, length, type, op);
					
						//WAVE_CloseAndDisable(client_socket, events[i]);
						close(client_socket);
						client_end++;
					}		
				}
				else if(client_num == reducSize && (events[i].events & EPOLLOUT)) // 如果有数据发送
				{	
					client_socket = (*epoll_data_temp1).fd;
					//程序员的程序有错
					if(error){
						memcpy(buf1, "Fail_reduc", 11);
						//将信息类型和长度不匹配警告广播给所有进程
						if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
							printf("root %d Reduc call to send ac fail message fail\n", WAVE_RANK);
							exit(0);
						}
						//WAVE_CloseAndDisable(client_socket, events[i]);
						close(client_socket);
						client_end++;
#ifdef DEBUG
	printf("root %d Reduc call to send ac fail message: %s\n", WAVE_RANK, buf1);
#endif
					}
					else{
						memcpy(buf1, "Success_reduc", 14);
						//将认证成功广播给所有进程
						if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
							printf("root %d Reduc call to send ac success message fail\n", WAVE_RANK);
							exit(0);
						}
#ifdef DEBUG
	printf("root %d Reduc call to send ac success message: %s\n", WAVE_RANK, buf1);
#endif
						//设置用于读操作的文件描述符
						(*epoll_data_temp1).state = WAVE_CLIENT_READ1;
						ev.data.ptr = epoll_data_temp1;
						//设置用于注测的读操作事件
						ev.events=EPOLLIN;
						epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);
					}
				}//end if out
			}//end for 
		}//end while(1)
		
#ifdef DEBUG
	printf("root %d Reduc end\n",WAVE_RANK);
#endif
		close(epfd);
		//释放申请的认证信息缓冲区
		free(clients);
		free(buf1);
		free(events);
		free(temp);
		if(error){
			printf("root %d Reduc fail because error in your program(not all node can take part in Reduc).\n", WAVE_RANK);
			exit(0);
		}
		else
			return 1;
	}
	else{
		int server_socket;				 //服务器套接口
		struct sockaddr_in server_in;    //服务器网络地址
		struct hostent *server_name;
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度

		//转换服务器域名或 IP 地址为 IP 结构体
		if((server_name = gethostbyname(WAVE_NODES[root].ip)) == 0){
			printf("node %d send reduc. error resolving host %s\n", WAVE_RANK, WAVE_NODES[root].ip);
			exit(0);
		}
		//初始化 IP 地址结构
		bzero(&server_in,sizeof(server_in));
		server_in.sin_family = AF_INET;
		server_in.sin_addr.s_addr = htonl(INADDR_ANY);
		server_in.sin_addr.s_addr = ((struct in_addr*)(server_name->h_addr))->s_addr;
		server_in.sin_port = htons(WAVE_NODES[root].port);

		while(1){
			//获取远程服务器套接口描述符
			if((server_socket = socket(AF_INET,SOCK_STREAM,0)) == -1){
				printf("node %d send reduc call to socket fail\n", WAVE_RANK);
				exit(0);
			}
#ifdef DEBUG
	printf("node %d send reduc start connect node %d(ip %s port %d)\n", WAVE_RANK, root, WAVE_NODES[root].ip, WAVE_NODES[root].port);
#endif
			//发送连接请求到服务器
			while(connect(server_socket, (struct sockaddr *)&server_in, sizeof(server_in)) == -1){
				usleep(WAVE_CLOCK);
			}
#ifdef DEBUG
	printf("node %d send reduc connect node %d success\n", WAVE_RANK, root);
#endif
			
			//创建认证信息
#ifdef WAVE_WORD_32
			sprintf(buf1, "%d %d %d %d", WAVE_RANK, WAVE_TAG_REDUC, type, length);
#else
			sprintf(buf1, "%d %ld %d %ld", WAVE_RANK, WAVE_TAG_REDUC, type, length);
#endif
			//发送认证消息给归约方
			if(send(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d send reduc(认证步骤1：发送自己的rank,tag,type,length失败)\n", WAVE_RANK);
				exit(0);
			}
			//从归约方接收认证消息的确认
			if(recv(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d send reduc(认证步骤2：接收提示消息失败)\n", WAVE_RANK);
				exit(0);
			}
			
		//处理确认信息
		{
			//通信双方认证成功，身份正确
			if(strcmp(buf1, "Success_reduc") == 0){
				//发送消息给归约方
				if(WAVE_SendMessage(server_socket, orimessage, reducMessageSize) == 0){
					printf("node %d send reduc fail in send message\n", WAVE_RANK);
					exit(0);
				}
				//断开连接
				close(server_socket);
				break;
			}
			else{//通信双方认证失败，身份不匹配
				//归约进程此时不是在归约，而在做一对一通信，此时还不想给自己发信息，需要不断再请求
				if(strcmp(buf1, "Fail_conflict") == 0){
					//断开连接
					close(server_socket);
					usleep(WAVE_CLOCK);
					continue;
				}
				//归约数据类型，长度不匹配，说明程序有错
				if(strcmp(buf1, "Fail_reduc") == 0){
					printf("error in your program that root %d reduc and node %d send message is different\n", root, WAVE_RANK);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
			}
		}	
		
		}

#ifdef DEBUG			
	printf("node %d Reduc end\n",WAVE_RANK);
#endif
		free(buf1);
		return 1;
	}
}


//epoll实现的普通广播函数
int WAVE_Bcast(void *orimessage, int length, int type, int root){
	size_t bcastMessageSize = WAVE_SizeOf(type)*length;  //广播消息所占内存大小
	//发送进程
	if(WAVE_RANK == root){
		int bcastSize = WAVE_SIZE - 1;   //广播目标进程数目
		int client_socket;						//客户端套接口
		struct WAVE_epoll_data *clients = (struct WAVE_epoll_data *)malloc(sizeof(struct WAVE_epoll_data)*bcastSize); //连接的客户端状态
		socklen_t client_size = sizeof(struct sockaddr);         //客户端网络地址长度
		struct sockaddr_in client_in;  //客户端网络地址
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度
		char *buf2;
		char *buf3;
		size_t i, j, k, l, m;
		int nfds;
		size_t client_num=0; //已连接的客户端数
		size_t client_end=0;  //已结束广播的客户端数
		size_t client_accept = 0;   //建立连接的客户端数目
		int error = 0;
		
		//监听网络请求
		if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
			printf("root %d Bcast call to listen fail\n", WAVE_RANK);
			exit(0);
		}
		
		//声明epoll_event结构体的变量,ev用于注册事件,数组用于回传要处理的事件
		struct epoll_event ev;
		struct epoll_event *events =(struct epoll_event *)malloc(sizeof(struct epoll_event)*WAVE_EPOLL_EVENTS_NUM);
		//生成用于处理accept的epoll专用的文件描述符
		int epfd = epoll_create(WAVE_SIZE);
		//设置与要处理的事件相关的文件描述符
		struct WAVE_epoll_data epoll_data_temp0;
		epoll_data_temp0.fd = WAVE_SERVER_SOCKET;
		epoll_data_temp0.state = WAVE_CLIENT_ORI;
		ev.data.ptr = &epoll_data_temp0;
		//设置要处理的事件类型
		ev.events = EPOLLIN;
		//注册epoll事件
		epoll_ctl(epfd, EPOLL_CTL_ADD, WAVE_SERVER_SOCKET, &ev);
		
#ifdef DEBUG
	printf("node %d Bcast accepting other node connections ...\n", WAVE_RANK);
#endif
		while(client_end < bcastSize){
			//等待epoll事件的发生
			nfds = epoll_wait(epfd, events, WAVE_EPOLL_EVENTS_NUM, -1);
			
			//处理所发生的所有事件
			for(i=0; i<nfds; ++i)
			{
				struct WAVE_epoll_data *epoll_data_temp1 = (struct WAVE_epoll_data *)(events[i].data.ptr);
				//如果新监测到一个SOCKET用户连接到了绑定的SOCKET端口，建立新的连接。
				if(client_accept < bcastSize && (*epoll_data_temp1).fd == WAVE_SERVER_SOCKET)
				{
					//接收客户请求
					if((client_socket = accept(WAVE_SERVER_SOCKET, (struct sockaddr*)&client_in, &client_size))==-1){
						printf("root %d Bcast call to accept fail \n", WAVE_RANK);
						exit(0);
					}
#ifdef DEBUG
					char *str = inet_ntoa(client_in.sin_addr);
					printf("root %d Bcast accept a connection from %s\n", WAVE_RANK, str);
#endif

					//非阻塞方式
					WAVE_SetNonBlocking(client_socket);
					//设置用于读操作的文件描述符
					struct WAVE_epoll_data *epoll_data_temp2 = (struct WAVE_epoll_data *)(clients + client_accept);
					(*epoll_data_temp2).fd = client_socket;
					(*epoll_data_temp2).state = WAVE_CLIENT_START;
					ev.data.ptr = epoll_data_temp2;
					//设置用于注册的读操作事件
					ev.events=EPOLLIN;
					//注册ev
					epoll_ctl(epfd, EPOLL_CTL_ADD, client_socket, &ev);
					client_accept++;
				}
				//如果是已经连接的用户，读入认证消息
				else if(events[i].events & EPOLLIN)
				{
					client_socket = (*epoll_data_temp1).fd;
					//获取客户发送的数据
					if(recv(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
						printf("root %d Bcast call to recv ac message fail \n", WAVE_RANK);
						exit(0);
					}
					
					j = strtol(buf1, &buf2, 10);              //连接进程的rank
					k = strtol((char *)(buf2+1), &buf3, 10);  //连接进程想要的消息tag
					l = strtol((char *)(buf3+1), &buf2, 10);  //连接进程想要的消息类型
					m = strtol((char *)(buf2+1), NULL, 10);   //连接进程想要的消息长度
#ifdef DEBUG
					printf("root %d Bcast received ac message from client%d: %s\n", WAVE_RANK, j, buf1);
#endif
					//信息类型和长度不匹配说明程序有错
					if(k != WAVE_TAG_BCAST || l != type || m != length){
						error = 1;
					}
					
					//设置用于注测的写操作事件
					(*epoll_data_temp1).state = WAVE_CLIENT_WRITE1;
					ev.data.ptr = epoll_data_temp1;
					//修改client_socket上要处理的事件为EPOLLOUT
					ev.events = EPOLLOUT;
					epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);
					client_num++;
				}
				else if(client_num == bcastSize && (events[i].events & EPOLLOUT))// 如果有数据发送
				{
						client_socket = (*epoll_data_temp1).fd;
						
						//发送对认证消息的确认
						switch ((*epoll_data_temp1).state)
						{
							case WAVE_CLIENT_WRITE1:
							{
								//程序员的程序有错
								if(error){
									memcpy(buf1, "Fail_bcast", 11);
									//将信息类型和长度不匹配警告广播给所有进程
									if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
										printf("root %d Bcast call to send ac fail message fail\n", WAVE_RANK);
										exit(0);
									}
									//WAVE_CloseAndDisable(client_socket, events[i]);
									close(client_socket);
									client_end++;
#ifdef DEBUG
	printf("root %d Bcast call to send ac fail message: %s\n", WAVE_RANK, buf1);
#endif
								}
								else{
									memcpy(buf1, "Success_bcast", 14);
									//将认证成功广播给所有进程
									if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
										printf("root %d Bcast call to send ac success message fail\n", WAVE_RANK);
										exit(0);
									}
#ifdef DEBUG
	printf("root %d Bcast call to send ac success message: %s\n", WAVE_RANK, buf1);
#endif
									//设置用于读操作的文件描述符
									(*epoll_data_temp1).state = WAVE_CLIENT_READ1;
									ev.data.ptr = epoll_data_temp1;
									//设置用于注测的读操作事件
									ev.events=EPOLLOUT;
									epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);
								}
								break;
							}
							
							case WAVE_CLIENT_READ1:
							{
								if(WAVE_SendMessage(client_socket, orimessage, bcastMessageSize) == 0){
									printf("root %d bcast fail in the last step\n", WAVE_RANK);
									exit(0);
								}
								//WAVE_CloseAndDisable(client_socket, events[i]);
								close(client_socket);
								client_end++;
								break;
							}
						}//end switch
				}//end if out
			}//end for 
		}//end while(1)
#ifdef DEBUG
	printf("root %d Bcast end\n", WAVE_RANK);
#endif
		//释放申请的认证信息缓冲区
		close(epfd);
		free(clients);
		free(buf1);
		free(events);
		if(error){
			printf("root %d Bcast fail because error in your program(not all node can take part Bcast).\n", WAVE_RANK);
			exit(0);
		}
		else
			return 1;
	}
	else{
		int server_socket;				 //服务器套接口
		struct sockaddr_in server_in;    //服务器网络地址
		struct hostent *server_name;
		char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度

		//转换服务器域名或 IP 地址为 IP 结构体
		if((server_name = gethostbyname(WAVE_NODES[root].ip)) == 0){
			printf("node %d recv bcast. error resolving host %s\n", WAVE_RANK, WAVE_NODES[root].ip);
			exit(0);
		}
		//初始化 IP 地址结构
		bzero(&server_in,sizeof(server_in));
		server_in.sin_family = AF_INET;
		server_in.sin_addr.s_addr = htonl(INADDR_ANY);
		server_in.sin_addr.s_addr = ((struct in_addr*)(server_name->h_addr))->s_addr;
		server_in.sin_port = htons(WAVE_NODES[root].port);

		while(1){
			//获取远程服务器套接口描述符
			if((server_socket = socket(AF_INET,SOCK_STREAM,0)) == -1){
				printf("node %d recv bcast call to socket fail\n", WAVE_RANK);
				exit(0);
			}
#ifdef DEBUG
	printf("node %d recv bcast start connect node %d(ip %s port %d)\n", WAVE_RANK, root, WAVE_NODES[root].ip, WAVE_NODES[root].port);
#endif
			//发送连接请求到服务器
			while(connect(server_socket, (struct sockaddr *)&server_in, sizeof(server_in)) == -1){
				usleep(WAVE_CLOCK);
			}
#ifdef DEBUG
	printf("node %d recv bcast start connect node %d success\n", WAVE_RANK, root);
#endif
		
			//创建认证信息
#ifdef WAVE_WORD_32
			sprintf(buf1, "%d %d %d %d", WAVE_RANK, WAVE_TAG_BCAST, type, length);
#else
			sprintf(buf1, "%d %ld %d %ld", WAVE_RANK, WAVE_TAG_BCAST, type, length);
#endif
			//发送认证消息给广播方
			if(send(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d recv bcast(认证步骤1：发送自己的rank,tag,type,length失败)\n", WAVE_RANK);
				exit(0);
			}
			//从广播方接收认证消息的确认
			if(recv(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d recv bcast(认证步骤2：接收提示消息失败)\n", WAVE_RANK);
				exit(0);
			}
		
		//处理确认信息
		{
			//通信双方认证成功，身份正确
			if(strcmp(buf1, "Success_bcast")==0){
				//直接接收数据
				if(WAVE_RecvMessage(server_socket, orimessage, bcastMessageSize) == 0){
					printf("node %d recv bcast fail in the last step\n", WAVE_RANK);
					exit(0);
				}
				//断开连接
				close(server_socket);
				break;
			}
			else{//通信双方认证失败，身份不匹配
				//广播进程此时不是在广播，而在做一对一通信，此时还不想给自己发信息，需要不断再请求
				if(strcmp(buf1, "Fail_conflict") == 0){
					//断开连接
					close(server_socket);
					usleep(WAVE_CLOCK);
					continue;
				}
				//广播标签，长度不匹配，说明程序有错
				if(strcmp(buf1, "Fail_bcast") == 0){
					printf("error in your program that node %d bcast and node %d recv message is different\n", root, WAVE_RANK);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
			}
		}
		}
#ifdef DEBUG
	printf("node %d Bcast end\n", WAVE_RANK);
#endif
		free(buf1);
		return 1;
	}
}


//接收消息函数
int WAVE_Recv(void *message, size_t length, int type, int src, size_t tag, WAVE_Status *status){
	int client_socket;             //客户端套接口
	socklen_t client_size = sizeof(struct sockaddr);         //客户端网络地址长度
	struct sockaddr_in client_in;  //客户端网络地址
	char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度
	char *buf2;
	char *buf3;
	size_t i, j, k, l;
	int flag = 0;
	
	//监听网络请求
	if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
		printf("node %d recv message call to listen fail\n", WAVE_RANK);
		exit(0);
	}

#ifdef DEBUG
	printf("node %d recv message accepting send node %d connections ...\n", WAVE_RANK, src);
#endif
	
	while(1){
		//接收客户请求
		if((client_socket = accept(WAVE_SERVER_SOCKET, (struct sockaddr*)&client_in, &client_size)) == -1){
			printf("node %d recv message call to accept fail \n", WAVE_RANK);
			exit(0);
		}

		//获取客户发送的认证信息
		if(recv(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
			printf("node %d recv message(认证步骤1：接受发送方的rank,tag,type,length失败)\n", WAVE_RANK);
			exit(0);
		}
		i = strtol(buf1, &buf2, 10);              //连接进程的rank
		j = strtol((char *)(buf2+1), &buf3, 10);  //连接进程想要的消息tag
		k = strtol((char *)(buf3+1), &buf2, 10);  //连接进程想要的消息类型
		l = strtol((char *)(buf2+1), &buf3, 10);   //连接进程想要的消息长度
		
#ifdef DEBUG
	printf("node %d recv message call to accept a node %d and ac message: %s \n", WAVE_RANK, i, buf1);
#endif
		if(k == type && l == length){
			if(src==WAVE_ANY_SOURCE && (j==tag || tag==WAVE_TAG_ANY))
				flag = 1;
			if(i==src && (j==tag || tag==WAVE_TAG_ANY))
				flag = 1;
		}
		//验证身份成功，请求发送消息
		if(flag == 1){
			if(status != NULL){
				(*status).source = i;
				(*status).tag = j;
			}
			
			memcpy(buf1, "Success_send", 13);
			//发送需要的确认消息，让发送方开始发送
			if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d recv message(认证步骤3：发消息告诉目标进程连接成功信号和消息长度失败)\n", WAVE_RANK);
				exit(0);
			}

			//直接接收数据
			if(WAVE_RecvMessage(client_socket, message, WAVE_SizeOf(type)*length) == 0){
				printf("node %d recv message fail in the last step\n", WAVE_RANK);
				exit(0);
			}
			
			//断开连接
			close(client_socket);
			break;
		}
		
		
		//连接的进程不是此次要接收的目标进程，选择拒绝，继续监听
		if(i != src){
			//这一类只是执行导致的冲突，并不是程序错误
			if(j == WAVE_TAG_BCAST || j == WAVE_TAG_REDUC || j == WAVE_TAG_BARRIER){
				memcpy(buf1, "Fail_conflict", 14);
				if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
					printf("node %d recv message(认证步骤2：发消息提醒进程发生冲突失败)\n", WAVE_RANK);
					exit(0);
				}
				close(client_socket);
				continue;
			}
			else{
				memcpy(buf1, "Fail_rank", 10);
				if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
					printf("node %d recv message(认证步骤2：发消息提醒错误的目标进程失败)\n", WAVE_RANK);
					exit(0);
				}
				close(client_socket);
				continue;
			}
		}
		//连接的进程的信息标签不匹配，说明程序有问题，选择拒绝退出。
		if(j != tag ){
			memcpy(buf1, "Fail_tag", 9);
			if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d recv message(认证步骤2：发消息提醒目标进程的错误的标签失败)\n", WAVE_RANK);
				exit(0);
			}
			close(client_socket);
			printf("error in your program that the node %d recv and send tag is different\n", WAVE_RANK);
			//释放申请的认证信息缓冲区
			free(buf1);
			exit(0);
			return 0;
		}
		//连接的进程的消息类型不匹配，说明程序有问题，选择拒绝退出。
		if(k != type ){
			memcpy(buf1, "Fail_type", 10);
			if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d recv message(认证步骤2：发消息提醒目标进程数据类型不匹配失败)\n", WAVE_RANK);
				exit(0);
			}
			close(client_socket);
			printf("error in your program that the node %d recv and send data type is different\n", WAVE_RANK);
			//释放申请的认证信息缓冲区
			free(buf1);
			exit(0);
			return 0;
		}
		//连接的进程的消息类型不匹配，说明程序有问题，选择拒绝退出。
		if(l != length ){
			memcpy(buf1, "Fail_length", 12);
			if(send(client_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
				printf("node %d send message(认证步骤2：发消息提醒目标进程数据长度不匹配失败)\n", WAVE_RANK);
				exit(0);
			}
			close(client_socket);
			printf("error in your program that the node %d recv and send data length is different\n", WAVE_RANK);
			//释放申请的认证信息缓冲区
			free(buf1);
			exit(0);
			return 0;
		}
	}
#ifdef DEBUG
	printf("node %d recv from node %d end\n", WAVE_RANK, src);
#endif
	//释放申请的认证信息缓冲区
	free(buf1);
	return 1;
}


//发送消息函数
int WAVE_Send(void *orimessage, size_t length, int type, int dest, size_t tag){
	int server_socket;				 //服务器套接口
	struct sockaddr_in server_in;    //服务器网络地址
	struct hostent *server_name;
	char *buf1 = (char *)malloc(sizeof(char)*WAVE_ACMESSAGE_LENGTH);  //收发认证信息缓冲区长度

	//转换服务器域名或 IP 地址为 IP 结构体
	if((server_name = gethostbyname(WAVE_NODES[dest].ip)) == 0){
		printf("node %d error resolving host %s\n", WAVE_RANK, WAVE_NODES[dest].ip);
		exit(0);
	}
	//初始化 IP 地址结构
	bzero(&server_in,sizeof(server_in));
	server_in.sin_family = AF_INET;
	server_in.sin_addr.s_addr = htonl(INADDR_ANY);
	server_in.sin_addr.s_addr = ((struct in_addr*)(server_name->h_addr))->s_addr;
	server_in.sin_port = htons(WAVE_NODES[dest].port);
	
	while(1){
		//获取远程服务器套接口描述符
		if((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1){
			printf("node %d send message call to socket fail\n", WAVE_RANK);
			exit(0);
		}
#ifdef DEBUG
	printf("node %d send message start connect node %d(ip %s port %d)\n", WAVE_RANK, dest, WAVE_NODES[dest].ip, WAVE_NODES[dest].port);
#endif
		//发送连接请求到服务器
		while(connect(server_socket, (struct sockaddr *)&server_in, sizeof(server_in)) == -1){
			usleep(WAVE_CLOCK);
		}
#ifdef DEBUG
	printf("node %d send message start connect node %d success\n", WAVE_RANK, dest);
#endif
		
		//创建认证信息
#ifdef WAVE_WORD_32
		sprintf(buf1, "%d %d %d %d", WAVE_RANK, tag, type, length);
#else
		sprintf(buf1, "%d %ld %d %ld", WAVE_RANK, tag, type, length);
#endif
		//发送认证消息给发送方
		if(send(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
			printf("node %d send message(认证步骤1：发送自己的rank,tag,type,length失败)\n", WAVE_RANK);
			exit(0);
		}

		//从发送方接收认证消息的确认
		if(recv(server_socket, buf1, WAVE_ACMESSAGE_LENGTH, 0) == -1){
			printf("node %d send message(认证步骤2：接收提示消息失败)\n", WAVE_RANK);
			exit(0);
		}
#ifdef DEBUG
	printf("node %d send message ac confirm %s\n", WAVE_RANK, buf1);
#endif		
		//处理确认信息
		{
			//通信双方认证成功，身份正确
			if(strcmp(buf1, "Success_send") == 0){
				if(WAVE_SendMessage(server_socket, orimessage, WAVE_SizeOf(type)*length) == 0){
					printf("node %d send message fail in the last step\n", WAVE_RANK);
					exit(0);
				}
				//断开连接
				close(server_socket);
				break;
			}
			else{//通信双方认证失败，身份不匹配
				//进程编号不匹配，说明发送方此时还不想给自己发信息，需要不断再请求
				if(strcmp(buf1, "Fail_rank") == 0){
					//断开连接
					close(server_socket);
					usleep(WAVE_CLOCK);
					continue;
				}
				//通信双方标签不匹配，说明程序有错
				if(strcmp(buf1, "Fail_tag") == 0){
					printf("error in your program that node %d send and node %d recv message tag is different\n", WAVE_RANK, dest);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
				//通信双方数据类型不匹配，说明程序有错
				if(strcmp(buf1, "Fail_type") == 0){
					printf("error in your program that node %d send and node %d recv message type is different\n", WAVE_RANK, dest);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
				//通信双方数据长度不匹配，说明程序有错
				if(strcmp(buf1, "Fail_length") == 0){
					printf("error in your program that node %d send and node %d recv message length is different\n", WAVE_RANK, dest);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
				//此时其它进程都在同步，自己却在send，说明程序有错
				if(strcmp(buf1, "Fail_barrier") == 0){
					printf("error in your program that node %d send and other nodes are barrier\n", WAVE_RANK);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
				//此时其它进程都在归约，自己却在send，说明程序有错
				if(strcmp(buf1, "Fail_reduc") == 0){
					printf("error in your program that node %d send and other nodes are reduction\n", WAVE_RANK);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
				//此时其它进程都在广播，自己却在send，说明程序有错
				if(strcmp(buf1, "Fail_bcast") == 0){
					printf("error in your program that node %d send and other nodes are bcast\n", WAVE_RANK);
					//断开连接
					close(server_socket);
					//释放申请的认证信息缓冲区
					free(buf1);
					exit(0);
					return 0;
				}
			}//end if
		} // 
	}//end while
#ifdef DEBUG
	printf("node %d send to node %d end\n", WAVE_RANK, dest);
#endif	
	//释放空间
	free(buf1);
	return 1;
}


//返回所有进程数目
int WAVE_Size(int *size){
        *size = WAVE_SIZE;
        if(WAVE_SIZE != -1)
                return 1;
        else
                return 0;
}


//返回进程全局编号
int WAVE_Rank(int *rank){
	*rank = WAVE_RANK;
	if(WAVE_RANK != -1)	
		return 1;
	else
		return 0;
}


//初始化函数
int WAVE_Init(int& argc, char *argv[]){
	int i, j, k;
	//输入参数的后三个（rank, size, serverIP, myIP）
	WAVE_RANK = atoi(argv[argc-4]); 
	WAVE_SIZE = atoi(argv[argc-3]);
	char *server_ip=argv[argc-2];  // 要连接的服务器的 IP 地址
	char *my_ip=argv[argc-1];      // 自己的 IP 地址
	argv[argc-4] = NULL;
	argv[argc-3] = NULL;
	argv[argc-2] = NULL;
	argv[argc-1] = NULL;
	argc = argc - 4;
	unsigned int port = 14080;     //服务器端口号
	int buffcellsize = 50;
	char *bufcell1 = (char *)malloc(sizeof(char)*buffcellsize);
	char *bufcell2;
	char *bufcell3;
	int buffsize = buffcellsize*WAVE_SIZE;             				//收发数据缓冲区大小
	char *buf=(char *)malloc(sizeof(char)*buffsize);                //收发数据缓冲区
	char *bufcurrent;
	
	//申请保存所有进程的IP和端口信息所用的内存
	WAVE_NODES = (struct WAVE_node_info *)malloc(sizeof(struct WAVE_node_info)*WAVE_SIZE);
	for(i=0; i<WAVE_SIZE; i++)
		WAVE_NODES[i].ip = (char *)malloc(sizeof(char)*15);
	memcpy(WAVE_NODES[0].ip, server_ip, strlen(server_ip));
	WAVE_NODES[0].port = port;
	
	if(WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH < WAVE_SIZE)
		WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH = WAVE_SIZE;
	WAVE_EPOLL_EVENTS_NUM = WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH;
	
	
	//0号进程临时充当服务器，其余进程为客户端进程
	if(WAVE_RANK == 0){
		size_t initSize = WAVE_SIZE - 1;   //初始化其余的目标进程数目
		int client_socket;						//客户端套接口
		socklen_t client_size = sizeof(struct sockaddr);         //客户端网络地址长度
		struct sockaddr_in client_in;  //客户端网络地址
		int nfds;
		size_t client_num=0; //已接受的客户端数
		size_t client_end=0;  //已结束初始化的客户端数
		size_t client_accept = 0;   //建立连接的客户端数目
		
		//申请 TCP/IP 协议的套接口
		if((WAVE_SERVER_SOCKET = socket(AF_INET, SOCK_STREAM, 0)) == -1 ){
			printf("server(%s) call to socket fail\n", server_ip);
			exit(0);
		}
		//初始化 IP 地址
		bzero(&WAVE_SERVER_IN, sizeof(WAVE_SERVER_IN));  //清 0
		WAVE_SERVER_IN.sin_family = AF_INET;
		WAVE_SERVER_IN.sin_addr.s_addr = INADDR_ANY;
		WAVE_SERVER_IN.sin_port = htons(port);
		//套接口捆定 IP 地址
		if(bind(WAVE_SERVER_SOCKET, (struct sockaddr *)&WAVE_SERVER_IN, sizeof(WAVE_SERVER_IN)) == -1){
			printf("server(%s %d) call to bind fail\n", server_ip, port);
			exit(0);
		}
		//监听网络请求
		if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
			printf("server(%s) call to listen fail\n", server_ip);
			exit(0);
		}
		
		//声明epoll_event结构体的变量,ev用于注册事件,数组用于回传要处理的事件
		struct epoll_event ev;
		struct epoll_event *events =(struct epoll_event *)malloc(sizeof(struct epoll_event)*WAVE_EPOLL_EVENTS_NUM);
		//生成用于处理accept的epoll专用的文件描述符
		int epfd = epoll_create(WAVE_SIZE);
		//设置与要处理的事件相关的文件描述符
		ev.data.fd = WAVE_SERVER_SOCKET;
		//设置要处理的事件类型
		ev.events = EPOLLIN;
		//注册epoll事件
		epoll_ctl(epfd, EPOLL_CTL_ADD, WAVE_SERVER_SOCKET, &ev);
		
#ifdef DEBUG
	printf("server(%s) Accepting connections ...\n", server_ip);
#endif
		while(client_end < initSize){
			//等待epoll事件的发生
			nfds = epoll_wait(epfd, events, WAVE_EPOLL_EVENTS_NUM, -1);
			
			//处理所发生的所有事件
			for(i=0; i<nfds; i++)
			{
				//如果新监测到一个SOCKET用户连接到了绑定的SOCKET端口，建立新的连接。
				if(client_accept < initSize && events[i].data.fd == WAVE_SERVER_SOCKET)
				{
					//接收客户请求
					if((client_socket = accept(WAVE_SERVER_SOCKET, (struct sockaddr*)&client_in, &client_size))==-1){
						printf("server %d call to accept fail \n", WAVE_RANK);
						exit(0);
					}
#ifdef DEBUG
					char *str = inet_ntoa(client_in.sin_addr);
					printf("server %d accept a connection from %s\n", WAVE_RANK, str);
#endif

					//非阻塞方式
					WAVE_SetNonBlocking(client_socket);
					ev.data.fd = client_socket;
					//设置用于注册的读操作事件
					ev.events=EPOLLIN;
					//注册ev
					epoll_ctl(epfd, EPOLL_CTL_ADD, client_socket, &ev);
					client_accept++;
				}
				//如果是已经连接的用户，读入认证消息
				else if(events[i].events & EPOLLIN)
				{
					client_socket = events[i].data.fd;
					//获取客户发送的数据
					if(recv(client_socket, bufcell1, buffcellsize, 0) == -1){
						printf("server(%s) call to recv fail \n",server_ip);
						exit(0);
					}

					j = (int)strtol(bufcell1, &bufcell2, 10); //进程编号
					WAVE_NODES[j].port = (int)strtol((char *)(bufcell2+1), &bufcell3, 10); //进程所使用的端口号
					memcpy(WAVE_NODES[j].ip, (char *)(bufcell3+1), strlen((char *)(bufcell3+1)));//进程所在节点IP
#ifdef DEBUG
	printf("server(%s) received from client%d: %s\n",server_ip, j, bufcell1);
#endif
					
					//设置用于注测的写操作事件
					ev.data.fd = client_socket;
					//修改client_socket上要处理的事件为EPOLLOUT
					ev.events = EPOLLOUT;
					epoll_ctl(epfd, EPOLL_CTL_MOD, client_socket, &ev);
					client_num++;
				}
				else if(client_num == initSize && (events[i].events & EPOLLOUT))// 如果有数据发送
				{
					client_socket = events[i].data.fd;
					
					//初始化发送消息
					if(client_end == 0){
						bufcurrent = buf;
						//整理要发送的信息
						for(j=0; j<WAVE_SIZE; j++){
							sprintf(bufcurrent, "%s:%d|", WAVE_NODES[j].ip, WAVE_NODES[j].port);
							bufcurrent = (char *)(buf + strlen(buf));
						}
#ifdef DEBUG
	printf("server send to node:%s\n", buf);
#endif
					}
					
					//发送给其余所有进程
					//if(send(client_socket, buf, buffsize, 0) == -1){
					if(WAVE_SendMessage(client_socket, buf, buffsize) == 0){
						printf("server(%s) call to send other node fail\n", server_ip);
						exit(0);
					}
					
					close(client_socket);
					client_end++;	
				}//end if out
			}//end for
		}//end while
		
		//释放申请的空间
		close(epfd);
		free(events);
	}
	else{
		//尝试绑定一个端口以便以后用
			unsigned int my_port = port + WAVE_RANK;
			//申请 TCP/IP 协议的套接口
			if((WAVE_SERVER_SOCKET = socket(AF_INET, SOCK_STREAM, 0)) == -1 ){
				printf("server(%s) call to socket fail\n", server_ip);
				exit(0);
			}
			//初始化 IP 地址
			bzero(&WAVE_SERVER_IN, sizeof(WAVE_SERVER_IN));  //清 0
			WAVE_SERVER_IN.sin_family = AF_INET;
			WAVE_SERVER_IN.sin_addr.s_addr = INADDR_ANY;
			WAVE_SERVER_IN.sin_port = htons(my_port);
			//套接口捆定 IP 地址
			while(bind(WAVE_SERVER_SOCKET, (struct sockaddr *)&WAVE_SERVER_IN, sizeof(WAVE_SERVER_IN)) == -1){
				my_port += 20;
				bzero(&WAVE_SERVER_IN, sizeof(WAVE_SERVER_IN));  //清 0
				WAVE_SERVER_IN.sin_family = AF_INET;
				WAVE_SERVER_IN.sin_addr.s_addr = INADDR_ANY;
				WAVE_SERVER_IN.sin_port = htons(my_port);
			}
			//监听网络请求
			if(listen(WAVE_SERVER_SOCKET, WAVE_SERVER_SOCKET_LISTEN_QUEUE_LENGTH) == -1){
				printf("node %d call to listen fail\n", WAVE_RANK);
				exit(0);
			}
		
		int server_socket;				 //服务器套接口
		struct sockaddr_in server_in;    //服务器网络地址
		struct hostent *server_name;
		
		//转换服务器域名或 IP 地址为 IP 结构体
		if((server_name = gethostbyname(server_ip)) == 0){
			printf("node %d error resolving local host\n", WAVE_RANK);
			exit(0);
		}
		//初始化 IP 地址结构
		bzero(&server_in,sizeof(server_in));
		server_in.sin_family = AF_INET;
		server_in.sin_addr.s_addr = htonl(INADDR_ANY);
		server_in.sin_addr.s_addr = ((struct in_addr*)(server_name->h_addr))->s_addr;
		server_in.sin_port = htons(port);
		
		//获取远程服务器套接口描述符
		if((server_socket = socket(AF_INET,SOCK_STREAM,0)) == -1){
			printf("node %d call to socket fail\n", WAVE_RANK);
			exit(0);
		}
		//发送连接请求到到服务器
		while(connect(server_socket, (struct sockaddr *)&server_in, sizeof(server_in)) == -1){
			usleep(WAVE_CLOCK);
		}
		
		sprintf(bufcell1, "%d:%d:%s", WAVE_RANK, my_port, my_ip);
		//发送数据到服务器
		if(send(server_socket, bufcell1, buffcellsize, 0) == -1){
			printf("node %d error in send \n", WAVE_RANK);
			exit(0);
		}
#ifdef DEBUG
	printf("node %d sending message: %s \n",WAVE_RANK, bufcell1);
#endif
		//从服务器接收数据
		//if(recv(server_socket, buf, buffsize, 0) == -1){
		if(WAVE_RecvMessage(server_socket, buf, buffsize) == 0){
			printf("node %d error in recv \n", WAVE_RANK);
			exit(0);
		}
		
#ifdef DEBUG
	printf("node %d get respone from Server: %s \n",WAVE_RANK, buf);
#endif
		j=0;
		for(i=0; i<WAVE_SIZE; i++){
			//读取IP
			k = 0;
			while(buf[j]!=':'){
				WAVE_NODES[i].ip[k] = buf[j];
				j++;
				k++;
			}

			j++; // buf[j]=':'
			k=0;
			while(buf[j] != '|'){
				bufcell1[k] = buf[j];
				j++;
				k++;
			}
			bufcell1[k] = '|';
			j++;  // buf[j]='|'
			WAVE_NODES[i].port = (unsigned int)atoi(bufcell1);
			
		}
		//断开连接
		close(server_socket);
	}
#ifdef INFO
	if(WAVE_RANK == 1){
		printf("THE INFORMATION(ip, port) OF THE NODE\n");
		for(i=0; i<WAVE_SIZE; i++)
			printf("node:%d ip:%s port:%d\n",i,WAVE_NODES[i].ip,WAVE_NODES[i].port);
		printf("\n\n");
	}
#endif
	
	//free the memory
	free(bufcell1);
	free(buf);
#ifdef DEBUG
	printf("node %d in %d end init\n", WAVE_RANK, WAVE_SIZE);
#endif
	return 1;
}


//收尾函数
int WAVE_Finalize(){
	int i;
	close(WAVE_SERVER_SOCKET);
	//释放保存所有进程的IP和端口信息所用的内存
	if(WAVE_NODES != NULL){
		for(i=0; i<WAVE_SIZE; i++)
			if(WAVE_NODES[i].ip != NULL)
				free(WAVE_NODES[i].ip);
		free(WAVE_NODES);
		return 1;
	}
	else{
		return 0;
	}
#ifdef DEBUG
	printf("node %d in %d has successlly finalized!!\n", WAVE_RANK, WAVE_SIZE);
#endif
}




















/* Author: cos
*  date: 2015.8
*  参照MPI实现的分布式计算框架的启动程序，fork出多条进程
*  
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>


//打开输出调试信息
//#define DEBUG
//打开输出节点IP和端口信息
//#define INFO

int main(int argc, char* argv[]){
#ifdef INFO
	printf("fork process in the different node must provide the node ip file(ip.txt)\n");
	printf("else we will start al the procs in the same node\n");
        printf("example of ip.txt:\n127.0.0.1\n167.3.3.3\n156.3.3.3\n\n");
#endif
	// check the input arguments
	if(argc < 3){
		printf("INPUT ARGUMENTS AT LEAST TWO(the number of process and the name of exe)\n");
		printf("example: ./main 9 ./exe\n\n");
		exit(0);
	}
	// the number of process
	int procs = atoi(argv[1]);
	// the name of the exe
	char *exe = argv[2];
	int length=0;
	for(int i=2; i<argc; i++){
		length+=strlen(argv[i]);
		length+=1;
	}	
	length--;
	char *EXE = (char *)malloc(sizeof(char)*length);
	char *EXE_current=EXE; 
	for(int i=2; i<argc-1; i++){
		memcpy(EXE_current, argv[i], strlen(argv[i]));
		EXE_current = (char *)(EXE_current+strlen(argv[i]));
		*EXE_current = ' ';
		EXE_current = (char *)(EXE_current+1);
	}
	memcpy(EXE_current, argv[argc-1], strlen(argv[argc-1]));


#ifdef DEBUG
	printf("start %d procs to execute %s ", procs, exe);
#endif
	
	FILE *fp=fopen("ip.txt", "r");
	//same node mode
	if(fp == NULL){
#ifdef DEBUG
	printf("on the current node \n");
#endif
//		system("echo '127.0.0.1' > ip.txt");
		int pid=-1;
		int* PID_array = (int *)malloc(sizeof(int)*procs);
		int rank = -1;
		//fork new process
		for(int i=0; i<procs; i++){
			pid = fork();
			if(pid == 0){
				rank=i;
				break;
			}
			else{
				PID_array[i] = pid;
			}
		}
		if(pid == 0){
			char * EXE_ALL=(char *)malloc(sizeof(char)*(strlen(EXE)+1+10+1+10+20));
			sprintf(EXE_ALL, "%s %d %d 127.0.0.1 127.0.0.1", EXE, rank, procs);
#ifdef DEBUG
	printf("%s\n", EXE_ALL);
#endif
			system(EXE_ALL);
			free(EXE_ALL);
		}
		else{
/*
			sleep(2);
			char * KILL=(char *)malloc(sizeof(char)*15);
			for(int i=0; i<procs; i++){
				sprintf(KILL, "kill -9 %d", PID_array[i]);
#ifdef DEBUG
	printf("%s\n",KILL);
#endif
				system(KILL);
				memset(KILL, 0, 15);
			}
			free(KILL);
*/
			for(int i=0; i<procs; i++)
				waitpid(PID_array[i], NULL, 0);
		}
		free(PID_array);			
	}
	else{
#ifdef DEBUG
        printf("on the different node \n");
#endif
		//input the node ip information
		int max_differentNodes = 20;
		char **IP = (char **)malloc(sizeof(char *)*max_differentNodes);
		IP[0]=(char *)malloc(sizeof(char)*15);
		int IP_count=0;
		while(fscanf(fp, "%s", IP[IP_count++]) != -1){
			if(IP_count < max_differentNodes)
				IP[IP_count] = (char *)malloc(sizeof(char)*15);
			else{
				char **oldIP = IP;
				max_differentNodes+=5;
				IP = (char **)malloc(sizeof(char *)*max_differentNodes); 
				for(int i=0; i<max_differentNodes; i++){
					IP[i] = oldIP[i];
				}
				free(oldIP);
				IP[IP_count] = (char *)malloc(sizeof(char)*15);
			}
		}
		IP_count--;	
#ifdef INFO	
		printf("THE CONTENT OF ip.txt\n");
		for(int i=0; i<IP_count; i++){
			printf("%s\n", IP[i]);
		}	
		printf("\n");	
#endif
		//fork new process
		int* PID_array = (int *)malloc(sizeof(int)*procs);
		int pid = -1;
		int rank = -1;
		for(int i=0; i<procs; i++){
			pid = fork();
			if(pid == 0){
				rank=i;
				break;
			}
			else{
				PID_array[i] = pid;
			}
		}
		
		if(pid == 0){
			char * EXE_ALL=(char *)malloc(sizeof(char)*(strlen(EXE)+2*strlen(IP[rank%IP_count])+strlen(IP[0])+32));
                        sprintf(EXE_ALL, "ssh -X %s %s %d %d %s %s",IP[rank%IP_count], EXE, rank, procs, IP[0], IP[rank%IP_count]);
#ifdef DEBUG
        printf("%s\n", EXE_ALL);
#endif
                        system(EXE_ALL);
                        free(EXE_ALL);
		}
		else{
/*			sleep(2);
                        char * KILL=(char *)malloc(sizeof(char)*15);
                        for(int i=0; i<procs; i++){
                                sprintf(KILL, "kill -9 %d", PID_array[i]);
#ifdef DEBUG
        printf("%s\n",KILL);
#endif
                                system(KILL);
                                memset(KILL, 0, 15);
                        }
                        free(KILL);
*/
			for(int i=0; i<procs; i++)
				waitpid(PID_array[i], NULL, 0);
		}

		// free the memmory
		if(fp != NULL)	
			fclose(fp);
		for(int i=0; i<max_differentNodes; i++)
				free(IP[i]);
		free(IP);
		free(PID_array);
	}
	
	//free the alloc memory
	free(EXE);
	return 0;
}					

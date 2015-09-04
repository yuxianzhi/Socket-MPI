#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "WAVE.h"

int main(int argc, char *argv[]){
	int rank, size, i;
	WAVE_Init(argc, argv);
	WAVE_Rank(&rank);
	WAVE_Size(&size);
	printf("rank: %d size: %d\n", rank, size);
	char *temp1 = (char *)malloc(sizeof(char)*30);
	int *temp2 = (int *)malloc(sizeof(int)*30);
	unsigned int *temp3 = (unsigned int *)malloc(sizeof(unsigned int)*30);
	float *temp4 = (float *)malloc(sizeof(float)*30);
	double *temp5 = (double *)malloc(sizeof(double)*30);
	int *temp6 = (int *)malloc(sizeof(int)*30);
	int *temp7 = (int *)malloc(sizeof(int)*30);
	int *temp8 = (int *)malloc(sizeof(int)*30);
	double *temp9 = (double *)malloc(sizeof(double)*4000000);
	if(rank ==0 )
		sleep(20);
	    WAVE_Barrier();	



	for(i=0; i<30; i++)
		temp7[i]= i;
	WAVE_Reduce(temp7, temp8, 30, WAVE_INT, WAVE_ADD, 0);
	if(rank == 1){
		for(i=0; i<30; i++)
			temp1[i]=(char)(i+'0');
		for(i=0; i<30; i++)
			temp2[i]=i;
		for(i=0; i<30; i++)
			temp3[i]=i;
		for(i=0; i<30; i++)
			temp4[i]=i*0.111111;
		for(i=0; i<30; i++)
			temp5[i]=i*0.111111;
		for(i=0; i<30; i++)
			temp6[i]=i+50;
		for(i=0; i<4000000; i++)
			temp9[i]=i*0.111111;
	}
	WAVE_Bcast(temp6, 30, WAVE_INT, 1);
	if(rank == 1){
		WAVE_Send(temp1, 30, WAVE_CHAR, 0, 1);
		WAVE_Send(temp2, 30, WAVE_INT, 0, 1);
		WAVE_Send(temp3, 30, WAVE_UINT, 0, 1);
		WAVE_Send(temp4, 30, WAVE_FLOAT, 0, 1);
		WAVE_Send(temp5, 30, WAVE_DOUBLE, 0, 1);
		WAVE_Send(temp9, 4000000, WAVE_DOUBLE, 0, 1);
	}
	if(rank == 0){
		WAVE_Recv(temp1, 30, WAVE_CHAR, 1, 1,NULL);
		WAVE_Recv(temp2, 30, WAVE_INT, 1, 1,NULL);
		WAVE_Recv(temp3, 30, WAVE_UINT, 1, 1,NULL);
		WAVE_Recv(temp4, 30, WAVE_FLOAT, 1, 1,NULL);
		WAVE_Recv(temp5, 30, WAVE_DOUBLE, 1, 1,NULL);
		WAVE_Recv(temp9, 4000000, WAVE_DOUBLE, 1, 1,NULL);
	}
else{
}
	if(rank == 0){
		for(i=0;i<30; i++){
			printf("%c ",temp1[i]);
			printf("%d ",temp2[i]);
			printf("%u ",temp3[i]);
			printf("%f ",temp4[i]);
			printf("%lf ",temp5[i]);
			printf("%d ",temp6[i]);
			printf("%d ",temp8[i]);
		printf("\n");
		}

		for(i=0;i<4000000; i++){
			if(temp9[i]<1)
			printf("%d  %lf \n",i, temp9[i]);
		}
		printf("\n");
	}
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);
	WAVE_Barrier();
	WAVE_Barrier();
	printf("rank: %d size: %d\n", rank, size);

	WAVE_Finalize();

	return 0;
}

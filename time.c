#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main (int argc, char* argv[])
{
    if(argc < 2)
    {
        printf(2,"time : Not enough arguments\n");
        exit();
    }
    else
    {
        int pid;
        pid = fork();
        if(pid<0)
        {
            printf(2,"time : Fork failed\n");
            exit();
        }
        if(pid==0)
        {
            if(exec(argv[1], argv + 1) < 0)
            {
                printf(2,"time: could not exec\n");
            }
            exit();
        }
        else
        {
            int wtime, rtime;
            if(waitx(&wtime, &rtime) == -1)
            {
                printf(2,"time: Child process not found\n");
                exit();
            }
            printf(1,"Process %s\n \t Running Time : %d \n \t Waiting Time : %d \n",argv[1],rtime,wtime);
            exit();
        }
        
    }
    
}
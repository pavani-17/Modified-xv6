#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(void)
{
    int i, pid;
    for(i=0;i<10;i++)
    {
        pid = fork();
        if(pid != 0)
        {
            int cur_pid = getpid();
            printf(2,"%d started\n",cur_pid);
            int j;
            for(j=0;j<1000000;j++);
            printf(2,"Round 1 done %d\n",cur_pid);
            for(j=0;j<1000000;j++);
            printf(2,"Round 2 done %d\n",cur_pid);
            for(j=0;j<1000000;j++);
            printf(2,"Round 3 done %d\n",cur_pid);
            printf(2,"%d ended\n",cur_pid);
            exit();
        }
    }
    for(i=0;i<10;i++)
    {
        wait();
    }
}
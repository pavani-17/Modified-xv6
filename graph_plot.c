#include "types.h"
#include "user.h"

int number_of_processes = 7;

int main(int argc, char *argv[])
{
  int j;
  for (j = 0; j < number_of_processes; j++)
  {
    int pid = fork();
    if (pid < 0)
    {
      printf(1, "Fork failed\n");
      continue;
    }
    if(pid==0){
      volatile int i;
      volatile int k;
      for (k = 0; k < number_of_processes; k++)
      {
        if ((k+j) % 2)
        {
          sleep(20); //io time
        }
        else
        {
          for (i = 0; i < 100000000; i++)
          {
            ; //cpu time
            for (i = 0; i < 100000000; i++)
            {
              ; //cpu time
              for (i = 0; i < 100000000; i++)
              {
                ; //cpu time
              }
            }
          }
        }
      }
      exit();
    }
  }
  for (j = 0; j < number_of_processes; j++){
    int wtime; int rtime;
    //int pid =
    waitx(&wtime, &rtime);
    // printf(1,"%d has waiting ticks = %d && running ticks = %d\n",pid, wtime, rtime);
  }
  exit();
}
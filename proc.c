#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  // acquire(&tickslock);
  p->ctime = ticks;
  // release(&tickslock);
  p->rtime = 0;
  #if SCHEDULER == SCHED_PBS
  p->priority = 60;
  #else
  p->priority = -1;
  #endif
  p->n_run = 0;
  p->w_time = 0;
  p->tw_time = 0;
  #if SCHEDULER == SCHED_MLFQ
  p->cur_q = 0;
  p->n_ticks = 0;
  p->q[0] = 0;
  p->q[1] = 0;
  p->q[2] = 0;
  p->q[3] = 0;
  p->q[4] = 0;
  #else
  p->cur_q = -1;
  p->n_ticks = -1;
  p->q[0] = -1;
  p->q[1] = -1;
  p->q[2] = -1;
  p->q[3] = -1;
  p->q[4] = -1;
  #endif
  release(&ptable.lock);

  //#ifdef GRAPH
  //cprintf("%d %d %d\n",p->pid, p->ctime, p->cur_q);
  //#endif

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  #if SCHEDULER == SCHED_MLFQ
  //cprintf("%d %d %d \n",p->pid, ticks, p->cur_q);
  push_queue(p->cur_q, p->pid);
  #endif

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  #if SCHEDULER == SCHED_MLFQ
  //cprintf("%d %d %d \n",np->pid, ticks, np->cur_q);
  push_queue(np->cur_q, np->pid);
  #endif

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }
  //acquire(&tickslock);
  curproc->etime = ticks;
  // release(&tickslock);

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  //#ifdef GRAPH
  //cprintf("%d %d %d\n",curproc->pid, curproc->etime, curproc->cur_q);
  //#endif
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  #if SCHEDULER == SCHED_RR
  struct proc *p;
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->w_time = 0;
      p->n_run ++;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }

  #elif SCHEDULER == SCHED_FCFS
  struct proc *p;
  int create_time;
  struct proc* selected;
  for(;;){
    selected = 0;
    create_time = ticks + 5; 
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Search for minimum creation time
      if(create_time > p->ctime)
      {
        create_time = p->ctime;
        selected = p;
      }
    }
    // Execute the above selected process
    if(selected != 0)
    {
      //cprintf("Process with pid %d running on CPU %d\n",selected->pid,c->apicid);
      c->proc = selected;
      switchuvm(selected);
      selected->state = RUNNING;
      selected->w_time = 0;
      selected->n_run ++;
      swtch(&(c->scheduler), selected->context);
      switchkvm();
    }
    c->proc = 0;
    release(&ptable.lock);
  }

  #elif SCHEDULER == SCHED_PBS
  struct proc *p;
  int priority, max_wait;
  struct proc* selected;
  for(;;){
    selected = 0;
    priority = 101;
    max_wait = ticks;
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Search for minimum priority
      if(priority > p->priority)
      {
        priority = p->priority;
        selected = p;
        max_wait = p->w_time;
      }
      else if (priority == p->priority)
      {
        if(max_wait < p->w_time)
        {
          priority = p->priority;
          selected = p;
          max_wait = p->w_time;
        }
      }
    }
    // Execute the above selected process
    if(selected != 0)
    {
      //cprintf("Process with pid %d and priority %d running on CPU %d\n",selected->pid,selected->priority,c->apicid);
      c->proc = selected;
      switchuvm(selected);
      selected->state = RUNNING;
      selected->w_time = 0;
      selected->n_run ++;
      swtch(&(c->scheduler), selected->context);
      switchkvm();
    }
    c->proc = 0;
    release(&ptable.lock);
  }
  
  #elif SCHEDULER == SCHED_MLFQ
  struct proc* selected;
  struct proc* p;
  int selected_pid = -1;
  int i;
  for(;;)
  {
    sti();
    selected = 0;
    selected_pid = -1;
    acquire(&ptable.lock);
    for(i=0;i<5;i++)
    {
      if(top_queue(i) != -1)
      {
        selected_pid = top_queue(i);
        break;
      }
    }
    if(selected_pid == -1)
    {
      release(&ptable.lock);
      continue;
    }
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid == selected_pid)
      {
        selected = p;
        break;
      }
    }
    pop_queue(selected->cur_q);
    if(selected != 0)
    {
      //cprintf("Selected process with pid %d in queue %d\n",selected->pid,selected->cur_q);
      c->proc = selected;
      switchuvm(selected);
      selected->state = RUNNING;
      selected->n_ticks = 0;
      selected->w_time = 0;
      selected->n_run ++;
      swtch(&(c->scheduler), selected->context);
      switchkvm();
    }
    c->proc = 0;
    release(&ptable.lock);
  }
  #else
  while(1);
  #endif
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  #if SCHEDULER == SCHED_MLFQ
    push_queue(myproc()->cur_q, myproc()->pid);
  #endif
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
      #if SCHEDULER == SCHED_MLFQ
        push_queue(p->cur_q, p->pid);
        //cprintf("%d %d %d \n",p->pid, ticks, p->cur_q);
      #endif
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
      {
        p->state = RUNNABLE;
        #if SCHEDULER == SCHED_MLFQ
        push_queue(p->cur_q, p->pid);
        cprintf("%d %d %d \n",p->pid, ticks, p->cur_q);
        #endif
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s %d", p->pid, state, p->name, p->cur_q);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// If it finds a process, it returns the rtime (running time) and wtime (waiting time) of the exited child process
int
waitx(int* wtime, int* rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        *rtime = p->rtime;
        *wtime = p->tw_time;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Increment the runtime of all RUNNING processes
// Increment the waittime of all RUNNABLE processes
void 
update_times()
{
  struct proc* p;
  acquire(&ptable.lock);
  for(p=ptable.proc;p < &ptable.proc[NPROC];p++)
  {
    if(p->state == RUNNING)
    {
      p->rtime ++;
      #if SCHEDULER == SCHED_MLFQ
      p->q[p->cur_q]++;
      p->n_ticks++;
      #endif 
    }
    if(p->state == RUNNABLE)
    {
      p->tw_time++;
      p->w_time++;
      #if SCHEDULER == SCHED_MLFQ
      //cprintf("%d %d\n",p->w_time, queues_aging[p->cur_q]);
      if(p->w_time > queues_aging[p->cur_q] && p->cur_q != 0)
      {
        pop_pid_queue(p->cur_q, p->pid);
        p->cur_q --;
        //#ifdef GRAPH
        //cprintf("%d %d %d\n",p->pid, ticks, p->cur_q);
        //#endif
        push_queue(p->cur_q,p->pid);
        p->w_time = 0;
        //cprintf("Process %d went to queue %d due to aging \n",p->pid, p->cur_q);
      }
      #endif
    }
  }
  release(&ptable.lock);
}

// Change the priority of the a process (For priority based scheduling)
int
set_priority (int new_priority, int pid)
{
  struct proc* p;
  int old_priority = 101;
  // acquire(&ptable.lock);
  for(p=ptable.proc; p < &ptable.proc[NPROC];p++)
  {
    if(p->pid == pid)
    {
      old_priority = p->priority;
      p->priority = new_priority;
      break;
    }
  }
  if(old_priority == 101)
  {
    cprintf("No process with pid %d \n",pid);
    // release(&ptable.lock);
    return -1;
  }
  //release(&ptable.lock);
  if(old_priority > new_priority) // If priority increases, reschedule
  {
    yield();
  }
  return old_priority;
}

// Push a process into the queue
void
push_queue (int q_no, int p)
{
  queues[q_no][++queues_tails[q_no]] = p;
  return;
}

// Pop a process from the queue
void 
pop_queue (int q_no)
{
  int i;
  if(queues_tails[q_no] == -1)
  {
    return;
  }
  for(i=0; i<= queues_tails[q_no] - 1; i++)
  {
    queues[q_no][i] = queues[q_no][i+1];
  }
  queues_tails[q_no]--;
  return;
}

// Pop process based on pid
void
pop_pid_queue (int q_no, int pid)
{
  int i;
  if(queues_tails[q_no] == -1)
  {
    return;
  }
  for(i=0;i<=queues_tails[q_no];i++)
  {
    if(queues[q_no][i] == pid)
    {
      break;
    }
  }
  for(;i<= queues_tails[q_no] - 1; i++)
  {
    queues[q_no][i] = queues[q_no][i+1];
  }
  queues_tails[q_no]--;
  return;
}

// Return the first element of the queue
int
top_queue (int q_no)
{
  if(queues_tails[q_no] == -1)
  {
    return -1;
  }
  else
  {
    return queues[q_no][0];
  }
}

// Initialise the queue
void 
q_init()
{
  int i;
  for(i=0;i<5;i++)
  {
    queues_tails[i] = -1;
    switch(i)
    {
      case 0:
        queues_maxticks[i] = 1;
        queues_aging[i] = -1;
        break;
      case 1:
        queues_maxticks[i] = 2;
        queues_aging[i] = 10;
        break;
      case 2:
        queues_maxticks[i] = 4;
        queues_aging[i] = 20;
        break;
      case 3:
        queues_maxticks[i] = 8;
        queues_aging[i] = 30;
        break;
      case 4:
        queues_maxticks[i] = 16;
        queues_aging[i] = 40;
        break;
    }
  }
}

// ps Implementation
void
proc_info ()
{
  struct proc* p;
  cprintf(" PID  Priority  State    r_time w_time n_run  cur_q   q0   q1   q2   q3   q4 \n\n");
  static char *states[] = {
    [UNUSED]    "unused  ",
    [EMBRYO]    "embryo  ",
    [SLEEPING]  "sleeping",
    [RUNNABLE]  "runnable",
    [RUNNING]   "running ",
    [ZOMBIE]    "zombie  "
  };
  for(p=ptable.proc; p != &ptable.proc[NPROC]; p++)
  {
    if(p->state == UNUSED) continue;
    cprintf(" %d    %d        %s    %d      %d      %d     %d    %d    %d    %d    %d   %d\n",
      p->pid, p->priority, states[p->state], p->rtime, p->w_time, p->n_run, p->cur_q, p->q[0], p->q[1], p->q[2], p->q[3], p->q[4] );
  }
  return;
}
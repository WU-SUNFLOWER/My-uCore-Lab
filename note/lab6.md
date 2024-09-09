# 练习0：填写已有实验

## 代码迁移与修改

与lab5类似，lab6除了需要把之前实验的代码迁移过来之外，也要在上一个实验的基础上进行一些修改。

首先，进程控制块中又增加了新内容：

```C++
static struct proc_struct *
alloc_proc(void) {
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL) {
        // 略...
        proc->rq = NULL;                 // running queue contains Process
        list_init(&proc->run_link);      // the entry linked in run queue
        proc->time_slice = 0;            // time slice for occupying the CPU

        skew_heap_init(&proc->lab6_run_pool);    // FOR LAB6 ONLY: the entry in the run pool
        proc->lab6_stride = 0;                   // FOR LAB6 ONLY: the current stride of the process
        proc->lab6_priority = 0;                 // FOR LAB6 ONLY: the priority of process, set by lab6_set_priority(uint32_t)
    }
    return proc;
}
```

此外，从lab6开始操作系统对CPU时钟中断的响应处理，完全交由调度器接口完成：

```C++
static void
trap_dispatch(struct trapframe *tf) {
    // 略...
    switch (tf->tf_trapno) {
        // 略...
        case IRQ_OFFSET + IRQ_TIMER:
            ++ticks;
            assert(current != NULL);
            sched_class_proc_tick(current);
            break;
        // 略...
    }
}
```

这里用到了`sched_class_proc_tick`函数，但在`kern/schedule/sched.c`中该函数被定义为一个静态函数。我们需要去掉它的`static`关键字，否则编译会通不过！

## 修复uCore中cprintf的bug

uCore官方实现的提供给用户程序使用的`cprintf`函数实现是非原子的。这会导致什么问题呢？

举个例子，假设有两个进程A和B，如果进程A想打印字符串"Hello World"，进程B想打印字符串"Thanks for You"，则可能最终会在设备屏幕上输出"Hello Thanks for World"或者其他的混乱结果。这显然会影响我们正常开展本次实验，以及导致无法使用`make grade`对我们的代码进行评测！

这是因为uCore中用户进程每打印一个ASCII字符，都需要请求一次`SYS_putc`系统调用，因此虽然每次系统调用的执行是原子的，但这个用户程序在逐个打印字符的过程中仍然可能会被时钟中断，转而执行其他进程，从而导致屏幕输出结果的混乱。

这里我决定使用最简单粗暴的方式来修复这个bug——即给`cprintf`函数加一把大锁。

首先，由于lab6中uCore还没有实现锁机制，我们需要**在内核代码中**自行封装一个自旋锁。

代码如下，其中原子操作`__sync_lock_test_and_set`和`__sync_lock_release`由gcc编译器提供，我们无需关心其具体实现：

```C++
// kern/sync/my_spin_lock.h
// 这个结构体用于表示自旋锁
typedef struct {
    volatile int locked;
} spinlock_t;

void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

extern spinlock_t global_print_lock;

// kern/sync/my_spin_lock.c
#include <my_spin_lock.h>
#include <sched.h>

// 加锁函数
void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&(lock->locked), 1)) {
        schedule();  
    }
}

// 解锁函数
void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&(lock->locked));
}

// 初始化一个全局的自旋锁
spinlock_t global_print_lock = {0}; // 全局锁初始化为未锁定状态
```

> 请你自行思考以下问题：
>
> *   为什么不能在提供给用户程序的标准库中实现锁相关的代码？
> *   在`spin_lock`函数中，在锁被抢占的情况下，为什么我们不能直接让当前进程通过while死循环自旋，等待时钟中断的到来以被动放弃CPU使用权，而必须让它调用`schedule`函数主动放弃CPU控制权？

然后，我们把加锁/解锁函数封装成系统调用，以便用户程序使用：

```C++
// libs/unistd.h
#define SYS_print_lock 100
#define SYS_print_unlock 101
```

```C++
// kern/syscall/syscall.c
#include <my_spin_lock.h>

static int sys_print_lock(uint32_t arg[]) {
    spin_lock(&global_print_lock);
    return 0;
}

static int sys_print_unlock(uint32_t arg[]) {
    spin_unlock(&global_print_lock);
    return 0;
}

static int (*syscalls[])(uint32_t arg[]) = {
    // ...
    [SYS_print_lock]        sys_print_lock,
    [SYS_print_unlock]      sys_print_unlock,
};
```

添加用户程序库中的系统调用接口：

```C++
// user/libs/syscall.h
void sys_print_lock(void);
void sys_print_unlock(void);

// user/libs/syscall.c
void sys_print_lock(void) {
    syscall(SYS_print_lock);
}

void sys_print_unlock(void) {
    syscall(SYS_print_unlock);
}
```

最后，修改`cprintf`函数的底层代码：

```C++
// user/libs/stdio.c
int vcprintf(const char *fmt, va_list ap) {
    int cnt = 0;
    sys_print_lock();  // 加锁
    vprintfmt((void*)cputch, &cnt, fmt, ap);
    sys_print_unlock();  // 解锁
    return cnt;
}
```

# 练习1: 使用 Round Robin 调度算法

## 请理解并分析sched\_class中各个函数指针的用法，并结合Round Robin调度算法描述ucore的调度执行过程

### sched\_class与抽象接口

从lab6开始，uCore中支撑进程调度算法需要按`struct sched_class`结构体所描述的规范进行函数封装，并挂载到一个名为`default_sched_class`的该结构体的实例上，藉此以实现进程调度算法和uCore本体代码的分离。

该结构体的定义如下：

```C++
struct sched_class {
    // the name of sched_class
    const char *name;
    // Init the run queue
    void (*init)(struct run_queue *rq);
    // put the proc into runqueue, and this function must be called with rq_lock
    void (*enqueue)(struct run_queue *rq, struct proc_struct *proc);
    // get the proc out runqueue, and this function must be called with rq_lock
    void (*dequeue)(struct run_queue *rq, struct proc_struct *proc);
    // choose the next runnable task
    struct proc_struct *(*pick_next)(struct run_queue *rq);
    // dealer of the time-tick
    void (*proc_tick)(struct run_queue *rq, struct proc_struct *proc);
};
```

lab6中uCore默认使用的该结构体实例为：

```C++
struct sched_class default_sched_class = {
    .name = "RR_scheduler",
    .init = RR_init,
    .enqueue = RR_enqueue,
    .dequeue = RR_dequeue,
    .pick_next = RR_pick_next,
    .proc_tick = RR_proc_tick,
};
```

从这个结构体的定义中我们也能够大概猜测出来，uCore是如何将进程调度抽象成方便易用的接口的：

*   uCore会将**所有可供调度的进程**通过一个`struct run_queue`结构体实例（也就是uCore官方文档中所谓的"队列"）来进行管理。
*   当新创建一个进程，或者需要暂时暂停某个进程的执行转而执行其他进程时，就通过`enqueue`将它放到"队列"里。
*   执行调度时，首先通过`pick_next`决策接下来计算机要执行哪个进程，再通过`dequeue`从"队列"里取出要执行的这个进程。

> 这里我要给"队列"打引号，是因为根据调度器算法的不同，这个管理可供调度进程的容器不一定是个queue，还可能是红黑树或者其他更复杂的数据结构。

> 注意区分`struct run_queue`容器和前两个lab中涉及的全体进程链表`proc_list`！

通过查看`kern/schedule/sched.c`中`schedule`函数的实现，我们可以很容易地验证我们主观猜测的正确性：

```C++
void
schedule(void) {
    bool intr_flag;
    struct proc_struct *next;
    local_intr_save(intr_flag);
    {
        current->need_resched = 0;
        if (current->state == PROC_RUNNABLE) {
            sched_class_enqueue(current);
        }
        if ((next = sched_class_pick_next()) != NULL) {
            sched_class_dequeue(next);
        }
        if (next == NULL) {
            next = idleproc;
        }
        next->runs ++;
        if (next != current) {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}
```

## 基于Round Robin算法的uCore调度

根据已有的操作系统知识，我们不难意识到，在uCore中用户想要创建一个用户进程，最主要的方法是通过`SYS_fork`系统调用来实现的。这里我就从一个新进程的创建开始分析。

在`do_fork`中，创建完新进程后会为其调用`wakeup_proc`函数：

```C++
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf) {
    // 略...
    //    1. call alloc_proc to allocate a proc_struct
    struct proc_struct* child_proc = alloc_proc();
    // 略...
    //    6. call wakeup_proc to make the new child process RUNNABLE
    wakeup_proc(child_proc);
    // 略...
}
```

在`wakeup_proc`函数，会将这个新进程的状态标记为`PROC_RUNNABLE`，并且放进调度器"队列"当中去，这就为新进程被调度器调度执行创造了基本的条件：

```C++
void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE);
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        if (proc->state != PROC_RUNNABLE) {
            proc->state = PROC_RUNNABLE;
            proc->wait_state = 0;
            if (proc != current) {
                sched_class_enqueue(proc);
            }
        }
        else {
            warn("wakeup runnable process.\n");
        }
    }
    local_intr_restore(intr_flag);
}
```

进一步分析可知，在本实验中实际上操作系统会把我们的新进程给放到"队列"的队尾，并且将其的所剩时间片值拉满：

```C++
// kern/schedule/sched.c
static inline void
sched_class_enqueue(struct proc_struct *proc) {
    if (proc != idleproc) {
        sched_class->enqueue(rq, proc);
    }
}

// kern/schedule/default_sched.c
static void
RR_enqueue(struct run_queue *rq, struct proc_struct *proc) {
    assert(list_empty(&(proc->run_link)));
    list_add_before(&(rq->run_list), &(proc->run_link));
    if (proc->time_slice == 0 || proc->time_slice > rq->max_time_slice) {
        proc->time_slice = rq->max_time_slice;
    }
    proc->rq = rq;
    rq->proc_num ++;
}
```

> 在被uCore广泛使用的循环双向链表中，将节点插入到链表头节点的前方，相当于将该节点插入到链表的末尾。这体现了循环双向链表是极端好用的。

接下来的某个时刻，当前CPU正在执行的进程`current`被时钟中断，`sched_class_proc_tick`函数会被调用。而根据前面对`default_sched_class`结构体的分析可知，在本次实验中，正常情况下`RR_proc_tick`函数又会被进一步调用。

```C++
// kern/schedule/sched.c
void sched_class_proc_tick(struct proc_struct *proc) {
    if (proc != idleproc) {
        sched_class->proc_tick(rq, proc);  // RR_proc_tick(rq, proc)
    }
    else {
        proc->need_resched = 1;
    }
}

// kern/trap/trap.c
static void trap_dispatch(struct trapframe *tf) {
    // 略...
    switch (tf->tf_trapno) {
        // 略...
        case IRQ_OFFSET + IRQ_TIMER:
            ++ticks;
            assert(current != NULL);
            sched_class_proc_tick(current);
            break;
        // 略...
    }
}
```

可以看到`RR_proc_tick`的实现非常简单：将当前进程的时间片自减；如果当前进程的时间片归零了，则将其`need_resched`属性值置为1，即将其标记为"让操作系统从该进程切换到其他进程执行"。

```C++
// kern/schedule/default_sched.c
static void RR_proc_tick(struct run_queue *rq, struct proc_struct *proc) {
    if (proc->time_slice > 0) {
        proc->time_slice --;
    }
    if (proc->time_slice == 0) {
        proc->need_resched = 1;
    }
}
```

如果当前进程在调用`sched_class_proc_tick`被标记为"让操作系统从该进程切换到其他进程执行"，那么等待它的结果便是要主动暂时放弃CPU的使用权，让给其他进程继续执行：

```C++
void trap(struct trapframe *tf) {
    // dispatch based on what type of trap occurred
    // used for previous projects
    if (current == NULL) {
        trap_dispatch(tf);
    }
    else {
        // keep a trapframe chain in stack
        struct trapframe *otf = current->tf;
        current->tf = tf;
    
        bool in_kernel = trap_in_kernel(tf);
    
        trap_dispatch(tf);
    
        current->tf = otf;
        if (!in_kernel) {
            if (current->flags & PF_EXITING) {
                do_exit(-E_KILLED);
            }
            if (current->need_resched) {
                schedule();
            }
        }
    }
}
```

接下来又进入到了我们熟悉的`schedule`（代码上面已经贴过了）。由于正常情况当前进程的状态理应为`PROC_RUNNABLE`，我们首先将它放回调度器"队列"的尾部。接下来需要挑选一个其他的进程继续执行，而挑选的逻辑非常简单，直接取"队列"中的首个进程就行（有点FIFO的味道了）：

```C++
// kern/schedule/sched.c
static inline struct proc_struct *
sched_class_pick_next(void) {
    return sched_class->pick_next(rq);
}

// kern/schedule/default_sched.c
static struct proc_struct *
RR_pick_next(struct run_queue *rq) {
    list_entry_t *le = list_next(&(rq->run_list));
    if (le != &(rq->run_list)) {
        return le2proc(le, run_link);
    }
    return NULL;
}
```

如果当前进程在"队列"中的下一个进程就是用户用fork新创建的那个进程的话，它就会被从"队列"中取出，并且进入`proc_run`函数进行真正的进程切换流程。这部分涉及x86硬件细节的操作就是前两个lab的内容了，这里不再复述。

与其他进程一样，在我们新创建的进程的时间片耗尽之后，上述的流程还会再次发生，不过这次是由我们的新进程再将CPU转让给其他进程执行了。

当我们的用户进程执行完毕后会执行`SYS_exit`系统调用，这在上个lab中我已经介绍过了。

```C++
int do_exit(int error_code) {
    // 略...
    current->state = PROC_ZOMBIE;
    current->exit_code = error_code;
    // 略...
    schedule();
    panic("do_exit will not return!! %d.\n", current->pid);
}
```

在该系统调用中，用户进程的状态会被标记为`PROC_ZOMBIE`，并且立即调用`schedule`放弃CPU的使用权。结合`schedule`的源码可知，由于它的状态已经不是可供调度器调度的`PROC_RUNNABLE`了，因此它作为僵尸进程不会被放入调度器"队列"中，也就再也不可能被调度器调度了，只有静静地等待被父进程回收PCB块彻底销号。

# 练习2: 实现 Stride Scheduling 调度算法

Stride Scheduling算法看上去很唬人，但实际上非常容易理解，即"谁步数落后就调度谁"，所以我这里并不打算再来赘述这个算法本身的内容。让我们把注意力放在本实验最难理解的部分，也就是uCore处理stride值"回绕"的策略。

## uCore处理stride值"回绕"的策略

在lab6中为了支撑Stride Scheduling算法，进程PCB块中新增了一个名为`lab6_stride`的`uint32_t`类型字段，来记录每个进程的"总步数"。

但是学过计算机组成原理的我们都知道，随着总步数的累加，这个字段最终有可能会发生"回绕"（CSAPP中称之为"truncation"）。例如原先某个进程的总步数（stride）为$4294967295$（即32位无符号整型能表示的最大值`0xFFFFFFFF`），若该进程再次被调度而导致总步数增加$10$，则其在计算机内部记录的总步数由于"回绕"现象的存在，会变成一个比较小的数值$(4294967295 + 10) \bmod 2^{32} = 9$，而不是真正的数学上的理论值（下文简称"真值"）$4294967305$.

这显然不是我们想要的。Stride Scheduling算法在每次调度时依赖最小堆来挑选stride值最小的进程，而最小堆的底层又是依赖不同进程之间的stride值两两比较实现的，因此"回绕"的存在显然会导致这个算法工作到后来出现异常。对此，uCore中充分利用该算法自身的一个性质，以及计算机中有符号整型和无符号整型之间的映射关系，巧妙地绕开了这个问题，在不消除"回绕"现象的情况下，确保算法中最小堆内部比较的正确工作，从而保证算法的正确性。

### 一个结论

为了理解uCore的处理方案，首先我们需要理解一个结论。

**在不考虑中途新创建进程的前提下**，每次Stride调度器进行调度决策时，在当前所有进程的总步数中设最大的总步数值为$S_{max}$，最小的总步数值为$S_{min}$，另外设当前所有进程的步进值中最大者为$Pass_{max}$，那么则有：$S_{max} - S_{min} ≤ Pass_{max}$成立.

> 注意，在这个不等式中，$Pass_{max} = \text{min} \{Pass_{i} \mid i = 1,2,\dots,n\}$由各个进程自身的步进值$Pass_{i}$共同决定，在系统运行的过程中为一**常量**。而$S_{max}$和$S_{min}$在每次调度过程中都有可能是不一样的。

这个结论看上去比较抽象，我们先通过一个实际的例子感受一下。假设现在机器上有三个进程P1、P2和P3：

| 第几次调度 | P1($Pass_{1}=16$) | P2($Pass_{2}=7$) | P3($Pass_{3}=10$) | $S_{max} - S_{min}$ | $Pass_{max}$ | 结论成立么？ |
| ----- | ----------------- | ---------------- | ----------------- | ------------------- | ------------ | ------ |
| 初值    | ***stride=100***  | **stride=113**   | stride=102        | 13                  | 16           | 成立     |
| 1     | **stride=116**    | stride=113       | ***stride=102***  | 14                  | 16           | 成立     |
| 2     | **stride=116**    | stride=113       | ***stride=112***  | 4                   | 16           | 成立     |
| 3     | stride=116        | ***stride=113*** | **stride=122**    | 9                   | 16           | 成立     |
| 4     | ***stride=116***  | stride=120       | **stride=122**    | 6                   | 16           | 成立     |

如你所见，这个结论在上述的实例中的确是成立的。下面我们再通过数学归纳法来证明这个结论。

由于时间关系，这里我只打算以四个进程P1、P2、P3和P4的情况为例，来证明该结论的正确性。你可以自行将其推广到n个进程的情况。

在系统刚开始运行时，由于$S_1 = S_2 = S_3 = S_4 = 0$，因此调度器可以任从这四个进程中挑出（具体挑哪个，与这四个进程放入最小堆的顺序，以及最小堆的具体实现有关）来执行。如果第一次挑出进程P1来执行，那么到下次调度时我们有$S_1 = Pass_1$, $S_2=S_3=S_4=0$，此时$S_{max} - S_{min} = S_1 = Pass_1 ≤ Pass_{max}$，结论成立.

不难理解，事实上开始时我们的这四个进程会被轮番调度执行一遍。在此之后的首次调度（记作第$1$次调度）时，我们有：

*   $S_1 = Pass_1, S_2=Pass_2, S_3=Pass_3, S_4=Pass_4$.
*   设进程$i$和$j$的总步数$S_i$和$S_j$分别取到四个进程中的最大值和最小值，那么$S_{max} - S_{min} = Pass_{i} - Pass_{j} ≤ Pass_{i} ≤ \max \{Pass_1, Pass_2, Pass_3, Pass_4 \} = Pass_{max}$，即结论成立.

ok，现在数学归纳法的第一步已经完成了，下面我们来看一般情况。为了方便叙述，我们记$S_i^k$表示进程$i$在第$k$次调度时的总步数值是多少。

假设第$k$次调度时结论仍然成立，且我们不妨假设在第$k$次调度时，$S_{max}^k = S_3^k, S_{min}^k = S_4^k$，那么也就是说在第$k$次调度时，$S_{max}^k - S_{min}^k = S_3^k - S_4^k ≤ Pass_{max}$.

接下来来到第$k+1$次调度，简单分析可知$S_{max}^{k+1}$的取值可能为$S_3^k$或者$S_4^k + Pass_4$，且$S_{min}^{k+1}$的取值可能为$S_1^k, S_2^k$或者$S_4^k + Pass_4$.

> 思考一下：为什么$S_{max}^{k+1}$的取值不可能是$S_1^k$或者$S_2^k$？

① 先分析$S_{max}^{k+1} = S_3^k$这种情况。

在假设第$k$次调度时结论仍然成立的基础上，我们可以知道：

*   $S_3^k - S_1^k ≤ Pass_{max}$
*   $S_3^k - S_2^k ≤ Pass_{max}$
*   $S_3^k - (S_4^k + Pass_4) = S_3^k - S_4^k - Pass_4 ≤ S_3^k - S_4^k ≤ Pass_{max}$

> 提示：由$S_{max}^k - S_{min}^k  ≤ Pass_{max}$，可推知$| S_{i}^k - S_{j}^k | ≤ Pass_{max}$，其中$i,j∈\{1,2,3,4\}$.

归纳上面这三个不等式，我们可以知道：

$S_{max}^{k+1} - S_{min}^{k+1} = S_3^k - min \{S_1^k, S_2^k, S_4^k + Pass_4\} ≤ Pass_{max}$.

也就是说，对于$S_{max}^{k+1} = S_3^k$这种情况，在第$k+1$次调度时我们要证明的结论仍然成立！

② 类似地，让我们来看看$S_{max}^{k+1} = S_4^k + Pass_4$这种情况。

因为$S_{min}^k = S_4^k$， 我们很容易理解：

*   $S_4^k + Pass_4 - S_1^k = Pass_4 + (S_4^k - S_1^k) ≤ Pass_4 ≤ Pass_{max}$
*   $S_4^k + Pass_4 - S_2^k = Pass_4 + (S_4^k - S_2^k) ≤ Pass_4 ≤ Pass_{max}$

归纳上面这俩不等式，我们可以知道：

$S_{max}^{k+1} - S_{min}^{k+1} = S_4^k + Pass_4 - min \{S_1^k, S_2^k\} ≤ Pass_{max}$.

也就是说，对于$S_{max}^{k+1} = S_4^k + Pass_4$这种情况，在第$k+1$次调度时我们要证明的结论仍然成立！

**综上所述，我们终于证明了开头所述结论的正确性！**

### 对结论进行改造

现在我们虽然已经有了本文中最重要的结论，但我们并不能直接使用这个结论。这是因为在lab6中uCore的PCB块并没有直接表示步进数`pass`的字段，而只有表示进程优先级的`uint32_t lab6_priority`字段。

根据注释提示，我们可以知道这两者之间存在如下的换算关系：

$Pass = BIG\_STRIDE / Priority$，其中$BIG\_STRIDE$为一常量。

而uCore中规定合法的优先级取值都要满足$Priority ≥ 1$，于是我们可以对刚才的结论进行改造：

$S_{max} - S_{min} ≤ Pass_{max} = \frac{BIG\_STRIDE} {Priority_{min}} = BIG\_STRIDE$

而这个不等式又蕴含着下面的结论：

**对某次调度中的任意两个进程$i$和$j$，都有$|S_i - S_j| ≤ BIG\_STRIDE$.**

**也就是说，通过修改常量$BIG\_STRIDE$的取值，我们可以将任意两个进程的总步数之差控制在一个范围之内。** 显然，这范围不能太大，否则计算机可能无法正确表示和处理。

**在uCore中，我们将这个常量的值取为`0x7FFFFFFF`，也就是有符号整型`int32_t`所能表示的最大整数值。**

正如我们所料，这个取值看上去的确像个边界值。

不过再想想又感觉很诡异，之前我们提过uCore中PCB块的`lab6_priority`和`lab6_stride`字段都是以`uint32_t`类型储存的，现在又搞出一个`int32_t`所能表示的最大值又是什么意思？它与彻底绕开`lab6_stride`字段的"回绕"问题又有什么关系呢？请继续看我的分析！

### 绕开"回绕"问题的终极方案

为了方便分析，我这里假定`lab6_priority`和`lab6_stride`字段都以范围比较小的`uint8_t`类型储存；同时我取`BIG_STRIDE=0x7F`，也就是`int8_t`所能表示的最大整数。而幸运的是，推理结论是可以直接推广到的范围更大的`uint32_t`类型上的。

现在我们假设有两个进程$P$和$Q$，它们的总步数**真值**分别为$p_{r}$和$q_{r}$。

另外我们设它们的总步数以`uint8_t`无符号整型类型储存在计算机上的取值（即这俩进程PCB块的`lab6_stride`字段）分别为$p$和$q$，并且$p<q$.

显而易见，如果`p`和`q`在递增的还都没有发生过"回绕"现象，那么$p=p_r,q=q_r$，于是有$p ≤ q \Rightarrow p_r ≤ q_r$，此时调度算法可以正确做出决策。

此外，由于`BIG_STRIDE=0x7F=127`，我们可以知道$0 ≤ q - p ≤ 127$.

但是正如我前面介绍的，倘若$p$发生了"回绕"而$q$还没有"回绕"，这种直接比较计算机中储存的$p$和$q$取值的方法就失效了——虽然$p < q = q_r$，但事实上$p_r = p + 256, p_r > q_r$！

好吧，虽然$p$的取值这时候和真值已经不相等了，但既然计算机比大小的本质是做差，而前面我们已经证明的结论正好是一个作差的形式，我们仍然尝试着用它推一推看一看，看看能有些什么收获。

① 我们先来看看这时候真值的作差结果$q_r - p_r$，和计算机中储存值的作差结果$q - p$有什么关系。非常简单，代入$p_r = p + 256$，可以得到$q_r - p_r = q - p - 256$。这个关系现在似乎还没啥用。

② 另一方面，利用前面我们证明的结论，我们有$0 < p_r - q_r ≤ 127$，即$0 < p + 256 - q ≤ 127$，再整理一下可以得到$129 ≤ q - p < 256$.

> 提醒：这里我们将$q_r - p_r$的结果视作是`int8_t`类型的，而$q - p$则是`uint8_t`类型的，所以①和②最严谨的表示方式是:
>
> *   $q_r - p_r = (\text{int8\_t})((\text{uint8\_t})(q - p) - 256)$
> *   $129 ≤ (\text{uint8\_t})(q - p) < 256$

你注意到了吗，这里$q-p$的取值范围正好和前面没有发生"回绕"情况时该表达式的取值范围凑成了一个无符号整型`uint8_t`的取值范围！

**现在我们终于来到了本次lab中uCore设计得最巧妙的地方**。来来来，让我们看看C语言中`uint8_t`和`int8_t`这两种数据类型的映射关系：

![image.png](https://p0-xtjj-private.juejin.cn/tos-cn-i-73owjymdk6/069d251ba3d54023a0c7ace669f375c7~tplv-73owjymdk6-jj-mark-v1:0:0:0:0:5o6Y6YeR5oqA5pyv56S-5Yy6IEAgUEFL5ZCR5pel6JG1:q75.awebp?policy=eyJ2bSI6MywidWlkIjoiMzU0NDQ4MTIyMDAwODc0NCJ9&rk3s=f64ab15b&x-orig-authkey=f32326d3454f2ac7e96d3d06cdbb035152127018&x-orig-expires=1726510608&x-orig-sign=M4M6zIJucaceBuSJ%2B9YlvfPAnEk%3D)

从图中可见，在从无符号整型`uint8_t`到`int8_t`的类型转换中，整数区间$[128, 256)$会被映射到整数区间$[-128, 0)$. 显而易见，如果某个以`uint8_t`类型储存的整数$x$落在$[128, 256)$这个区间内，则将其转换为`int8_t`类型后，它在这种类型的意义下，在数学上将会被解释成整数$x - 256$.

现在，我们再把前面分析出来的两个关系式，和这个映射关系摆在一起，看看能有什么发现：

*   $q_r - p_r = (\text{int8\_t})((\text{uint8\_t})(q - p) - 256)$
*   $129 ≤ (\text{uint8\_t})(q - p) < 256$

我们震惊地发现，以`uint8_t`类型表示的$q - p$恰好落在了刚才我们提到的映射区间$[128, 256)$之内。并且，最关键的地方在于，在此区间内从`uint8_t`到`int8_t`的映射关系恰好为减去常数$256$——**这正好就是出现"回绕"现象时从$q - p$换算到真值$q_r-p_r$时所要减去的那个常数！！！**

也就是说，在以`uint8_t`类型储存进程总步数值`lab6_stride`时，我们只需要做以下几件事情，就能够保证在不设法消除"回绕"现象本身的前提下，程序比较一个出现"回绕"的总步数值和一个正常的总步数值时，仍然能得到正确的比较结果：

*   **取`BIG_STRIDE=0x7F=127`**
*   **在比较任意两个进程的总步数值`lab6_stride`时，先将它们作差，并将作差结果转换为`int8_t`类型，再与0作有符号数之间的比较。** 若出现了"一值回绕一值正常"的情况，则作差结果由于前述映射关系的存在，在`int8_t`类型的意义下，会被正确解释成俩进程总步数真值之间的差值，因此比较结果始终正确。

> 通过更加严密的分析，应该可以证明如果$p$和$q$都发生了$N$次"回绕"，或者$p$发生$N+1$次回绕而$q$只发生$N$次"回绕"的情况下，这个处理方法仍然是正确和有效的。

显而易见的是，上述分析和结论可以被轻松地推广到lab6中使用更宽的`uint32_t`类型来储存`lab6_stride`字段的情形。

到这里uCore中处理无符号整型"回绕"问题方案的正确性就介绍完了。虽然**我还是没有想通thu的大佬们是怎么从0到1设计出这个方案的**，不过光是这个分析正确性的过程就让我对有符号数和无符号数之间的联系有了更深刻的认识，这个方案本身也让我震惊地拍案叫绝。

## 代码实现

和之前的几个lab一样，这一次的代码也是非常简单的，并且优先队列二叉堆的函数接口也已经封装好了，只需要跟着注释的提示敲就行了。

只需要注意几点：

*   lab6中进程默认的`lab6_priority`字段取值为0，因此在`stride_pick_next`函数计算步进值时需要进行特判，不能直接套用公式`stride += BIG_STRIDE / priority`。
*   使用完`skew_heap_remove`和`skew_heap_insert`后需要手工更新`rq->lab6_run_pool`。
*   `stride_pick_next`函数中从调度器"队列"中取元素之前，要先判断一下`rq->lab6_run_pool`是否等于`NULL`。

```C++
#include <defs.h>
#include <list.h>
#include <proc.h>
#include <assert.h>
#include <default_sched.h>

#define USE_SKEW_HEAP 1

/* You should define the BigStride constant here*/
/* LAB6: YOUR CODE */
#define BIG_STRIDE (0x7FFFFFFF)  /* you should give a value, and is ??? */

/* The compare function for two skew_heap_node_t's and the
 * corresponding procs*/
static int
proc_stride_comp_f(void *a, void *b)
{
     struct proc_struct *p = le2proc(a, lab6_run_pool);
     struct proc_struct *q = le2proc(b, lab6_run_pool);
     int32_t c = p->lab6_stride - q->lab6_stride;
     if (c > 0) return 1;
     else if (c == 0) return 0;
     else return -1;
}

static void
stride_init(struct run_queue *rq) {
     list_init(&rq->run_list);
     rq->lab6_run_pool = NULL;
     rq->proc_num = 0;
}

static void
stride_enqueue(struct run_queue *rq, struct proc_struct *proc) {
     // (1) insert the proc into rq correctly
     rq->lab6_run_pool = 
          skew_heap_insert(rq->lab6_run_pool, &proc->lab6_run_pool, &proc_stride_comp_f);
     // (2) recalculate proc->time_slice
     if (proc->time_slice <= 0 || proc->time_slice > rq->max_time_slice) {
          proc->time_slice = rq->max_time_slice;
     }
     // (3) set proc->rq pointer to rq
     proc->rq = rq;
     // (4) increase rq->proc_num
     ++rq->proc_num;
}

static void
stride_dequeue(struct run_queue *rq, struct proc_struct *proc) {
     assert(proc->rq == rq);
     rq->lab6_run_pool = 
          skew_heap_remove(rq->lab6_run_pool, &proc->lab6_run_pool, &proc_stride_comp_f);
     --rq->proc_num;
}

static struct proc_struct *
stride_pick_next(struct run_queue *rq) {

     if (rq->lab6_run_pool == NULL) return NULL; 
       
     struct proc_struct* proc = le2proc(rq->lab6_run_pool, lab6_run_pool);
     assert(proc->rq == rq);
     proc->lab6_stride += 
          (proc->lab6_priority == 0) ? BIG_STRIDE : (BIG_STRIDE / proc->lab6_priority);
     return proc;
}

static void
stride_proc_tick(struct run_queue *rq, struct proc_struct *proc) {
     /* LAB6: YOUR CODE */
     assert(proc->rq == rq);
     if (--proc->time_slice <= 0) {
          proc->need_resched = 1;
     }
}

struct sched_class default_sched_class = {
     .name = "stride_scheduler",
     .init = stride_init,
     .enqueue = stride_enqueue,
     .dequeue = stride_dequeue,
     .pick_next = stride_pick_next,
     .proc_tick = stride_proc_tick,
};
```

# 参考资料

<https://blog.csdn.net/u012750235/article/details/131884423>

<https://blog.csdn.net/mzz510/article/details/136109343>

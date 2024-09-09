# 练习0

从lab7开始uCore引入了信号量的实现，于是我们针对`cprintf`函数的大锁也可以作相应的修改:

```C++
// kern/sync/my_spin_lock.h
#ifndef __MY_LOCK_H__
#define __MY_LOCK_H__

#include <sem.h>
extern semaphore_t global_print_lock;

void print_lock();
void print_unlock();

#endif

// kern/sync/my_spin_lock.c
#include <my_spin_lock.h>
#include <sched.h>

semaphore_t global_print_lock;

void inline print_lock() {
    down(&global_print_lock);
}

void inline print_unlock() {
    up(&global_print_lock);
}
```

初始化互斥锁：

```C++
// kern/init/init.c
#include <my_spin_lock.h>

int kern_init(void) {
    extern char edata[], end[];
    memset(edata, 0, end - edata);

    sem_init(&global_print_lock, 1);
    
    // ...
}
```

针对用户程序的系统调用要改一下：

```C++
// kern/syscall/syscall.c
#include <my_spin_lock.h>

static int sys_print_lock(uint32_t arg[]) {
    down(&global_print_lock);
    return 0;
}

static int sys_print_unlock(uint32_t arg[]) {
    up(&global_print_lock);
    return 0;
}
```

最后别忘了提供给内核使用的`cprintf`函数也要修改，不然本次实验还是会出问题：

```C++
// kern/libs/stdio.c
#include <my_spin_lock.h>

int vcprintf(const char *fmt, va_list ap) {
    int cnt = 0;
    print_lock();
    vprintfmt((void*)cputch, NO_FD, &cnt, fmt, ap);
    print_unlock();
    return cnt;
}
```

# 练习1: 理解内核级信号量的实现和基于内核级信号量的哲学家就餐问题

## 定时器的实现

为了支撑"在uCore上模拟哲学家就餐问题"的需求，事实上除了实现内核级信号量外，还需要定时器予以支撑。

虽然"定时器"听起来很高大上，但事实上在uCore中的实现原理非常容易理解。我这里就通过自顶向下分析代码的方式，简单过一遍。

内核进程设置定时器的核心函数为`do_sleep`，同时该函数通过封装成`sys_sleep`系统调用的形式暴露给用户使用。

```C++
// kern/process/proc.c
int
do_sleep(unsigned int time) {
    if (time == 0) {
        return 0;
    }
    bool intr_flag;
    local_intr_save(intr_flag);
    timer_t __timer, *timer = timer_init(&__timer, current, time);
    current->state = PROC_SLEEPING;
    current->wait_state = WT_TIMER;
    add_timer(timer);
    local_intr_restore(intr_flag);

    schedule();

    del_timer(timer);
    return 0;
}

// kern/syscall/syscall.c
static int
sys_sleep(uint32_t arg[]) {
    unsigned int time = (unsigned int)arg[0];
    return do_sleep(time);
}
```

大致阅读`do_sleep`的实现可以知道，该函数无非做了几件关键的事情：

*   创建一个`timer_t`对象用于记录定时器信息，并通过`add_timer`函数塞到一个操作系统维护的定时器链表里去。
*   设置当前进程的`state`和`wait_state`属性。通过`wait_state`属性，在唤醒某个进程前，我们可以了解到该进程沉睡的原因。
*   调用`schedule`切到其他进程，当前进程正式放弃CPU使用权。联系lab6可知，由于现在当前进程已不在调度器"队列"中，不用担心在被唤醒前当前进程再次被调度器调度。
*   该进程重新被唤醒后首先通过`del_timer`删除定时器链表中的记录，然后继续沿之前的控制流往下执行。

**这里比较有看头的地方是uCore是如何维护定时器链表的。**

这里直接看代码的话可能不免会有些疑惑，所以我们先通过一个例子来理解一下。

设内核进程A执行了`do_sleep(20)`然后睡下，这表示进程A将从它调用该函数起的时刻休眠20个时钟tick。紧接着调度器立马切换到内核进程B继续执行，内核进程B执行了`do_sleep(38)`然后睡下，这表示进程B将从它调用该函数起的时刻休眠38个tick。

**假设这个过程中间始终没有发生时钟中断**，且起初定时器链表为空，那么这时uCore内部的定时器链表情况应该是怎么样的呢？请看下图：

      +------------+       +----------------------+       +----------------------+
      | timer_list | <---> | timer1->expires = 20 | <---> | timer2->expires = 18 |
      +------------+       +----------------------+       +----------------------+

可以看到对于第二个定时器，其`expires`字段的值并不是38，而是第二个定时器在第一个定时器被触发后，再等多少时间会被触发。

那如果我们再推广一下这个设计，比如这时候又有一个内核进程C调用了`do_sleep(26)`，链表又会变成什么样呢？请看下图：

      +------------+       +----------------------+        +--------------------+       +----------------------+
      | timer_list | <---> | timer1->expires = 20 | <---> | timer3->expires = 6 | <---> | timer2->expires = 12 |
      +------------+       +----------------------+       +---------------------+       +----------------------+

可以看到，链表中除了首条记录的`expires`表示了其对应的定时器，相对于当前时刻，还剩多少个时钟tick会被触发，后边所有记录的`expires`都表示在上一个定时器被触发后，自己对应的定时器还剩多少个时钟tick会被触发。

理解了这个设计，我们也就自然而然能看懂`add_timer`函数的实现。源码胜千言，这里就不多说了。

```C++
// kern/schedule/sched.c
void add_timer(timer_t *timer) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        assert(timer->expires > 0 && timer->proc != NULL);
        assert(list_empty(&(timer->timer_link)));
        list_entry_t *le = list_next(&timer_list);
        while (le != &timer_list) {
            timer_t *next = le2timer(le, timer_link);
            if (timer->expires < next->expires) {
                next->expires -= timer->expires;
                break;
            }
            timer->expires -= next->expires;
            le = list_next(le);
        }
        list_add_before(le, &(timer->timer_link));
    }
    local_intr_restore(intr_flag);
}
```

再来看看uCore是如何响应时钟中断更新定时器链表的。在练习0中，我们已对`trap_dispatch`函数做了修改，使得CPU被时钟中断后会首先跳到`run_timer_list`这个函数。让我们来看看它：

```C++
void run_timer_list(void) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        list_entry_t *le = list_next(&timer_list);
        if (le != &timer_list) {
            timer_t *timer = le2timer(le, timer_link);
            assert(timer->expires != 0);
            timer->expires --;
            while (timer->expires == 0) {
                le = list_next(le);
                struct proc_struct *proc = timer->proc;
                if (proc->wait_state != 0) {
                    assert(proc->wait_state & WT_INTERRUPTED);
                }
                else {
                    warn("process %d's wait_state == 0.\n", proc->pid);
                }
                wakeup_proc(proc);
                del_timer(timer);
                if (le == &timer_list) {
                    break;
                }
                timer = le2timer(le, timer_link);
            }
        }
        sched_class_proc_tick(current);
    }
    local_intr_restore(intr_flag);
}
```

由于定时器链表和调度器"队列"都是不同进程之间的竞争资源，这里首先要通过`local_intr_save`和`local_intr_restore`建立临界区，这我们已经很熟悉了。临界区中的最后一条代码调用的`sched_class_proc_tick`函数我们也熟悉不过了，它是lab6中的主角。

这里我们关注时钟中断发生后uCore是如何维护定时器链表的。从代码中可以看到，uCore只会通过`timer->expires --;`这一条代码仅仅是将定时器链表中的首条记录的剩余tick自减。如果剩余tick归零，则唤醒对应的进程，并从链表中移除首条记录。在此过程中并不会有遍历链表操作的发生。

如果你已经理解了前面我介绍的uCore中定时器链表的设计理念，相信你会觉得这是自然而正确的。**这就是设置相对值，而非设置一个绝对值或者其他什么东西的好处！**

那么从链表中移除定时器记录后，需要对链表中剩下的记录再做什么更新吗？

根据链表中每条记录`expires`字段的含义，可以理解，如果是位于链表头部的定时器因为定时时间到被触发（此时`expires`字段已清零），而需要从链表中移除，是不需要对其下一条记录以及后续的任何记录做修改的。

不过如果某个定时器在等待中途因被用户或者操作系统撤销而需要从链表中移除，那么这时就需要将它剩余的`expires`值增加到它在链表中的下一个定时器上去。**注意，这绝不会导致下一个定时器的触发时间被推后，而恰恰只有这样做才能保证下一个定时器不会被提前触发**——如果你还是不能理解，请再回顾一下上面介绍的定时器链表的设计理念。

`del_timer`的代码如下：

```C++
void del_timer(timer_t *timer) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        if (!list_empty(&(timer->timer_link))) {
            if (timer->expires != 0) {
                list_entry_t *le = list_next(&(timer->timer_link));
                if (le != &timer_list) {
                    timer_t *next = le2timer(le, timer_link);
                    next->expires += timer->expires;
                }
            }
            list_del_init(&(timer->timer_link));
        }
    }
    local_intr_restore(intr_flag);
}
```

## 内核级信号量的实现

信号量的结构非常简单，一个计数器加一个等待队列。信号量的核心操作也非常简单，一个P操作（`down`函数）一个V操作（`up`函数）。

```C++
// kern/sync/sem.h
typedef struct {
    int value;
    wait_queue_t wait_queue;
} semaphore_t;

// kern/sync/sem.c
void up(semaphore_t *sem) {
    __up(sem, WT_KSEM);
}

void down(semaphore_t *sem) {
    uint32_t flags = __down(sem, WT_KSEM);
    assert(flags == 0);
}
```

我们先来分析一下**用于申请共享资源的P操作**的实现细节：

```C++
// kern/sync/wait.c
void wait_current_set(wait_queue_t *queue, wait_t *wait, uint32_t wait_state) {
    assert(current != NULL);
    wait_init(wait, current);
    current->state = PROC_SLEEPING;
    current->wait_state = wait_state;
    wait_queue_add(queue, wait);
}

// kern/sync/sem.c
static __noinline uint32_t __down(semaphore_t *sem, uint32_t wait_state) {
    bool intr_flag;
    local_intr_save(intr_flag);
    if (sem->value > 0) {
        sem->value --;
        local_intr_restore(intr_flag);
        return 0;
    }
    wait_t __wait, *wait = &__wait;
    wait_current_set(&(sem->wait_queue), wait, wait_state);
    local_intr_restore(intr_flag);

    schedule();

    local_intr_save(intr_flag);
    wait_current_del(&(sem->wait_queue), wait);
    local_intr_restore(intr_flag);

    if (wait->wakeup_flags != wait_state) {
        return wait->wakeup_flags;
    }
    return 0;
}
```

可以看到，这个函数虽然长得唬人，但实际上执行的**都是我们在之前几个lab中碰到的基本操作**：

*   检查一下`value`计数器清零了没（地主家还有没有余粮？）。还有余粮的话计数器就自减，表示某种共享资源被当前进程又取走了一份，然后返回直接返回就行。
*   如果地主家已经没余粮提供给当前进程了，就把当前进程记录到该信号量的等待队列中去，并且让该进程休眠，以及把CPU让给其他可供调度的进程执行。
*   待该进程再度被唤醒（从`schedule`函数返回）后，从信号量的等待队列中移除自己的记录。

> 思考：`__down`函数中出现了几处`local_intr_save`和`local_intr_restore`建立的中断临界区，它们分别是在保护什么可能会在不同进程之间被争夺的共享资源？

再看一眼**用于释放共享资源的V操作**：

```C++
// kern/sync/sem.c
static __noinline void __up(semaphore_t *sem, uint32_t wait_state) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        wait_t *wait;
        if ((wait = wait_queue_first(&(sem->wait_queue))) == NULL) {
            sem->value ++;
        }
        else {
            assert(wait->proc->wait_state == wait_state);
            wakeup_wait(&(sem->wait_queue), wait, wait_state, 1);
        }
    }
    local_intr_restore(intr_flag);
}

// kern/sync/wait.c
void wakeup_wait(wait_queue_t *queue, wait_t *wait, uint32_t wakeup_flags, bool del) {
    if (del) {
        wait_queue_del(queue, wait);
    }
    wait->wakeup_flags = wakeup_flags;
    wakeup_proc(wait->proc);
}
```

这就更简单啦！

*   先看看有没有进程在急着等地主家新收进的余粮（等待队列是否为空），如果没有就把新收进的余粮记到账上（`sem->value ++`），以备下次有进程来申请。
*   如果有进程已经在队列里等着了，就直接把新收进的余粮（竞争资源）直接给最早来申请的那个进程进程（在逻辑上），这样也就不需要再记账了。此外现在既然这个进程已经拿到了余粮，那就应该唤醒它让，好让它被调度器调度并得以继续往下执行。

## 基于内核级信号量的哲学家就餐问题

lab7中模拟实现的"哲学家就餐问题"源码与我们直观理解上的不太一样，其并没有直接使用信号量来抽象竞争资源"叉子"的这个概念，而是引入了一个`int state_sema[N]`状态数组作为竞争资源。

该数组中的每一项用于记录对应的那个哲学家的状态。在uCore中，将哲学家的状态抽象成三种情况：

*   `THINKING`：某个哲学家正在思考，此时不需要使用叉子。
*   `HUNGRY`：某个哲学家想吃饭了，正在等待获取叉子（还没有得到叉子）。
*   `EATING`：某个哲学家正在吃饭，此时他占用左手右手的两个叉子。

按照哲学家就餐问题的规则，某个哲学家的状态可由`THINKING`迁移到`HUNGRY`（思考结束，等待叉子），再由`HUNGRY`迁移到`EATING`（从等待叉子，到成功得到叉子进行吃饭），再由`EATING`迁移回`THINGKING`（吃饭结束放弃叉子，继续思考）。

基于三种状态的转换关系，不难理解，如果要判断某个哲学家是否有需要且有可能申请到叉子，我们只需要确认以下三件事：

*   该哲学家自身当前处于`HUNGRY`状态
*   该哲学家左边的哲学家当前处于非`EATING`的状态，否则该哲学家现在还申请不到他左手边的叉子
*   同理，该哲学家右边的哲学家当前也应处于非`EATING`的状态

> 思考：为什么`int state_sema[N]`状态数组是竞争资源？我们应该如何确保它的进程安全？

为了保证**对状态数组的读写操作是原子的**，uCore中采取的处理方案是引入一把用二值信号量模拟的大锁`semaphore_t mutex`。信号量的P操作（`down(&mutex)`）对应上锁操作，V操作（`up(&mutex)`）对应释放锁操作。

ok，现在哲学家的状态转移，和对竞争资源的保护都弄清楚了，不过现在还剩一件很重要的事情。

我们知道，进程可不像现实世界中的哲学家那样长着眼睛，或者有什么主观能动性，看到有空闲的叉子就会主动去争抢。因此我们还需要搞明白uCore中是如何实现当有某个哲学家就餐完毕让出叉子后（`EATING=>THINGKING`），如何**通知**其他正在等待叉子（`HUNGRY`）的哲学家来竞争叉子的。

在整理一下思路，其实很简单，我们无非要实现下面这样的需求：

*   当某个处于`HUNGRY`状态、正在申请叉子的哲学家，发现他左手边或者右手边的叉子已经被占用了（即其左手边或者右手边的那位哲学家正处于`EATING`），那么我们肯定希望让这位哲学家赶紧去睡觉，把CPU的使用权让给其他进程。
*   当某个处于`EATING`状态的哲学家完成吃饭、切换到`THINGKING`状态时，在逻辑上他的两把叉子就空闲了。这时我们就希望他能检查一下他左边和右边的哲学家如果正处于`HUNGRY`状态睡大觉的话，是否会因为自己让出叉子而能够切换到`EATING`状态。如果能，则把他左边或者右边的哲学家叫醒，让他们去吃饭。

进一步分析问题，我们发现**只需要建立某种通知机制，而这个机制能够在一定条件下让指定进程睡下，以及能够在一定条件下通知（唤醒）某个进程**，就可以实现上述的需求。

是的，信号量恰好就可以实现这个需求！基于对信号量的理解，我们知道"在一定条件下让指定进程睡下"这个需求恰好对应了信号量的P操作（`down`）；而"在一定条件下唤醒某个进程"的需求恰好对应了信号量的V操作（`up`）。

具体到uCore，它为每个哲学家进程引入了一个专属的二值控制信号量。为了方便管理和允许某个哲学家进程操作其他哲学家进程的控制信号量，这些信号量被放置在`semaphore_t s[N]`全局数组中。

当某个正准备吃饭的哲学家进程`i`发现自己申请不到左手或者右手边的叉子时，就对自己的控制信号量执行P操作让自己睡眠（`down(&s[i])`）。当某个哲学家进程吃饭完毕释出叉子后，就看看自己左手边和右手边的哲学家是否满足了能够吃饭的条件，如能，就通过V操作（`up(&s[LEFT])`和`up(&s[RIGHT])`）来唤醒他们。这一切都是非常自然而合理的。

现在我已经彻底讲清楚了uCore中利用信号量来模拟哲学家就餐问题的核心思路，相信这时再去阅读源码就没什么阻碍可言了：

```C++
void phi_test_sema(i) /* i：哲学家号码从0到N-1 */
{ 
    if(state_sema[i]==HUNGRY&&state_sema[LEFT]!=EATING
            &&state_sema[RIGHT]!=EATING)
    {
        state_sema[i]=EATING;
        up(&s[i]);
    }
}

void phi_take_forks_sema(int i) /* i：哲学家号码从0到N-1 */
{ 
        down(&mutex); /* 进入临界区 */
        state_sema[i]=HUNGRY; /* 记录下哲学家i饥饿的事实 */
        phi_test_sema(i); /* 试图得到两只叉子 */
        up(&mutex); /* 离开临界区 */
        down(&s[i]); /* 如果得不到叉子就阻塞 */
}

void phi_put_forks_sema(int i) /* i：哲学家号码从0到N-1 */
{ 
        down(&mutex); /* 进入临界区 */
        state_sema[i]=THINKING; /* 哲学家进餐结束 */
        phi_test_sema(LEFT); /* 看一下左邻居现在是否能进餐 */
        phi_test_sema(RIGHT); /* 看一下右邻居现在是否能进餐 */
        up(&mutex); /* 离开临界区 */
}

int philosopher_using_semaphore(void * arg) /* i：哲学家号码，从0到N-1 */
{
    int i, iter=0;
    i=(int)arg;
    cprintf("I am No.%d philosopher_sema\n",i);
    while(iter++<TIMES)
    { /* 无限循环 */
        cprintf("Iter %d, No.%d philosopher_sema is thinking\n",iter,i); /* 哲学家正在思考 */
        do_sleep(SLEEP_TIME);
        phi_take_forks_sema(i); 
        /* 需要两只叉子，或者阻塞 */
        cprintf("Iter %d, No.%d philosopher_sema is eating\n",iter,i); /* 进餐 */
        do_sleep(SLEEP_TIME);
        phi_put_forks_sema(i); 
        /* 把两把叉子同时放回桌子 */
    }
    cprintf("No.%d philosopher_sema quit\n",i);
    return 0;    
}
```

> 请你再自行思考以下问题：
>
> 1.  为了配合上述"通知机制"，应该如何设定`semaphore_t s[N]`中每个信号量的计数器的初值？
> 2.  `semaphore_t s[N]`全局数组自身在哲学家之间是竞争资源吗？需要大锁`mutex`来提供保护吗？
> 3.  能否调换`phi_take_forks_sema`函数中`up(&mutex)`和`down(&s[i])`这两句代码？

# 练习2: 完成内核级条件变量和基于内核级条件变量的哲学家就餐问题

这个练习实际上又放水了，我们只需要照着源码中的注释提示敲一遍代码，就能让程序跑通。但我们的追求肯定不能止步于此，理解管程机制的设计思想才是我们的最终目的。

## 管程机制的核心思想

由于上来直接看uCore中管程的底层实现代码会感觉不知所云，这里我们依然采用"自顶向下"的学习方法，先来看看怎么用封装好的函数来实现"哲学家就餐问题"。

回顾前面用信号量机制来实现哲学家就餐问题的思路，我们会发现实际上我们只做了几件最关键的事情：

*   一把大锁保平安（即实现*Mutual Exclusion*）：针对共享资源`int state_sema[N]`数组，引入一把用二值信号量实现的大锁`mutex`保护之。
*   构建通知机制（即实现*Synchronization*）：我们利用信号量的P/V操作建立了一种通知机制，使得某位哲学家用餐完毕放下叉子后，若发现其左侧或者右侧的「正因为想吃饭却没有叉子而沉睡」的哲学家现在可以用餐了，则通知（唤醒）之，以便其继续往下执行。

同时，在分析代码的过程中，我们也看到了使用信号量解决哲学家就餐问题等「同步互斥」问题时也存在**使用不便、容易出错**的问题。概括来说：

*   我们必须谨慎地安排各种信号量P/V操作在程序控制流中的执行顺序，否则可能会导致死锁。
*   我们必须确保所有所有信号量的P操作都能有相应的V操作，不能多也不能漏，否则也可能会导致死锁。

而管程机制的出现，正是为了解决这个问题，使得程序员以一种更简单的形式来编写涉及「同步互斥」问题的代码，将主要精力放在解决问题上，而无需担忧如何避免死锁。

我们先来以两个进程A和B为例，大致了解一下管程机制的思想。

首先，如同传统的信号量机制，管程机制中每个并发进程都有属于自己的一段互斥代码（临界区）。管程机制规定一段时间之内只有一个进程可以执行临界区内的代码，以保证并发进程之间的互斥性。在uCore实现的管程机制中，我们仍然使用一把大锁来做到这点——这看上去与基于信号量实现「同步互斥」问题似乎没什么区别。

我们假设进程A首先被调度执行，于是它会首先抢占到那把大锁。接下来，我们又假设A在执行中途会发生阻塞，而继续往下执行则需要某个条件（condition）成立。要使得这个条件成立，就需要进程B进入临界区执行某些操作。在进程A发生阻塞后，它首先需要释放大锁，这样才能保证进程B能够进入临界区执行。当B进入临界区并使得条件成立后，它会通知（唤醒）进程A，让进程A得以继续往下执行。同时，因为管程机制规定一段时间内只有一个进程可以在临界区中执行，所以进程B这时就不得不休眠，直到进程A退出执行，或者进程A再次请求进程B让某个条件成立。

这就是管程机制最基本的思想，并且我们可以把两个进程的情况很容易地推广到更多并发进程的情景中\~

乍一听这似乎与基于信号量的并发编程方式没什么区别，但细品一下还是能发现一个重要的不同。在上面的叙述中，我们不再讨论信号量云云，而是引入了名为"条件"的概念来对进程间的同步关系进行抽象。为了方便叙述，我们接下来将会把这种用于控制进程间同步的"条件"称作"条件变量"（condition variable）。

同时，从上面的叙述中可以看到，管程机制为各个进程提供了两个相比信号量P/V操作抽象程度更高的、针对条件变量的操作：

*   等待条件变量成立：执行该操作的进程休眠，直到被其他进程唤醒（通知）自己等待的某个条件变量成立。
*   通知条件变量成立：如果有进程正因为等待这个条件变量成立而正在睡眠，那么就使得当前进程睡下，并且唤醒这些正在等待条件变量成立的进程中的一个或多个。如果没有正在等待条件变量成立的进程，该操作则什么也不做。

在uCore源码中，前者对应函数`void cond_wait(condvar_t *cvp)`，后者对应函数`void cond_signal(condvar_t *cvp)`。

假设你已经有了一定的编程经验，应该不难理解管程机制抽象出"条件变量"这个概念是**自然且合理**的。例如在"生产者-消费者"问题中，生产者进程继续往下执行的条件就是"缓冲区未满"，而消费者进程继续往下执行的条件就是"缓冲区不为空"。应该不难理解，基于"条件变量"来思考并发编程，显然比直接设计和操作更底层的信号量更加容易和不易出错。

## 利用管程机制实现"哲学家就餐问题"的思路

为了理解uCore中利用管程机制实现"哲学家就餐问题"的思路，我们就首先需要弄明白在这里uCore作者将什么抽象成了条件变量。

这里我们先不看`phi_take_forks_condvar`和`phi_put_forks_condvar`函数中最前面和最后面的代码是干什么用的，只需大致知道它们是为了保证并发进程的互斥性就行了。

通过分析源码中`cond_wait(&mtp->cv[i])`和`cond_signal(&mtp->cv[i])`的调用时机可知，在这里uCore作者将"哲学家进程`i`的状态是否可由`HUNGRY`切换到`EATING`状态"（也就是哲学家`i`是否可以用餐了）作为控制进程间同步的条件变量。具体来说：

*   当某位哲学家准备用餐（切换到`HUNGRY`状态）却发现通不过`phi_test_condvar`函数检查而无法用餐时，就意味着条件变量`&mtp->cv[i]`不成立，这时候就调用`cond_wait(&mtp->cv[i])`睡大觉等着条件变量成立。
*   当某位哲学家用餐完毕放下叉子时，他会主动检查左侧或右侧正在睡觉的哲学家（如果有的话）等待的条件变量是否成立（即左侧或右侧的哲学家现在是否可以用餐了）。如成立，就调用`cond_wait(&mtp->cv[LEFT])`或者`cond_wait(&mtp->cv[RIGHT])`宣布条件变量成立，并将对应的哲学家唤醒之。

```C++
struct proc_struct *philosopher_proc_condvar[N]; // N philosopher
int state_condvar[N];                            // the philosopher's state: EATING, HUNGARY, THINKING  
monitor_t mt, *mtp=&mt;                          // monitor

void phi_test_condvar (i) { 
    if(state_condvar[i]==HUNGRY&&state_condvar[LEFT]!=EATING
            &&state_condvar[RIGHT]!=EATING) {
        cprintf("phi_test_condvar: state_condvar[%d] will eating\n",i);
        state_condvar[i] = EATING ;
        cprintf("phi_test_condvar: signal self_cv[%d] \n",i);
        cond_signal(&mtp->cv[i]) ;
    }
}

void phi_take_forks_condvar(int i) {
     down(&(mtp->mutex));
//--------into routine in monitor--------------
     // LAB7 EXERCISE1: YOUR CODE
     // I am hungry
     // try to get fork
     state_condvar[i] = HUNGRY;
     phi_test_condvar(i);
     if (state_condvar[i] != EATING) {
        cond_wait(&mtp->cv[i]);
     }
//--------leave routine in monitor--------------
      if(mtp->next_count>0)
         up(&(mtp->next));
      else
         up(&(mtp->mutex));
}

void phi_put_forks_condvar(int i) {
     down(&(mtp->mutex));

//--------into routine in monitor--------------
     // LAB7 EXERCISE1: YOUR CODE
     // I ate over
     // test left and right neighbors
     state_condvar[i] = THINKING;
     phi_test_condvar(LEFT);
     phi_test_condvar(RIGHT);
//--------leave routine in monitor--------------
     if(mtp->next_count>0)
        up(&(mtp->next));
     else
        up(&(mtp->mutex));
}
```

可见，只需要我们牢牢抓住"条件变量是什么"、"什么时候谁需要等待条件变量成立"、"什么时候谁负责通知/宣布条件变量"成立这三个核心问题，就可以快速弄清楚这段代码的思路。

## 管程机制的底层实现

先来看看支撑管程机制的核心要素「条件变量」在uCore内部是如何定义的。可以看到，uCore中为了实现管程机制让进程睡眠和唤醒进程的效果，底层用的还是信号量来予以支撑——这并没有出乎我们的意料。

并且既然**条件变量只有成立和不成立两种状态**，我们可以大胆猜测这里的`semaphore_t sem`应该是一个二值信号量。

至于`int count`，从注释可知它用于记录当前正在有多少进程正在因等待当前这个条件变量成立而睡眠。

```C++
// kern/sync/monitor.h
typedef struct condvar{
    semaphore_t sem;        // the sem semaphore  is used to down the waiting proc, and the signaling proc should up the waiting proc
    int count;              // the number of waiters on condvar
    monitor_t * owner;      // the owner(monitor) of this condvar
} condvar_t;
```

管程机制本身的一些核心变量则放置在一个`struct monitor`结构体中，其定义如下：

```C++
typedef struct monitor{
    semaphore_t mutex;      // the mutex lock for going into the routines in monitor, should be initialized to 1
    semaphore_t next;       // the next semaphore is used to down the signaling proc itself, and the other OR wakeuped waiting proc should wake up the sleeped signaling proc.
    int next_count;         // the number of of sleeped signaling proc
    condvar_t *cv;          // the condvars in monitor
} monitor_t;
```

我们很容易意识到`semaphore_t mutex`这个信号量应该是用于控制并发进程之间互斥性的。至于`semaphore_t next`，从刚才在哲学家用餐问题部分的代码可以看到，这个信号量也应该与保证互斥性有关，不过它与`semaphore_t mutex`有什么区别呢？而`int next_count`看上去像个计数器，它又是干啥用的呢？

别急，我们慢慢往下看。

以下是管程机制的初始化代码。从`sem_init(&(mtp->mutex), 1)`这句能够猜到基于信号量的互斥锁`mutex`开始时处于未上锁的状态——这很容易理解，是为了首个并发进程得以进入临界区。

从`sem_init(&(mtp->cv[i].sem),0)`这句代码，再加上管程机制下"一段时间内只有一个并发进程可以执行"的规定（也就是说一段时间内只有一个并发进程可以调用`cond_signal`），我们可以印证之前的猜测——每个条件变量内部的信号量正是一个二值信号量。也就是说，**管程底层都是依赖二值信号量实现的，这也是与直接裸用信号量来实现并发编程的一个显著不同**——回顾一下之前我们是怎么用信号量实现"哲学家就餐问题"的。

此外我们似乎暂时得不到什么有效信息了。

```C++
// kern/sync/monitor.c
void monitor_init(monitor_t * mtp, size_t num_cv) {
    int i;
    assert(num_cv>0);
    mtp->next_count = 0;
    mtp->cv = NULL;
    sem_init(&(mtp->mutex), 1); //unlocked
    sem_init(&(mtp->next), 0);
    mtp->cv =(condvar_t *) kmalloc(sizeof(condvar_t)*num_cv);
    assert(mtp->cv!=NULL);
    for(i=0; i<num_cv; i++){
        mtp->cv[i].count=0;
        sem_init(&(mtp->cv[i].sem),0);
        mtp->cv[i].owner=mtp;
    }
}
```

下面我们就来看看管程机制中最核心的两个函数`cond_wait`和`cond_signal`。

在`cond_wait`中，计数器的加减，还有调用P操作`down(&cvp->sem)`使得当前进程睡下，这看上去没有什么亮点。

我们重点关注中间的if...else...代码块，它与之前我们在`phi_take_forks_condvar`函数和`phi_put_forks_condvar`函数中看到的解锁临界区的代码并无不同。至于它为什么会出现在这里，应该也是好理解的，毕竟管程机制规定了"一段时间内只有一个并发进程可以在临界区中执行"。那么现在既然调用`cond_wait`的进程即将休眠而无法在临界区中继续执行，就必须把互斥锁释放出来，以便其他并发进程进入临界区执行，或者继续在临界区中执行。

不过现在我们仍然不清楚`next`这个信号量有什么用。它是一把锁，还是其他什么东西？对此，我们还得继续分析。

```C++
void cond_wait(condvar_t *cvp) {
    monitor_t* mt = cvp->owner;
    cvp->count++;
    if (mt->next_count > 0) {
        up(&mt->next);
    } else {
        up(&mt->mutex);
    }
    down(&cvp->sem);
    cvp->count--;
}
```

最后我们来看一下另外一个核心函数`cond_signal`。

可以看到它只有在"有进程正在等待当前条件变量成立"时才会进一步执行具体的操作。而调用`up(&cvp->sem)`唤醒其中一个正在等待的进程则与`cond_wait`函数中的`down(&cvp->sem)`，也不必多说。

那么接下来`cond_signal`函数还应该完成一件什么事呢？对啦，根据管程机制的规定，当前调用`cond_signal`的这个进程自己还得睡下去！显而易见，这是通过`down(&mt->next)`这个P操作实现的。

```C++
void cond_signal(condvar_t *cvp) {
    if (cvp->count > 0) {
        monitor_t* mt = cvp->owner;
        mt->next_count++;
        up(&cvp->sem);
        down(&mt->next);
        mt->next_count--;
    }
}
```

那么这个进程睡下后什么时候会被唤醒呢？这就需要我们回忆一下在哪里出现了V操作`up(&mt->next)`。没错，就是在`cond_wait`函数，以及`phi_take_forks_condvar`函数和`phi_put_forks_condvar`函数中临界区后面的地方！

现在，我们终于知道管程机制中的并发进程间来回切换、确保一段时间内始终只有一个并发进程能执行的效果是怎么实现的了：

*   当某个调用`cond_signal`并发进程通过`down(&mt->next)`睡眠前，执行`up(&cvp->sem)`以唤醒一个正在等待条件变量成立的进程。
*   当某个并发进程调用`cond_wait`睡眠前，
*   *   如果存在（`mt->next_count>0`）另外的一个已经进入临界区的、因为调用`cond_signal`而睡眠的并发进程，那么就透过针对`mt->next`信号量的V操作`up(&mt->next)`唤醒之。这样可以使得这个已经进入临界区的并发进程得以苏醒并继续往下执行。
*   *   如果不存在这样的进程，那么就在睡眠前执行`up(&mt->mutex)`释放大锁。这样可以确保还未进入临界区的并发进程得以进入临界区执行。
*   当某个并发进程退出前，也要执行上述操作。这也是容易理解的，毕竟现在这个并发进程执行完了，那我们总得再找一个并发进程接着执行。而我们又不能找那些正在等待条件变量成立的进程来继续执行，那就只能找因调用`cond_signal`而休眠的进程了！

# 参考资料

<https://kiprey.github.io/2020/09/uCore-7/>

<https://chyyuu.gitbooks.io/ucore_os_docs/content/lab7/lab7_3_4_monitors.html>

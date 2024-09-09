和lab3一样，lab4也是很简单的，只需要按照uCore源码中的注释按部就班地填写代码就行。

# 练习1：分配并初始化一个进程控制块

## 代码

```C++
static struct proc_struct *
alloc_proc(void) {
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL) {
        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->runs = 0;
        proc->kstack = 0;
        proc->parent = NULL;
        proc->mm = NULL;
        memset(&proc->context, 0x00, sizeof(struct context));
        proc->tf = NULL;
        proc->cr3 = 0;
        proc->flags = 0;
        memset(&proc->name, 0x00, sizeof(proc->name));
    }
    return proc;
}
```

## 请说明proc\_struct中struct context context和struct trapframe \*tf成员变量含义和在本实验中的作用是啥？

实际上这个问题等完全做完了用户进程之前的切换实验之后再来回答会比较好。这里我先大致讲一下，在单核CPU上，我对context（全称应该叫做scheduler context）和trapframe这两个概念区别的私见。

我们知道，操作系统实现进程间切换在硬件上依赖CPU的时钟中断。直观上来看，假设现在有两个进程A和B，现在CPU正在执行进程A，然后时钟中断来了，进程A被中断执行，其执行上下文被存入trapframe当中。接下来CPU跳到时钟中断服务程序（也就是所谓的调度器scheduler），调度器决定接下来执行进程B，然后操作系统内核根据进程B的trapframe来进行恢复现场，最后调用`iret`跳到进程B继续执行。

这个过程看上去没啥毛病，但深究一下其实还是存在问题的。我们分别想象自己是进程A和进程B，再来思考一下上述的过程：

*   对于进程A来说，它观察到的现象是：开始时自己的用户代码正在执行，继而被时钟中断打断执行。CPU切换到内核态，并将栈寄存器切换成操作系统为自己分配的内核栈，并进入内核态执行中断处理程序（调度器）。
*   对于进程B来说，它观察到的现象是：开始时自己的内核代码（调度器）正在执行，最后调度器决定接下来CPU应该恢复执行进程B的用户代码。紧接着内核代码恢复保存在进程B自己的trapframe中的现场信息，并最终调用iret回到进程B自己的用户代码继续执行。这看上去就仿佛进程B的用户代码在被中断之后恢复执行一样。

当我们分别从进程A和进程B的视角来观察这个过程，我们就会发现，无论站在哪方的角度，都会有执行内核中断处理程序（调度器）这个过程：

*   对进程A来说，就是调度器让CPU暂时放弃执行自己，并决定接下来要执行进程B的这个过程。
*   对进程B来说，就是调度器通过恢复中断现场的方式真正让自己继续执行的这个过程。

也就是说，处于内核态的CPU在执行调度器代码时，有一个将CPU的控制权从进程A移交给进程B的中间过程。这个过程中，前半段我们认为CPU控制权仍在进程A手中，CPU仍然在为进程A执行内核代码；后半段我们认为CPU控制权已经移交给了进程B，CPU现在在为进程B执行内核代码。

现在我们来思考一下：进程A开始时被时钟中断并且被调度器决定暂停执行，后来的某个时刻经过操作系统调度又会被CPU重新捡起来继续执行。那么我们倘若仅仅恢复进程A的trapframe所保存的现场信息，就够了吗？

结合前面我们的讨论，可以知道这是不够的。因为当CPU的控制权还在A手上的时候，CPU还为进程A执行了一些内核调度器的代码，紧接着CPU的控制权转移到了进程B手中。虽然，此时在进程B的视角中，CPU还未从内核态回到进程B的用户代码继续执行。**因此在下次时钟中断发生后，恢复进程A的现场时，我们应该首先将CPU的状态恢复到其在执行调度器代码时，其控制权由进程A转交给进程B前一刻的状态，才能进一步确保操作系统调度的正确性。** 而为了保存CPU控制权从进程A被真正转交给进程B那一刻进程A的执行上下文，context这个数据结构就孕育而生了。同时，现在我们也就能理解为什么context数据结构的全称要叫做scheduler context了，因为该数据结构保存/恢复的是一个进程在执行内核调度器代码时的上下文信息。

（额，好吧，我承认这个解释确实比较牵强和抽象，还是得等到在uCore中实现进程调度之后再回过头来看这个问题...）

# 练习2：为新创建的内核线程分配资源

## 代码

```C++
int
do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf) {
    int ret = -E_NO_FREE_PROC;
    if (nr_process >= MAX_PROCESS) {
        goto fork_out;
    }
    ret = -E_NO_MEM;

    //    1. call alloc_proc to allocate a proc_struct
    struct proc_struct* child_proc = alloc_proc();
    if (child_proc == NULL) {
        goto fork_out;
    }
    child_proc->parent = current;
    //    2. call setup_kstack to allocate a kernel stack for child process
    if (setup_kstack(child_proc) != 0) {
        goto bad_fork_cleanup_proc;
    }
    //    3. call copy_mm to dup OR share mm according clone_flag
    child_proc->cr3 = boot_cr3;
    if (copy_mm(clone_flags, child_proc) != 0) {
        goto bad_fork_cleanup_kstack;
    }
    //    4. call copy_thread to setup tf & context in proc_struct
    copy_thread(child_proc, stack, tf);
    //    5. insert proc_struct into hash_list && proc_list
    bool intr = 0;
    local_intr_save(intr);
    {
        ++nr_process;
        child_proc->pid = get_pid();
        hash_proc(child_proc);
        list_add_before(&proc_list, &(child_proc->list_link));
    }
    local_intr_restore(intr);
    //    6. call wakeup_proc to make the new child process RUNNABLE
    wakeup_proc(child_proc);
    //    7. set ret vaule using child proc's pid
    ret = child_proc->pid;
fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(child_proc);
bad_fork_cleanup_proc:
    kfree(child_proc);
    goto fork_out;
}
```

## 请说明ucore是否做到给每个新fork的线程一个唯一的id？请说明你的分析和理由。

答案当然是可以的。不过为什么呢？

经本人考证，uCore中实现的get\_pid源自[Linux 2.4中的get\_pid实现](https://elixir.bootlin.com/linux/2.4.31/source/kernel/fork.c#L87)，不过由于uCore只是一个教学系统，所以对Linux源码中的pid生成策略进行了高度简化。

关键问题是如何来理解这个get\_pid中乍一看似乎有点复杂的规则。这里我想通过一个具体的例子来进行叙述。

假如某系统最多只能分配5个进程，且规定pid号从1开始计数，pid的最大值不能超过5。此外，我们假设所有进程的PCB块都被安排在一个链表中进行维护。每创建一个新进程，就把它添加到链表的末尾。

那么该系统启动后为起初的5个进程分配的pid号就应该依次为1、2、3、4、5，这也是它们在PCB块链表中的顺序。这在代码上是非常容易实现的——我们只需要引入一个计数器`last_pid`，使其初值为0，并且每次让它自增就可以了。当第5个进程被创建后，计数器`last_pid`的值也恰好取到5。

接下来假设系统运行了一段时间，这段时间内没有新的进程被创建，同时有的进程已经结束执行被系统回收了。我们不妨假设现在链表中的剩下的进程依次为1、4。

现在，假设我们又要创建一个新进程了，它的pid号应该取多少呢？首先，我们肯定不能让计数器`last_pid`再自增了，否则就破坏了这个情景中我们一开始的规定。**一个比较合理的做法是，把`last_pid`的值重新置成1，然后检查`last_pid`是否与，如果与当前链表中的某个进程的pid号存在冲突，则进一步调整`last_pid`的取值，直到不再有冲突发生。**

对于现在的这个例子，这看上去似乎是很容易操作的。我们先将计数器`last_pid`置成1，然后遍历进程链表，看看是不是与已有进程的pid号有冲突，如果有就让`last_pid`再自增。最后我们的进程链表就变成了1、4、2。

很好，到现在还没有任何问题。

接下来系统又运行了一段时间，这段时间内又不断有进程被创建和回收，此外操作系统也可能会出于某些原因调整进程链表中的节点顺序。在某个时刻，我们不妨假设进程链表变成了5、3、2、1，同时这时候的`last_pid`取值恰好为1。

接下来又有一个进程将要被创建，要为它分配的pid号应该为多少呢？我们先将计数器`last_pid`自增为2，然后同样地我们需要遍历进程链表，来检查现在计数器的取值与已有进程的pid号是否冲突。

先遍历5、3，没有冲突。不过当遍历到链表中的2时，我们会发现这个进程的pid与计数器取值冲突。按照之前的设计，我们此时需要将计数器自增到3，再继续遍历链表继续检查。不过，这么做对吗？

显然不对啊！在链表中的2之前明明有个3！于是乎，我们发现我们刚才设计的方案中存在一个严重的缺陷——**我们无法保证检测到冲突并导致计数器自增后，计数器新的取值与链表中之前遍历到的pid号是否是冲突的！**

见此情景，你肯定会马上想到一个brute force的做法——大不了每次计数器自增之后，我们就回到链表的开头，重新遍历整个链表，看看现在计时器的取值会不会与已有进程发生冲突嘛！如果冲突，再重复"自增-回到链表开头重新检查"的操作。

当然这么做确实可行，不过你应该也猜到了——这很慢。在实际的操作系统中，可能有上千个进程同时在跑，反复遍历PCB块链表的开销很大；而且从主观上来讲，计时器自增后频繁和链表前面进程的pid发生冲突的概率又似乎不会太大，每一次计时器自增后就要重新遍历链表也显得很夸张。由此可见，这种暴力做法的确不太划得来。

或许我们可以维护一个查找表/查找树之类的数据结构，把遍历过程中碰到的pid号全部记录下来，以便计数器自增后检查是否会与它们发生冲突。不过这事儿听上去也挺麻烦的，而且至少要付出$O(n)$的空间代价！

**有没有更聪明的办法呢？**

下面让我们来看看uCore中的get\_pid函数是如何处理这个问题的。

uCore中get\_pid函数的关键思想在于，**在遍历链表的过程中，在不开辟大量额外辅助空间的前提下，充分利用在计时器`last_pid`和链表中的某个pid发生冲突前的那些pid号，来帮助我们判断接下来发生的计数器自增是否是"安全"的，即计时器自增后是否存在与先前那些pid号发生冲突的可能性**。

具体来说，假设计时器`last_pid`现在的取值和链表中的第$x$个pid号$p_{x}$首次发生冲突而自增，那么在程序从链表中的第$1$个pid号遍历到第$x-1$个pid号的过程中，我们可以维护一个辅助变量`next_safe`，令$\text{next\_safe} = \min \{ p_i \mid i \in \mathbb{N}, 1 ≤ i < x, \text{last\_pid} < p_i \}$.

观察辅助变量`next_safe`的定义，我们应该不难理解，**倘若在计时器自增后满足$\text{last\_pid} ≥ \text{next\_safe}$，那么我们则怀疑自增后的计时器与链表中前$x-1$个pid中的某一个「可能」会发生冲突，这时就必须从头开始遍历链表重新检查**。否则我们就能确认自增后的计时器取值仍然是安全的，只需从第$x$个pid号起大胆地接着往下遍历链表，检查自增后的计数器是否与后续的pid号冲突即可。**换句话说，遍历链表过程中实时更新的辅助变量`next_safe`给出了一个计时器自增后取值的"安全上界"。**

uCore中通过上述处理手法，**仅仅花费了$O(1)$的空间代价，就避免了大量潜在的无用的"重新从头遍历链表"的操作，不可谓不巧妙！**

如果你能坚持读到这里，相信你已经对这个巧妙的算法思想有了充分的理解。接下来再来看具体的实现源码，相信也就不是什么难事了：

```C++
static int
get_pid(void) {
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++ last_pid >= MAX_PID) {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe) {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list) {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid) {
                if (++ last_pid >= next_safe) {
                    if (last_pid >= MAX_PID) {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (last_pid < proc->pid && proc->pid < next_safe) {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}
```

# 练习3：阅读代码，理解 proc\_run 函数和它调用的函数如何完成进程切换的。

```C++
void
proc_run(struct proc_struct *proc) {
    if (proc != current) {
        bool intr_flag;
        struct proc_struct *prev = current, *next = proc;
        local_intr_save(intr_flag);
        {
            current = proc;
            load_esp0(next->kstack + KSTACKSIZE);
            lcr3(next->cr3);
            switch_to(&(prev->context), &(next->context));
        }
        local_intr_restore(intr_flag);
    }
}
```

## 分析

*   `current = proc`：这条代码没什么好说的，就是将内核代码的全局指针变量`current`更新为将要执行进程的PCB结构体。
*   `load_esp0(next->kstack + KSTACKSIZE)`：更新GDT表中TSS段对应的`esp0`指针。这样接下来如果`next`进程在执行用户代码的过程中被中断，CPU才能正确地将`%esp`指针指向操作系统为该进程分配的内核栈。当然，这在lab4中还体现不出来。
*   `lcr3(next->cr3)`：更新cr3寄存器，让CPU使用`next`进程的二级页表进行内存地址映射。当然，这在lab4中还体现不出来，CPU使用的二级页表始终是`boot_pgdir`。
*   `switch_to(&(prev->context), &(next->context))`：保存`prev`进程的scheduler context，恢复`next`进程的scheduler context，将CPU的控制权正式移交给`next`进程。

在lab4中，由负责创建进程的函数`copy_thread`源码可知，被调度的`init`进程在`switch_to`执行后会跳到`forkret`函数执行。此外如果我们再进一步仔细分析源码，可以知道事实上由于lab4中CPU始终在内核态执行，执行`switch_to`函数后CPU的`%esp`指针始终指向事先调用`setup_kstack`函数为`init`进程分配的内核栈空间，不再发生任何换栈。

```C++
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf) {
    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE) - 1;
    *(proc->tf) = *tf;
    proc->tf->tf_regs.reg_eax = 0;
    proc->tf->tf_esp = esp;
    proc->tf->tf_eflags |= FL_IF;

    proc->context.eip = (uintptr_t)forkret;
    proc->context.esp = (uintptr_t)(proc->tf);
}

static void
forkret(void) {
    forkrets(current->tf);
}
```

`forkret`函数又会进一步调用汇编代码实现的`forkrets`函数。这个函数又会跳转到我们熟悉的老朋友`__trapret`代码块：

```assembler
    # return falls through to trapret...
.globl __trapret
__trapret:
    # restore registers from stack
    popal

    # restore %ds, %es, %fs and %gs
    popl %gs
    popl %fs
    popl %es
    popl %ds

    # get rid of the trap number and error code
    addl $0x8, %esp
    iret

.globl forkrets
forkrets:
    # set stack to this new process's trapframe
    movl 4(%esp), %esp
    jmp __trapret
```

`__trapret`代码块中利用`init`进程的trapframe来恢复现场，通过创建该进程时调用的`kernel_thread`函数可知，在`__trapret`代码块调用`iret`指令后，CPU会跳转到`kernel_thread_entry`继续执行。

注意，由于lab4中CPU始终以内核态的状态在跳来跳去执行，所以执行`iret`指令后并不会发生换栈！

```C++
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags) {
    struct trapframe tf;
    memset(&tf, 0, sizeof(struct trapframe));
    tf.tf_cs = KERNEL_CS;
    tf.tf_ds = tf.tf_es = tf.tf_ss = KERNEL_DS;
    tf.tf_regs.reg_ebx = (uint32_t)fn;
    tf.tf_regs.reg_edx = (uint32_t)arg;
    tf.tf_eip = (uint32_t)kernel_thread_entry;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

static int
init_main(void *arg) {
    cprintf("this initproc, pid = %d, name = \"%s\"\n", current->pid, get_proc_name(current));
    cprintf("To U: \"%s\".\n", (const char *)arg);
    cprintf("To U: \"en.., Bye, Bye. :)\"\n");
    return 0;
}

void proc_init(void) {
    // 略...
    int pid = kernel_thread(init_main, "Hello world!!", 0);
    // 略...
}
```

`kernel_thread_entry`函数也是用汇编代码来实现的。不言自明地，**与`kernel_thread`中的代码结合起来看，只需要搞明白`%edx`和`%ebx`寄存器在此发挥的作用，我们就很清楚接下来会发生什么了**。

```assembler
.text
.globl kernel_thread_entry
kernel_thread_entry:        # void kernel_thread(void)

    pushl %edx              # push arg
    call *%ebx              # call fn

    pushl %eax              # save the return value of fn(arg)
    call do_exit            # call do_exit to terminate current thread
```

最后`do_exit`函数触发，lab4的整套控制流程到这里也就宣告结束啦\~

```C++
int
do_exit(int error_code) {
    panic("process exit!!.\n");
}
```

## 在本实验的执行过程中，创建且运行了几个内核线程？

俩进程`idleproc`和`init`。在逻辑上当CPU进入`kern_init`函数执行时，我们就认为`idleproc`已经启动了，lab4不过是在`proc_init`函数中，为这个逻辑上存在的进程分配了一个PCB而已。

## 语句local\_intr\_save(intr\_flag);....local\_intr\_restore(intr\_flag);在这里有何作用?

在进程切换、修改操作系统/CPU关键配置的敏感阶段，显然我们不允许任何可屏蔽中断前来打扰，否则可能会导致无法预知的错误。所以这里在进行关键的进程切换操作前，要先把CPU的中断给关掉。

事实上，对于两个进程A和B，若进程调度结果为"从A切换到B"，那么当CPU控制权还在A手上时，A会调用`local_intr_save(intr_flag)`关掉中断，而后进入`switch_to`函数把CPU控制权转交给B，自己则进入待机状态。接下来从进程B的角度来看，当CPU控制权到它手上的时候，它会感觉自己是从`switch_to`函数退出来并继续往下执行，因此调用`local_intr_restore(intr_flag)`完成开中断操作实际上是已经掌握CPU控制权的进程B完成的。**不过在lab4中，由于还没有建立完整的进程调度机制，对于这一点在这个实验中我们同样感受不到。**

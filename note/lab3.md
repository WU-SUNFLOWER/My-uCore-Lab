相比前两个lab，lab3的必做部分是非常简单的，这里我先完成。challenge后面有空再补。

# 练习0

在uCore官方提供的源码中存在一个bug，在实验开始前需要进行修复，**否则会导致实验无法进行**。

在lab2中我们已经知道，为了确保内核代码能够正确开启页机制并架空段机制，我们需要临时创建一个页表映射，将逻辑地址区间为`[0, 4MB)`的虚存空间，映射到和逻辑地址为`[KERNBASE, KERNBASE+4MB)`相同的一片物理内存空间`[0, 4MB)`。当过渡操作完成后，我们再通过`boot_pgdir[0] = 0`这句代码把这个临时映射给撤销掉。

这看似没什么问题，但是尴尬的地方在于虽然我们已经通过手工修改页表目录的方式，看似"撤销"掉了针对这4MB空间的临时映射，但实际上这4MB虚存空间中的某些虚拟页的映射记录（即PTE项）可能已经被记录进了CPU的TLB缓存之中（在执行`enable_paging()`和`gdt_init()`的过程中）。这就导致接下来CPU若执行对这些虚拟页的访存操作，则仍然会访问到物理内存空间`[0, 4M)`，而非触发Page Fault——这显然是我们不想看到的！

解决方法也很简单，主要有两种：

*   手工调用x86 CPU提供的`invlpg`指令（uCore中提供了封装好的`tlb_invalidate`函数），以页为单位，逐页清除TLB中针对这4MB虚存空间的映射记录。
*   重新写一遍`cr3`寄存器，会导致CPU强制清空TLB缓存。

显然第二种比较方便，这里我们直接重新调用一遍`enable_paging`函数就行，因为其中包括了写`cr3`寄存器的操作。

```C++
void
pmm_init(void) {
    
    //略...
    
    //temporary map: 
    //virtual_addr 3G~3G+4M = linear_addr 0~4M = linear_addr 3G~3G+4M = phy_addr 0~4M     
    boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)];

    enable_paging();

    //reload gdt(third time,the last time) to map all physical memory
    //virtual_addr 0~4G=liear_addr 0~4G
    //then set kernel stack(ss:esp) in TSS, setup TSS in gdt, load TSS
    gdt_init();

    //disable the map of virtual_addr 0~4M
    boot_pgdir[0] = 0;

    //now the basic virtual memory map(see memalyout.h) is established.
    //check the correctness of the basic virtual memory map.
    check_boot_pgdir();

    print_pgdir();

    enable_paging();
}
```

# 练习1：给未被映射的地址映射上物理页

## 代码

```C++
int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr) {
    int ret = -E_INVAL;
    //try to find a vma which include addr
    struct vma_struct *vma = find_vma(mm, addr);

    pgfault_num++;
    //If the addr is in the range of a mm's vma?
    if (vma == NULL || vma->vm_start > addr) {
        cprintf("not valid addr %x, and  can not find it in vma\n", addr);
        goto failed;
    }
    //check the error_code
    switch (error_code & 3) {
    default:
            /* error code flag : default is 3 ( W/R=1, P=1): write, present */
    case 2: /* error code flag : (W/R=1, P=0): write, not present */
        if (!(vma->vm_flags & VM_WRITE)) {
            cprintf("do_pgfault failed: error code flag = write AND not present, but the addr's vma cannot write\n");
            goto failed;
        }
        break;
    case 1: /* error code flag : (W/R=0, P=1): read, present */
        cprintf("do_pgfault failed: error code flag = read AND present\n");
        goto failed;
    case 0: /* error code flag : (W/R=0, P=0): read, not present */
        if (!(vma->vm_flags & (VM_READ | VM_EXEC))) {
            cprintf("do_pgfault failed: error code flag = read AND not present, but the addr's vma cannot read or exec\n");
            goto failed;
        }
    }
    /* IF (write an existed addr ) OR
     *    (write an non_existed addr && addr is writable) OR
     *    (read  an non_existed addr && addr is readable)
     * THEN
     *    continue process
     */
    uint32_t perm = PTE_U;
    if (vma->vm_flags & VM_WRITE) {
        perm |= PTE_W;
    }
    addr = ROUNDDOWN(addr, PGSIZE);

    ret = -E_NO_MEM;

    //(1) try to find a pte, if pte's PT(Page Table) isn't existed, then create a PT.
    pte_t *ptep = get_pte(mm->pgdir, addr, 1);
    if (ptep == NULL) {
        cprintf("get_pte in do_pgfault failed\n");
        goto failed;
    }
    //(2) if the phy addr isn't exist, then alloc a page & map the phy addr with logical addr
    if (*ptep == 0) {
        if (pgdir_alloc_page(mm->pgdir, addr, perm) == NULL) {
            cprintf("pgdir_alloc_page in do_pgfault failed\n");
            goto failed;
        }
    }

    // 略...

   ret = 0;
failed:
    return ret;
}
```

## 请描述页目录（Page Directory）和页表项（Page Table Entry）中组成部分对ucore实现页替换算法的潜在用处。

在uCore中，如果某个虚拟页已经被换出到硬盘的swap分区，则页表中该虚拟页的PTE项则用作储存该虚拟页在硬盘中保存位置的索引信息，便于在缺页异常处理程序需要从硬盘中换回该虚拟页时，能够确定该虚拟页被储存在硬盘中的哪里。

由于每个进程都拥有自己独立的二级页表，所以在设计和实现`do_pgfault`缺页中断处理程序时我们需要传入实际触发Page Fault的那个进程的内存描述符指针（即`struct mm_struct *mm`，该结构体中包括了当前进程的页表目录指针`pde_t *pgdir`），这样才能确保页替换算法读取和修改该进程的二级页表，而非其他进程的二级页表。当然由于lab3中uCore还没有建立起进程管理机制，现在我们还不太能够感受到这点，这里只需要知道有这么一回事就行了。

## 如果ucore的缺页服务例程在执行过程中访问内存，出现了页访问异常，请问硬件要做哪些事情？

*   关闭中断。
*   若CPU当前运行在用户态，则根据GDT表中TSS段的配置信息，更新`%esp`和`%ss`寄存器，使得CPU切换到内核栈继续运行。而在lab3中CPU始终运行在内核态下，不会有换栈的操作。
*   将导致Page Fault的那条欲访问的虚拟地址存入`cr2`寄存器，根据导致Page Fault的原因生成Error Code，准备压到内核栈上。
*   保护现场，将恢复现场所需的相关信息压入内核栈，见结构体`struct trapframe`的定义：

```C++
struct trapframe {
    
    /* 略... */
    
    /* below here defined by x86 hardware */
    uint32_t tf_err;
    uintptr_t tf_eip;
    uint16_t tf_cs;
    uint16_t tf_padding4;
    uint32_t tf_eflags;
    /* below here only when crossing rings, such as from user to kernel */
    uintptr_t tf_esp;
    uint16_t tf_ss;
    uint16_t tf_padding5;
} __attribute__((packed));
```

*   索引到中断向量表下标为`0x0E`处，进入操作系统提供的中断处理程序执行。

# 练习2：补充完成基于FIFO的页面替换算法

## 代码

```C++
int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr) {

    // 略...

    else {
        if(swap_init_ok) {
            struct Page* page = NULL;
            //(1）According to the mm AND addr, try to load the content of right disk page
            //    into the memory which page managed.
            if (swap_in(mm, addr, &page) != 0) {
                cprintf("swap_in in do_pgfault failed\n");
                goto failed;
            }
            //(2) According to the mm, addr AND page, setup the map of phy addr <---> logical addr
            if (page_insert(mm->pgdir, page, addr, perm) != 0) {
                cprintf("page_insert in do_pgfault failed\n");
                goto failed;
            }
            //(3) make the page swappable.
            if (swap_map_swappable(mm, addr, page, 0) != 0) {
                cprintf("swap_map_swappable in do_pgfault failed\n");
                goto failed;
            }
            assert(page_ref(page) == 1);
            page->pra_vaddr = addr;
        }
        else {
            cprintf("no swap_init_ok but ptep is %x, failed\n",*ptep);
            goto failed;
        }
   }
   ret = 0;
failed:
    return ret;
```

```C++
static int
_fifo_map_swappable(struct mm_struct *mm, uintptr_t addr, struct Page *page, int swap_in)
{
    list_entry_t *head=(list_entry_t*) mm->sm_priv;
    list_entry_t *entry=&(page->pra_page_link);
 
    assert(entry != NULL && head != NULL);

    pte_t* ptr_pte = get_pte(mm->pgdir, addr, 0);
    assert((*ptr_pte & PTE_P) != 0);

    list_add_before(head, entry);
    return 0;
}
```

```C++
static int
_fifo_swap_out_victim(struct mm_struct *mm, struct Page ** ptr_page, int in_tick)
{
    list_entry_t *head=(list_entry_t*) mm->sm_priv;
    assert(head != NULL);
    assert(in_tick==0);
    list_entry_t* elem = list_next(head);

    //(1)  unlink the  earliest arrival page in front of pra_list_head qeueue
    assert(elem != head);
    list_del(elem);

    //(2)  set the addr of addr of this page to ptr_page
    *ptr_page = le2page(elem, pra_page_link);
    assert(ptr_page != NULL);
    
    return 0;
}
```

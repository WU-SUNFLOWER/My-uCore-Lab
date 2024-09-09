由于Lab2中uCore官方在代码里面给出了非常详细的提示性注释，只要熟悉**循环双链表数据结构的性质**和uCore中提供的**各种工具宏和工具函数**，应该还是非常容易完成的。

以下是我认为这个实验中需要重点注意和理解的几个点：

*   uCore中采用了将每个物理页的meta information（即源码中的`struct Page`数据结构）储存到特定的一片连续的物理内存区域（`pages`）中的策略（在`page_init`函数中完成初始化），而非像xv6那样直接把元信息埋到物理页帧的开头。我认为这个设计的主要目的在于便于更好地管理物理页的状态，毕竟如果直接把元信息储存到物理页中的话，一旦物理页被操作系统分配和写入数据，元信息就全部丢失了。
*   此外，由于`pages`中的`struct Page`结构体们是按照它们所对应的物理内存页首地址，自低地址向高地址存放的，通过比较指向这些结构体的`struct Page*`指针，我们很容易确定不同结构体所对应的物理页之间在物理内存中的位置关系。
*   基于这个设计，uCore中提供了丰富的工具宏和函数。借助它们，我们可以根据某个物理页面对应的`struct Page`数据结构的指针，确定其物理地址或虚拟地址，或者反过来做。
*   在实验过程中要时刻分清何时要使用物理地址，何时要使用虚拟地址。从lab2开始，操作系统kernel中的代码全部通过虚拟地址（在uCore的虚拟内存布局中，操作系统内部的代码和数据全部放置在虚拟地址`KERNBASE=0xC0000000`以上的位置）来进行访存。而在页表项和页表目录项中储存的，则全部都是真实的物理地址。

这里先贴上几个基本任务的代码，challenge任务后边有时间再弄。

# 练习1：实现 first-fit 连续物理内存分配算法

实际上`default_init_memmap`函数并不需要我们修改，直接使用源代码给出的即可：

```C++
static void
default_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    
    for (; p != base + n; p ++) {
        assert(PageReserved(p));

        p->flags = p->property = 0;
        SetPageProperty(p);
        ClearPageReserved(p);
        set_page_ref(p, 0);
        
        list_add_before(&free_list, &(p->page_link));
    }

    base->property = n;
    nr_free += n;
}
```

```C++
static struct Page *
default_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    struct Page *first_page = NULL;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n) {
            first_page = p;
            break;
        }
    }
    if (first_page != NULL) {
        struct Page* cur_page = first_page;
        for (; cur_page < first_page + n; ++cur_page) {
            assert(PageProperty(cur_page) && !PageReserved(cur_page));
            SetPageReserved(cur_page);
            ClearPageProperty(cur_page);
            list_del(&(cur_page->page_link));            
        }
        
        if (first_page->property > n) {
            cur_page->property = first_page->property - n;
        }

        nr_free -= n;
    }
    return first_page;
}
```

```C++
static void
default_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    list_entry_t* after_list_elem = list_next(&free_list);
    struct Page* after_page = le2page(after_list_elem, page_link);
    while (after_list_elem != &free_list) {
        struct Page* current_page = le2page(after_list_elem, page_link);
        if (base < current_page) {
            after_page = current_page;
            break;
        }
        after_list_elem = list_next(after_list_elem);
    }

    for (struct Page *p = base; p != base + n; p ++) {
        assert(PageReserved(p) && !PageProperty(p));
        
        p->property = 0;
        p->flags = 0;
        ClearPageReserved(p);
        SetPageProperty(p);
        set_page_ref(p, 0);

        list_add_before(after_list_elem, &(p->page_link));
    }
    
    base->property = n;

    if (base + n == after_page) {
        base->property += after_page->property;
        after_page->property = 0;
    }

    list_entry_t* current_list_elem = list_prev(&(base->page_link));
    struct Page* current_page = le2page(current_list_elem, page_link);
    if (current_list_elem != &free_list && current_page == base - 1) {
        while (current_list_elem != &free_list) {
            if (current_page->property > 0) {
                current_page->property += base->property;
                base->property = 0;
                break;
            }
            current_list_elem = list_prev(current_list_elem);
            current_page = le2page(current_list_elem, page_link);
        }
    }

    nr_free += n;

}
```

# 练习2：实现寻找虚拟地址对应的页表项（需要编程）

## 补全代码

```C++
pte_t *
get_pte(pde_t *pgdir, uintptr_t virtual_addr, bool can_create) {
    // (1) find page directory entry
    pde_t dictionary_entry = pgdir[PDX(virtual_addr)];   
    // (2) check if entry is not present
    if ((dictionary_entry & PTE_P) == 0) {
        // (3) check if creating is needed, then alloc page for page table
        // CAUTION: this page is used for page table, not for common data page
        if (can_create) {
            // (4) set page reference
            struct Page* new_page_table = alloc_page();
            if (new_page_table == NULL) return NULL;

            page_ref_inc(new_page_table);
            // (6) clear page content using memset
            memset(page2kva(new_page_table), 0x00, PGSIZE);
            // (7) set page directory entry's permission
            dictionary_entry = pgdir[PDX(virtual_addr)] = 
                page2pa(new_page_table) | PTE_P | PTE_W | PTE_U;
        } else {
            return NULL;
        }
    }

    pte_t* page_table = KADDR(PDE_ADDR(dictionary_entry));
    // (8) return page table entry
    return &page_table[PTX(virtual_addr)];     

}
```

## 如果ucore执行过程中访问内存，出现了页访问异常，请问硬件要做哪些事情？

在建立起分页机制之后，以下情况可能会导致CPU抛出Page Fault错误（中断向量号为0x0E）:

1.  页面不存在：

*   处理器访问的虚拟地址对应的页表条目（PTE）中的Present位（P位）为0，表示系统并未为虚拟地址所对应的虚拟页面分配物理内存页面。

2.  处理器尝试执行与页表条目中定义的权限不匹配的操作，例如：

*   试图写入一个只读页面，而该页面的页表条目中Write位（W位）为0。
*   用户模式下的代码试图访问一个只有内核模式（Ring 0）才能访问的页面，而该页面的页表条目中的用户模式位（U位）为0。

3.  虚拟地址超出范围：

*   处理器访问的虚拟地址超出了当前进程的虚拟地址空间范围（如试图访问超过用户空间上限的地址或空指针），导致无法在页表中找到对应的条目。

4.  页面属性冲突：

*   页表条目中的一些属性与当前操作不兼容,例如执行不可执行页面（x86中内存页面中的代码默认可以被执行，若某页面XD位或NX位被设置则禁止执行其中的代码）

5.  分页机制相关的CPU异常：

*   在启用分页机制时，某些硬件和软件错误，例如页表损坏、内存故障或处理器内部错误，也可能导致Page Fault异常的发生。

当Page Fault错误发生后，相当于在CPU内部触发了一个软中断。CPU会调用中断向量号0x0E对应的中断处理程序来进行错误处理，这里不再具体赘述。

# 练习3：释放某虚地址所在的页并取消对应二级页表项的映射

## 补全代码

```C++
static inline void
page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep) {
    //(1) check if this page table entry is present
    if ((*ptep & PTE_P)) {
        //(2) find corresponding page to pte
        struct Page* page = pte2page(*ptep);
        //(3) decrease page reference
        if (page_ref_dec(page) == 0) {
            // (4) and free this page when page reference reachs 0
            free_page(page);
        }
        // (5) clear second page table entry
        *ptep = 0;    // 这里写作*ptep &= ~PTE_P;也是可以的
        //(6) flush tlb
        tlb_invalidate(pgdir, la);
    }
}
```

## 数据结构Page的全局变量（其实是一个数组）的每一项与页表中的页目录项和页表项有无对应关系？如果有，其对应关系是啥？

有对应关系。页表目录中的某一项（PDE），或者页表中的某一项（PTE），只要P位为1（即对应的物理页面被分配），就都对应了一个具体的物理页面。

在uCore中，提供了如下的工具函数来实现这种转换：

```c++
struct Page* some_page = pte2page(pte);
struct Page* some_pt_page = pde2page(pde);

pte_t pte = page2pa(some_page) | PTE_P | PTE_W;
pte_t pde = page2pa(some_pt_page) | PTE_P | PTE_W;
```

## 如果希望虚拟地址与物理地址相等，则需要如何修改lab2，完成此事？

在正式开始讲解前，我不得不称赞一下这个题目出得还是很不错的。这个题目需要我们对x86架构中的分段/分页机制，以及编译器链接地址/加载地址这些概念有非常充分的理解，并且还能够理解uCore中在建立最终的分页内存管理机制过程中所使用的一些魔法和技巧。这些内容在接下来的讲解中我也会逐一涉及到。

首先，由于是直接映射，操作系统内核代码（数据）中的链接地址（即uCore内核运行时的虚拟地址/逻辑地址）就不应再取`0xC0100000`了，而应与内核代码（数据）被放置的物理地址（即加载地址）保持一致，即修改链接器脚本`tools/kernel.ld`中的内容如下：

```ld
/* 略... */

SECTIONS {
    /* Load the kernel at this address: "." means the current address */
    . = 0x00100000;
    
/* 略... */
```

紧接着，为了理解lab2中的代码哪里还需要修改，我们首先要知道lab2中uCore架空分段机制以及建立分页机制时的一些细节。

在lab2中，由于设置真正架空处理器段内存管理机制的GDT表的函数`gdt_init`是在启动页机制之后才被执行，而操作系统内核中的所有代码都以链接地址`0xC0100000`为基准进行生成，因此为了确保在段内存管理机制被架空前的那个过渡阶段，内核中的代码可以被正确执行，uCore的作者在进入内核`kern_init`前的汇编代码`entry.S`中搞了点小动作：

```c++
#include <mmu.h>
#include <memlayout.h>

#define REALLOC(x) (x - KERNBASE)

.text
.globl kern_entry
kern_entry:
    # reload temperate gdt (second time) to remap all physical memory
    # virtual_addr 0~4G=linear_addr&physical_addr -KERNBASE~4G-KERNBASE 
    lgdt REALLOC(__gdtdesc)
    movl $KERNEL_DS, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    ljmp $KERNEL_CS, $relocated

relocated:

    # set ebp, esp
    movl $0x0, %ebp
    # the kernel stack region is from bootstack -- bootstacktop,
    # the kernel stack size is KSTACKSIZE (8KB)defined in memlayout.h
    movl $bootstacktop, %esp
    # now kernel stack is ready , call the first C function
    call kern_init

/* 中间略... */

.align 4
__gdt:
    SEG_NULL
    SEG_ASM(STA_X | STA_R, - KERNBASE, 0xFFFFFFFF)      # code segment
    SEG_ASM(STA_W, - KERNBASE, 0xFFFFFFFF)              # data segment
__gdtdesc:
    .word 0x17                                          # sizeof(__gdt) - 1
    .long REALLOC(__gdt)
```

可以看到，在跳进内核C语言部分执行之前，uCore挂载了一个临时的GDT表。且其中用到了一个名为`KERNBASE`的宏，查看`kern/mm/mmlayout.h`可知，该宏的值为`0xC0000000`，恰好为uCore内核的链接地址`0xC0100000`与实际物理地址`0x00100000`的差值（偏移量）。进一步查看该头文件中对uCore虚拟内存布局的介绍，又可知虚拟地址`KERNBASE`往上的虚拟内存空间，恰好对应了物理地址`0`往上的物理内存空间——因此这就不难理解，为什么内核链接地址与物理地址的差值正好就是`KERNBASE`了。

搞清楚了uCore内核中虚拟内存空间与物理地址空间的映射关系，我们就可以大胆地提出一个解决方案，来化解挂载正式GDT表之前以`0xC0100000`为链接地址的内核代码无法正确被执行的难题——如果有某种办法将内核代码尝试访存的所有**绝对地址**（相对地址/偏移量不会出问题），全部都减去`KERNBASE=0xC0000000`这个偏移量，CPU不就可以正确访存了嘛！

uCore正是这么做的！现在我们来仔细看看`entry.S`中这个临时GDT表的内容，会发现其中每个分段的基地址居然都是`-KERNBASE`——这意味着什么呢？很显然，在执行`ljmp $KERNEL_CS, $relocated`后该临时GDT表就会正式生效。接下来对于内核代码中所有针对绝对逻辑地址的访问，在CPU执行访存前，都会将指令中的绝对逻辑地址先减去`KERNBASE`，得出正确的物理地址并再进行访存。通过这样的技巧，就实现了我们前述的构想。

至于该文件中出现的宏定义`REALLOC`，现在应该也不难理解了。由于在挂载临时GDT表并使之生效之前之前，CPU还无法自动完成这个减去偏移量的操作，就只能由我们自己手写代码来代劳了。

回到本题要求实现内核代码虚拟地址与物理地址相等的要求。既然我们已经修改过了`tools/kernel.ld`中的链接地址，使之等于放置内核代码/数据真正的物理地址，那么编译器生成的访存代码中的绝对逻辑地址，也应当与真实的物理地址相等。我们就不需要技巧性这么强的骚操作了，直接将`kern/mm/mmlayout.h`中的宏`KERNBASE`修改一下就行了：

```c++
#define KERNBASE            0x00000000
```

到这里就可以实现本题的要求了吗？实际上还不行，我们还需要修改一处地方。让我们来看看`pmm_init`这个函数：

```C++
void
pmm_init(void) {

    // 前略...

    // map all physical memory to linear memory with base linear addr KERNBASE
    //linear_addr KERNBASE~KERNBASE+KMEMSIZE = phy_addr 0~KMEMSIZE
    //But shouldn't use this map until enable_paging() & gdt_init() finished.
    boot_map_segment(boot_pgdir, KERNBASE, KMEMSIZE, 0, PTE_W);

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

}
```

`enable_paging`和`gdt_init`这两个比较关键的函数的源码，由于篇幅原因我这里就不贴了。简单来说，`enable_paging`这个函数会把我们已经通过`boot_map_segment`函数初始化好的内核页表`boot_pgdir`的物理地址给正式放进cr3寄存器里，并通过修改cr0寄存器正式为CPU启用分页内存管理机制。`gdt_init`则负责通过lgdt指令将把架空CPU分段内存管理机制的正式GDT表给挂到CPU上。这看上去一点儿毛病都没有，不是这样吗？

事实上这是会出问题的。我们知道，x86架构的CPU在处理被执行程序中的逻辑地址时，采用的是如下流程：

> **逻辑地址（虚拟地址）**=> 分段机制映射得到**线性地址** => 分页机制映射得到真正的**物理地址**

那么问题就来了。在使用`boot_map_segment`函数初始化二级页表时，实际上我们已经假设该二级页表应该是在CPU分段机制已经被架空的情况下，发挥将代码中的逻辑地址（虚拟地址）映射为真实物理地址的功能。而现在`gdt_init`函数还没有执行，CPU分段机制还未被架空的情况下，就直接调用`enable_paging`试图启用分页机制，势必会导致分页机制启动后，CPU就根本无法正确解析接下来要执行的代码中的逻辑地址，调用`gdt_init`以架空分段机制这事儿自然也就无从谈起了。

那我们将这两个函数的调用顺序调换一下，行吗？很遗憾，这样也会导致问题。倘若我们首先架空了CPU的分段机制，那么由于CPU的分页机制还没有启用，又会出现CPU无法正确解析将要执行代码中的逻辑地址的问题。因此试图先架空分段机制，再调用`enable_paging`启动分页机制这事儿，也就成了无稽之谈。

分析到这，我们似乎陷入了一个"先有鸡还是先有蛋"的怪圈之中。有办法破局吗？我们再来仔细分析一下这个问题。

假如我们仍然采用先`enable_paging`后`gdt_init`的顺序，在分页机制已启动但分段机制未被架空的过渡阶段，对于uCore内核代码中的某个逻辑地址`0xc01034d6`，CPU会先使用分段机制对其进行处理，即减去`0xC0000000`得到线性地址，再将`0x001034d6`进行分页映射，求出物理地址——当然我们知道这肯定是个错的物理地址。而实际上我们希望得到什么结果呢？自然就是直接对逻辑地址`0xc01034d6`进行分页映射，得到正确的物理地址。

这时候，我们不妨想一想，既然在逻辑地址`0xc01034d6`被分页映射前我们无法避免其被还未架空的分段机制"割上一刀"（减去`0xC0000000`），那如果有某种办法，能够让被"割上一刀"的线性地址，经过分页机制，也能被映射到正确的物理地址，这个问题不就能够被完美解决吗？！

回看`pmm_init`中的源码，我们发现uCore恰恰就是这么做的。下面我们来分析一下`boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)];`这句关键代码。

在学习过多级页表基础知识的基础上，我们不难理解`boot_pgdir[PDX(KERNBASE)]`这个PDE项指向的直接页表中存放着`[KERNBASE, KERNBASE+4MB)`这段虚拟内存空间到物理内存的映射，而这段虚拟内存空间恰好应该包括了uCore的全部内核代码/数据。这是因为在uCore的虚拟内存布局中，其内核代码/数据放置的起始地址为`KERNBASE+1MB`（和真实的物理地址之间正好差了一个偏移量`KERNBASE`），而检查编译后得到的`bin/kernel`文件可知uCore的内核代码/数据大小不过也就一百多kb，因此`[KERNBASE, KERNBASE+4MB)`这段虚拟内存空间是可以完全覆盖得到的。

现在，在执行`boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)];`这条代码后，范围为`[0, 4MB)`的经过分段机制"割上一刀"的线性地址，也会被分页机制映射到和最终建立的虚拟地址空间`[KERNBASE, KERNBASE+4MB)`（即没有被分段机制"割上一刀"）相同的物理内存空间当中去——**就仿佛分段机制减去`KERNBASE`的效果没有发生过一般**！

**通过这样巧妙的处理，我们就可以确保在已启动分页机制，但还未架空分段机制的情况下，内核代码仍然能够被CPU正确执行。**

在分段机制被正式架空后，我们只需要将多级页表中的这个临时映射取消掉即可（`boot_pgdir[0] = 0;`），因为后面虚拟内存空间中的低地址部分还要预留给应用程序使用。

在充分理解了lab2中uCore在`pmm_init`中使用的魔法技巧之后，我们就很容易看出该函数中哪里还需要进行修改了。很显然，我们需要前述魔法的根本原因，还是在于`entry.S`中临时GDT表埋下的祸根。而现在由于我们已经将宏`KERNBASE`修改为了0，**实际上这就导致了临时GDT表已经起到了所谓"架空分段机制"的效果**，因此`pmm_init`里的魔法技巧自然也就完全不需要了，我们直接将其注释掉即可：

```C++
void
pmm_init(void) {

    // 前略...

    //temporary map: 
    //virtual_addr 3G~3G+4M = linear_addr 0~4M = linear_addr 3G~3G+4M = phy_addr 0~4M     
    //boot_pgdir[0] = boot_pgdir[PDX(KERNBASE)];

    enable_paging();

    //reload gdt(third time,the last time) to map all physical memory
    //virtual_addr 0~4G=liear_addr 0~4G
    //then set kernel stack(ss:esp) in TSS, setup TSS in gdt, load TSS
    gdt_init();

    //disable the map of virtual_addr 0~4M
    //boot_pgdir[0] = 0;

    //因为现在虚拟地址已经等于物理地址了，原来的检查函数不能再要了，也直接注释掉
    //check_boot_pgdir();

    print_pgdir();

}
```

ok，到此为止可以说我们要改的地方都改掉了，来看看最终的效果：

![image.png](https://p0-xtjj-private.juejin.cn/tos-cn-i-73owjymdk6/dd206862eb0f4fe3807cfaa1b7540cc9~tplv-73owjymdk6-jj-mark-v1:0:0:0:0:5o6Y6YeR5oqA5pyv56S-5Yy6IEAgUEFL5ZCR5pel6JG1:q75.awebp?policy=eyJ2bSI6MywidWlkIjoiMzU0NDQ4MTIyMDAwODc0NCJ9&rk3s=f64ab15b&x-orig-authkey=f32326d3454f2ac7e96d3d06cdbb035152127018&x-orig-expires=1726510464&x-orig-sign=dtTGqxdjoukQs2EluhKEyUhu%2BkY%3D)

可以看到uCore已经成功建立起了和物理内存空间`[0x00000000, 0x38000000)`地址完全一致的虚拟内存空间，搞定！

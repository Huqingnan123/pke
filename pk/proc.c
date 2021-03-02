#include "proc.h"
//#include <kmalloc.h>
#include <string.h>
#include "pmm.h"
#include "mmap.h"
#include <error.h>
#include <sched.h>
#include "atomic.h"
#include "boot.h"
#include "error.h"
#include "intr.h"
#include "pk.h"
#include "defs.h"


// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT          10
#define HASH_LIST_SIZE      (1 << HASH_SHIFT)
#define pid_hashfn(x)       (hash32(x, HASH_SHIFT))

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32       0x9e370001UL

/* *
 * hash32 - generate a hash value in the range [0, 2^@bits - 1]
 * @val:    the input value
 * @bits:   the number of bits in a return value
 *
 * High bits are more random, so we use them.
 * */
uint32_t
hash32(uint32_t val, unsigned int bits) {
    uint32_t hash = val * GOLDEN_RATIO_PRIME_32;
    return (hash >> (32 - bits));
}

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];
 
// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *currentproc = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(trapframe_t *tf,pte_t * pagetable);
void switch_to(struct context *from, struct context *to);

static spinlock_t vm_lock = SPINLOCK_INIT;

// Create a user page table for a given process,
// with no user memory, but with trap_entry and user stack.
pte_t*  proc_pagetable()
{
    //panic("finish your code in proc_pagetable\n");
    //return 0;
    extern uintptr_t first_free_paddr;
    pte_t* pagetable = (pte_t *)__page_alloc();
    pte_t* stackPage = (pte_t *)__page_alloc();
    //在所有进程的用户页表中映射->由用户态进入内核态的代码
    __map_kernel_range_pgtbl(DRAM_BASE, DRAM_BASE, first_free_paddr - DRAM_BASE, PROT_READ|PROT_EXEC, (uintptr_t)pagetable);
    //每个用户页表中都应该维护自己的用户堆栈，需要对虚拟地址current.stack_top-RISCV_PGSIZE进行映射
    mappages(pagetable, current.stack_top - RISCV_PGSIZE, RISCV_PGSIZE, (uint64_t)stackPage, prot_to_type(PROT_READ|PROT_EXEC|PROT_WRITE, 1));
    return pagetable;

}

// alloc_proc - alloc a proc_struct and init all fields of proc_struct
static struct proc_struct *
alloc_proc(void) {
    //truct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    struct proc_struct *proc = (struct proc_struct*)__page_alloc();
    if (proc != NULL) {
    //LAB4:EXERCISE1 YOUR CODE
    /*
     * below fields in proc_struct need to be initialized
     *       enum proc_state state;                      // Process state
     *       int pid;                                    // Process ID
     *       int runs;                                   // the running times of Proces
     *       uintptr_t kstack;                           // Process kernel stack
     *       volatile bool need_resched;                 // bool value: need to be rescheduled to release CPU?
     *       struct proc_struct *parent;                 // the parent process
     *       struct mm_struct *mm;                       // Process's memory management field
     *       struct context context;                     // Switch here to run process
     *       struct trapframe *tf;                       // Trap frame for current interrupt
     *       uintptr_t upagetable;                              // CR3 register: the base addr of Page Directroy Table(PDT)
     *       uint32_t flags;                             // Process flag
     *       char name[PROC_NAME_LEN + 1];               // Process name
     * 
     * then remove the panic; 
     */
//        panic("you need add code in alloc_proc() first");
	//   proc->state = PROC_UNINIT;
	//   proc->pid = -1;
	//   proc->runs = 0;
	//   proc->kstack = 0;
	//   proc->need_resched = 0;
	//   proc->parent = NULL;
	//   memset(&(proc->context), 0, sizeof(struct context));
	//   proc->tf = NULL;
	//   proc->upagetable = (uintptr_t)root_page_table;
	//   proc->flags = 0;
	//   memset(proc->name, 0, PROC_NAME_LEN);

        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->runs = 0;
        proc->kstack = 0;
        proc->need_resched = 0;
        proc->parent = NULL;
        memset(&(proc->context), 0, sizeof(struct context));
        proc->tf = NULL;
        proc->upagetable =(uintptr_t)proc_pagetable() ;
        proc->flags = 0;
        memset(proc->name, 0, PROC_NAME_LEN);
        proc->wait_state = 0;
        proc->cptr = proc->optr = proc->yptr = NULL;
        proc->rq = NULL;
        list_init(&(proc->run_link));
        proc->time_slice = 0;
    }
    return proc;
}

// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name) {
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, PROC_NAME_LEN);
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc) {
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// get_pid - alloc a unique pid for process
static int
get_pid(void) {
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
            else if (proc->pid > last_pid && next_safe > proc->pid) {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}


void load_elf_test(const char* fn, elf_info* info);

// proc_run - make process "proc" running on cpu
// NOTE: before call switch_to, should load  base addr of "proc"'s new PDT
void
proc_run(struct proc_struct *proc) {
  
    if (proc != currentproc) {
        bool intr_flag;
        struct proc_struct *prev = currentproc, *next = proc;
        currentproc = proc;
        switch_to(&(prev->context), &(next->context));
        
    }
}

// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void) {
    extern elf_info current;
    int pid=currentproc->pid;
    struct proc_struct * proc=find_proc(pid);
    write_csr(sscratch, proc->tf);
    set_csr(sstatus, SSTATUS_SUM | SSTATUS_FS);
    currentproc->tf->status = (read_csr(sstatus) &~ SSTATUS_SPP &~ SSTATUS_SIE) | SSTATUS_SPIE;
    uintptr_t satp=MAKE_SATP(currentproc->upagetable);
    forkrets(currentproc->tf,(pte_t *)satp);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc) {
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid) {
    if (0 < pid && pid < MAX_PID) {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list) {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid) {
                return proc;
            }
        }
    }
    return NULL;
}

// kernel_thread - create a kernel thread using "fn" function
// NOTE: the contents of temp trapframe tf will be copied to 
//       proc->tf in do_fork-->copy_thread function
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags) {
    trapframe_t tf;
    memset(&tf, 0, sizeof(trapframe_t));

    tf.gpr[8] = (uintptr_t)fn;
    tf.gpr[9] = (uintptr_t)arg;
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    tf.epc = (uintptr_t)kernel_thread_entry;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc) {
	proc->kstack = (uintptr_t)__page_alloc();
    if(mappages((pte_t *)proc->upagetable,(uintptr_t) proc->kstack, RISCV_PGSIZE,
               (uintptr_t)proc->kstack,prot_to_type(PROT_READ|PROT_WRITE|PROT_EXEC,0))< 0){
   // uvmfree(pagetable, 0);
    panic("setup_kstack fali"); 
    return 0;
  }

    return 0;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc) {
    // printk("free kstack: %s\n",proc->name);
}


static void kfree(struct proc_struct *proc) {
	//printk("free proc: %s\n",proc->name);
}


// copy_mm - process "proc" duplicate OR share process "current"'s mm according clone_flags
//         - if clone_flags & CLONE_VM, then "share" ; else "duplicate"
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc) {
    vmcopy((pte_t *)proc->parent->upagetable,(pte_t *)proc->upagetable);
    return 0;
}

// copy_thread - setup the trapframe on the  process's kernel stack top and
//             - setup the kernel entry point and stack of process
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, trapframe_t *tf) {
    proc->tf = (trapframe_t *)(proc->kstack + KSTACKSIZE - sizeof(trapframe_t));
    *(proc->tf) = *tf;
    proc->tf->k_satp=MAKE_SATP(root_page_table);
    proc->tf->gpr[10] = 0;
    proc->tf->gpr[2] = (esp == 0) ? (uintptr_t)proc->tf -4 : esp;

    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)(proc->tf);
}


int
do_fork(uint32_t clone_flags, uintptr_t stack,  trapframe_t *tf) {

    int ret = -E_NO_FREE_PROC;
    struct proc_struct *proc;
    if (nr_process >= MAX_PROCESS) {
        goto fork_out;
    }
    ret = -E_NO_MEM;
    //LAB4:EXERCISE2 YOUR CODE
    /*
     * Some Useful MACROs, Functions and DEFINEs, you can use them in below implementation.
     * MACROs or Functions:
     *   alloc_proc:   create a proc struct and init fields (lab4:exercise1)
     *   setup_kstack: alloc pages with size KSTACKPAGE as process kernel stack
     *   copy_mm:      process "proc" duplicate OR share process "current"'s mm according clone_flags
     *                 if clone_flags & CLONE_VM, then "share" ; else "duplicate"
     *   copy_thread:  setup the trapframe on the  process's kernel stack top and
     *                 setup the kernel entry point and stack of process
     *   hash_proc:    add proc into proc hash_list
     *   get_pid:      alloc a unique pid for process
     *   wakeup_proc:  set proc->state = PROC_RUNNABLE
     * VARIABLES:
     *   proc_list:    the process set's list
     *   nr_process:   the number of process set
     */

    //    1. call alloc_proc to allocate a proc_struct
    //    2. call setup_kstack to allocate a kernel stack for child process
    //    3. call copy_mm to dup OR share mm according clone_flag
    //    4. call copy_thread to setup tf & context in proc_struct
    //    5. insert proc_struct into hash_list && proc_list
    //    6. call wakeup_proc to make the new child process RUNNABLE
    //    7. set ret vaule using child proc's pid

	//LAB5 YOUR CODE : (update LAB4 steps)
   /* Some Functions
    *    set_links:  set the relation links of process.  ALSO SEE: remove_links:  lean the relation links of process 
    *    -------------------
	*    update step 1: set child proc's parent to current process, make sure current process's wait_state is 0
	*    update step 5: insert proc_struct into hash_list && proc_list, set the relation links of process
    */
    if ((proc = alloc_proc()) == NULL) {
        goto fork_out;
    }

    proc->parent = currentproc;
    assert(currentproc->wait_state == 0);

    if (setup_kstack(proc) != 0) {
        goto bad_fork_cleanup_proc;
    }
    if (copy_mm(clone_flags, proc) != 0) {
        goto bad_fork_cleanup_kstack;
    }

    copy_thread(proc, stack, tf);

    bool intr_flag;
    local_intr_save(intr_flag);
    {
        proc->pid = get_pid();
        hash_proc(proc);
      //  set_links(proc);
    }
    local_intr_restore(intr_flag);

    wakeup_proc(proc);
   
    ret = proc->pid;


fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    goto fork_out;
}



// init_main - the second kernel thread used to create user_main kernel threads
static int
init_main(void *arg) {
    printk("this initproc, pid = %d, name = \"%s\"\n", currentproc->pid, get_proc_name(currentproc));
    printk("To U: \"%s\".\n", (const char *)arg);
    printk("To U: \"en.., Bye, Bye. :)\"\n");
    return 0;
}

// proc_init - set up the first kernel thread idleproc "idle" by itself and 
//           - create the second kernel thread init_main
void
proc_init() {
    int i;
    extern uintptr_t kernel_stack_top; 

    list_init(&proc_list);
    for (i = 0; i < HASH_LIST_SIZE; i ++) {
        list_init(hash_list + i);
    }

    if ((idleproc = alloc_proc()) == NULL) {
        panic("cannot alloc idleproc.\n");
    }

    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    idleproc->kstack = kernel_stack_top; 
    idleproc->need_resched = 1;
    set_proc_name(idleproc, "idle");
    nr_process ++;

    currentproc = idleproc;
    initproc=idleproc;
    printk("log: proc init \n");
}

// cpu_idle - at the end of kern_init, the first kernel thread idleproc will do below works
void
cpu_idle(void) {
    while (1) {
        if (currentproc->need_resched) {
            schedule();
        }
    }
}



// unhash_proc - delete proc from proc hash_list
static void
unhash_proc(struct proc_struct *proc) {
    list_del(&(proc->hash_link));
}

// remove_links - clean the relation links of process
static void
remove_links(struct proc_struct *proc) {
    list_del(&(proc->list_link));
    if (proc->optr != NULL) {
        proc->optr->yptr = proc->yptr;
    }
    if (proc->yptr != NULL) {
        proc->yptr->optr = proc->optr;
    }
    else {
       proc->parent->cptr = proc->optr;
    }
    nr_process --;
}


// do_wait - wait one OR any children with PROC_ZOMBIE state, and free memory space of kernel stack
//         - proc struct of this child.
// NOTE: only after do_wait function, all resources of the child proces are free.
int
do_wait(int pid, int *code_store) {
    //panic("finish your code in  do_wait\n");
    //return 0;
    struct proc_struct* proc;
    bool intr_flag;
    while(1) 
    { 
        if(pid != 0) 
        {
            proc = find_proc(pid);
            if(proc->state == PROC_ZOMBIE)
                break;
        }
        else 
        {
            for(proc = currentproc->cptr; proc != NULL; proc = proc->cptr) 
            {
                if(proc->state == PROC_ZOMBIE)
                    break;
            }
        }
        currentproc->state = PROC_SLEEPING;
        currentproc->wait_state = WT_CHILD;
        schedule();
    }
    //kill child-proc and free resource
    local_intr_save(intr_flag);
    {
        unhash_proc(proc);
    }
    local_intr_restore(intr_flag);
    return 0;
}


// do_exit - called by sys_exit
//   1. call exit_mmap & put_pgdir & mm_destroy to free the almost all memory space of process
//   2. set process' state as PROC_ZOMBIE, then call wakeup_proc(parent) to ask parent reclaim itself.
//   3. call scheduler to switch to other process
int
do_exit(int error_code) {
    if (currentproc == idleproc) {
        panic("idleproc exit.\n");
    }
    if (currentproc == initproc) {
        panic("initproc exit.\n");
    }
    
   
    currentproc->state = PROC_ZOMBIE;
    currentproc->exit_code = error_code;
    
    bool intr_flag;
    struct proc_struct *proc;
    local_intr_save(intr_flag);
    {
        proc = currentproc->parent;
        if (proc->wait_state == WT_CHILD) {
            wakeup_proc(proc);
        }
        while (currentproc->cptr != NULL) {
            proc = currentproc->cptr;
            currentproc->cptr = proc->optr;
    
            proc->yptr = NULL;
            if ((proc->optr = initproc->cptr) != NULL) {
                initproc->cptr->yptr = proc;
            }
            proc->parent = initproc;
            initproc->cptr = proc;
            if (proc->state == PROC_ZOMBIE) {
                if (initproc->wait_state == WT_CHILD) {
                    wakeup_proc(initproc);
                }
            }
        }
    }
    local_intr_restore(intr_flag);
    
    schedule();
    panic("do_exit will not return!! %d.\n", currentproc->pid);
}

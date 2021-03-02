#include "semap.h"
#include "proc.h"
#include "intr.h"
#include "sched.h"

void
sem_init(semaphore_t *sem, int value) {
    sem->value = value;
    wait_queue_init(&(sem->wait_queue));
}

static __noinline void __up(semaphore_t *sem, uint64_t wait_state) {
 //   printk("5_2 up vaddr %p value %d\n",sem->vaddr,sem->value);
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        wait_t *wait;
        if ((wait = wait_queue_first(&(sem->wait_queue))) == NULL) {
            sem->value ++;
        }
        else {
            kassert(wait->proc->wait_state == wait_state);
            wakeup_wait(&(sem->wait_queue), wait, wait_state, 1);
        }
    }
    local_intr_restore(intr_flag);
}

static __noinline uint64_t __down(semaphore_t *sem, uint64_t wait_state) {
    //panic("finish your code in __down\n");
    //return 0;

    //if value > 0, minus 1 directly
    if(try_down(sem) == 1)
        return 0;
    //or add currentproc to wait_queue
    wait_t wait;
    wait_current_set(&(sem->wait_queue), &wait, wait_state);
    schedule();
    //after schecule, judge wait_state == WT_KSEM
    if((wait.proc)->wait_state == WT_KSEM)
        wakeup_wait(&(sem->wait_queue), &wait, wait_state, 1);
    return 0;
}

void
up(semaphore_t *sem) {
    __up(sem, WT_KSEM);
}

void
down(semaphore_t *sem) {
    uint64_t flags = __down(sem, WT_KSEM);
    kassert(flags == 0);
}

bool
try_down(semaphore_t *sem) {
    bool intr_flag, ret = 0;
    local_intr_save(intr_flag);
    if (sem->value > 0) {
        sem->value --, ret = 1;
    }
    local_intr_restore(intr_flag);
    return ret;
}



void init_semaq(){
    semaphore_t * se;
    for(se=sema_q;se<&sema_q[NSEMA];se++){
         se->value = 0;
         se->vaddr=0;
         wait_queue_init(&(se->wait_queue));
    }
}

semaphore_t* alloc_sema(intptr_t sema_va){
    int found=0;
    semaphore_t * se;
    int value=0;
    copyin((pte_t *)currentproc->upagetable,(char *)&value,sema_va,sizeof(int));
   // printk("5_2 value %d\n",value);
    for(se=sema_q;se<&sema_q[NSEMA];se++){
      if(se->vaddr==0){
        se->vaddr=sema_va;
        sem_init(se,value);
        printk("5_2 alloc sema va %p pa %p  value %d \n",sema_va,se,se->value);
        found=1;
        break;
      }else
      {
        continue;
      }
    }

    if(found==0){
      panic("no sema alloc\n");
    }
    return se;
}

semaphore_t * find_sema(intptr_t sema_va){
    semaphore_t * se;
    for(se=sema_q;se<&sema_q[NSEMA];se++){
      if(se->vaddr==sema_va){
  //      printk("5_2 find sema va %p pa %p  value\n",sema_va,se,se->value);
        return se;
        }
    }  
 //   printk("no sema find\n");
    return NULL;
}
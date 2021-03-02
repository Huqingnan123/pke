// See LICENSE for license details.
#include "pmm.h"
#include "mmap.h"
#include "atomic.h"
#include "pk.h"
#include "boot.h"
#include "bits.h"
#include "defs.h"
#include "mtrap.h"
#include <stdint.h>
#include <errno.h>
#include "proc.h"


typedef struct {
  uintptr_t addr;
  size_t length;
  file_t* file;
  size_t offset;
  unsigned refcnt;
  int prot;
} vmr_t;
uint64_t k_satp=0xffffffff;
uint64_t * k_satp_p=&k_satp;
#define MAX_VMR (RISCV_PGSIZE / sizeof(vmr_t))
static spinlock_t vm_lock = SPINLOCK_INIT;
// static vmr_t* vmrs;
vmr_t* vmrs;

uintptr_t kernel_stack_top; 
uintptr_t first_free_paddr;
static uintptr_t first_free_page;
static size_t next_free_page;
static size_t free_pages;

int demand_paging = 1; // unless -p flag is given

uintptr_t __page_alloc()
{
  kassert(next_free_page != free_pages);
  uintptr_t addr = first_free_page + RISCV_PGSIZE * next_free_page++;
  memset((void*)addr, 0, RISCV_PGSIZE);
  return addr;
}

static vmr_t* __vmr_alloc(uintptr_t addr, size_t length, file_t* file,
                          size_t offset, unsigned refcnt, int prot)
{
  if (!vmrs) {
    spinlock_lock(&vm_lock);
      if (!vmrs) {
        vmr_t* page = (vmr_t*)__page_alloc();
        mb();
        vmrs = page;
      }
    spinlock_unlock(&vm_lock);
  }
  mb();

  for (vmr_t* v = vmrs; v < vmrs + MAX_VMR; v++) {
    if (v->refcnt == 0) {
      if (file)
        file_incref(file);
      v->addr = addr;
      v->length = length;
      v->file = file;
      v->offset = offset;
      v->refcnt = refcnt;
      v->prot = prot;
      return v;
    }
  }
  return NULL;
}

static void __vmr_decref(vmr_t* v, unsigned dec)
{
  if ((v->refcnt -= dec) == 0)
  {
    if (v->file)
      file_decref(v->file);
  }
}
/* PTE(页表项)与PDE（页目录项）的结构
31                     20 19                10 9      8 7 6 5 4 3 2 1 0
+------------------------+--------------------+--------+---------------+
|         PPN[1]         |       PPN[0]       |Reserved|D|A|G|U|X|W|R|V|
+-----------12-----------+---------10---------+----2---+-------8-------+
*/
static size_t pte_ppn(pte_t pte)
{
  return pte >> PTE_PPN_SHIFT;
}


/*
一个虚拟地址的结构如下
31                 22 21                12 11                     0
+--------------------+--------------------+------------------------+
|       VPN[1]       |       VPN[0]       |      page offset       |
+---------10---------+---------10---------+----------12------------+

一个物理地址（34位）的结构如下
33                     22 21                12 11                     0
+------------------------+--------------------+------------------------+
|         PPN[1]         |       PPN[0]       |       page offset      |
+-----------12-----------+---------10---------+-----------12-----------+
不过pk实际只用了低32位,和虚拟地址一样
*/
static uintptr_t ppn(uintptr_t addr)
{
  return addr >> RISCV_PGSHIFT;
}

//level = 1, 得到vpn[1]，即页目录项在一级页表的序号
//level = 0, 得到vpn[0]，即页表项在二级页表的序号
static size_t pt_idx(uintptr_t addr, int level)
{
  size_t idx = addr >> (RISCV_PGLEVEL_BITS*level + RISCV_PGSHIFT);
  return idx & ((1 << RISCV_PGLEVEL_BITS) - 1);
}

static pte_t* __attribute__((noinline)) __continue_walk_create(uintptr_t addr, pte_t* pte)
{
  *pte = ptd_create(ppn(__page_alloc()));
  return __walk_create(addr);
}

static pte_t* __attribute__((noinline)) __continue_walk_create_user(uintptr_t addr, pte_t* pte,pte_t * pagetable)
{
  *pte = ptd_create(ppn(__page_alloc()));
  return __walk_create_user(addr,pagetable);
}

//获得二级页表项
static pte_t* __walk_internal(uintptr_t addr, int create)
{
  pte_t* t = root_page_table;
  for (int i = (VA_BITS - RISCV_PGSHIFT) / RISCV_PGLEVEL_BITS - 1; i > 0; i--) {
    size_t idx = pt_idx(addr, i);
    if (unlikely(!(t[idx] & PTE_V)))
      return create ? __continue_walk_create(addr, &t[idx]) : 0;
    t = (pte_t*)(pte_ppn(t[idx]) << RISCV_PGSHIFT);
  }
  return &t[pt_idx(addr, 0)];
}

 pte_t* __walk(uintptr_t addr)
{
  return __walk_internal(addr, 0);
}

pte_t* __walk_create(uintptr_t addr)
{
  return __walk_internal(addr, 1);
}

pte_t* __walk_create_user(uintptr_t addr,pte_t * pagetable)
{
  return __walk_internal_user(pagetable,addr, 1);
}

static int __va_avail(uintptr_t vaddr)
{
  pte_t* pte = __walk(vaddr);
  return pte == 0 || *pte == 0;
}

static uintptr_t __vm_alloc(size_t npage)
{
  uintptr_t start = current.brk, end = current.mmap_max - npage*RISCV_PGSIZE;
  for (uintptr_t a = start; a <= end; a += RISCV_PGSIZE)
  {
    if (!__va_avail(a))
      continue;
    uintptr_t first = a, last = a + (npage-1) * RISCV_PGSIZE;
    for (a = last; a > first && __va_avail(a); a -= RISCV_PGSIZE)
      ;
    if (a > first)
      continue;
    return a;
  }
  return 0;
}

/*
页表项的标志位部分
page table entry (PTE) fields 
#define PTE_V     0x001 // Valid
#define PTE_R     0x002 // Read
#define PTE_W     0x004 // Write
#define PTE_X     0x008 // Execute
#define PTE_U     0x010 // User
#define PTE_G     0x020 // Global
#define PTE_A     0x040 // Accessed
#define PTE_D     0x080 // Dirty
#define PTE_SOFT  0x300 // Reserved for Software
*/
inline pte_t prot_to_type(int prot, int user)
{
  pte_t pte = 0;
  if (prot & PROT_READ) pte |= PTE_R | PTE_A;
  if (prot & PROT_WRITE) pte |= PTE_W | PTE_D;
  if (prot & PROT_EXEC) pte |= PTE_X | PTE_A;
  if (pte == 0) pte = PTE_R;
  if (user) pte |= PTE_U;
  return pte;
}

//用户地址是否越界（段错误警告）
int __valid_user_range(uintptr_t vaddr, size_t len)
{
  if (vaddr + len < vaddr)
    return 0;
  return vaddr + len <= current.mmap_max;
}



static int __handle_page_fault_user(uintptr_t vaddr, int prot,pte_t * pagetable)
{

 

  //you code here
  //get pte
 uintptr_t vpn = vaddr >> RISCV_PGSHIFT;
  vaddr = vpn << RISCV_PGSHIFT;
  pte_t* pte = __walk_create_user(vaddr,(pte_t*)pagetable);


  if (pte == 0 || *pte == 0 || !__valid_user_range(vaddr, 1))
    return -1;
  else if (!(*pte & PTE_V))
  {

    //you code here
   struct  Page *page=pmm_manager->alloc_pages(1);
   uintptr_t ppn =page2ppn(page);
   uintptr_t pa=page2pa(page);
  
    vmr_t* v = (vmr_t*)*pte;
    *pte = pte_create(ppn, prot_to_type(PROT_READ|PROT_WRITE, 0));
    flush_tlb();


    if (v->file)
    {

      size_t flen = MIN(RISCV_PGSIZE, v->length - (vaddr - v->addr));
      ssize_t ret = file_pread_pnn(v->file, (void*)vaddr, flen,ppn, vaddr - v->addr + v->offset);

      kassert(ret > 0);
      if (ret  < RISCV_PGSIZE)
        memset((void*)pa + ret, 0, RISCV_PGSIZE - ret);
    }
    else
      memset((int *)pa, 0, RISCV_PGSIZE);
  //  __vmr_decref(v, 1);
    
    *pte = pte_create(ppn, prot_to_type(v->prot, 1));
  }

  pte_t perms = pte_create(0, prot_to_type(prot, 1));
  if ((*pte & perms) != perms)
    return -1;

  flush_tlb();
  return 0;
}


static int __handle_page_fault(uintptr_t vaddr, int prot)
{
  if(vaddr<current.mmap_max){
    extern struct proc_struct *currentproc;
    return __handle_page_fault_user(vaddr,prot,(pte_t *)currentproc->upagetable);
  }

  //you code here
  //get pte
 uintptr_t vpn = vaddr >> RISCV_PGSHIFT;
  vaddr = vpn << RISCV_PGSHIFT;

  pte_t* pte = __walk(vaddr);

   printk("page fault vaddr:0x%l6x *pte %16lx\n", vaddr ,*pte);

  if (pte == 0 || *pte == 0 || !__valid_user_range(vaddr, 1))
    return -1;
  else if (!(*pte & PTE_V))
  {

    //you code here
   uintptr_t ppn =page2ppn(pmm_manager->alloc_pages(1));
  
    vmr_t* v = (vmr_t*)*pte;
    *pte = pte_create(ppn, prot_to_type(PROT_READ|PROT_WRITE, 0));
    flush_tlb();


    if (v->file)
    {
      size_t flen = MIN(RISCV_PGSIZE, v->length - (vaddr - v->addr));
      ssize_t ret = file_pread_pnn(v->file, (void*)vaddr, flen,ppn, vaddr - v->addr + v->offset);
          
      kassert(ret > 0);
      if (ret < RISCV_PGSIZE)
        memset((void*)vaddr + ret, 0, RISCV_PGSIZE - ret); 
    }
    else
      memset((void*)vaddr, 0, RISCV_PGSIZE);
    __vmr_decref(v, 1);
    *pte = pte_create(ppn, prot_to_type(v->prot, 1));
  }

  pte_t perms = pte_create(0, prot_to_type(prot, 1));
  if ((*pte & perms) != perms)
    return -1;

  flush_tlb();
  return 0;
}


int handle_page_fault(uintptr_t vaddr, int prot)
{
  spinlock_lock(&vm_lock);
    int ret = __handle_page_fault(vaddr, prot);
  spinlock_unlock(&vm_lock);
  return ret;
}



static void __do_munmap(uintptr_t addr, size_t len)
{
  for (uintptr_t a = addr; a < addr + len; a += RISCV_PGSIZE)
  {
    pte_t* pte = __walk(a);
    if (pte == 0 || *pte == 0)
      continue;

    if (!(*pte & PTE_V))
      __vmr_decref((vmr_t*)*pte, 1);

    *pte = 0;
  }
  flush_tlb(); // TODO: shootdown
}

uintptr_t __do_mmap(uintptr_t addr, size_t length, int prot, int flags, file_t* f, off_t offset)
{
  size_t npage = (length-1)/RISCV_PGSIZE+1;
  if (flags & MAP_FIXED)
  {
    if ((addr & (RISCV_PGSIZE-1)) || !__valid_user_range(addr, length))
      return (uintptr_t)-1;
  }
  else if ((addr = __vm_alloc(npage)) == 0)
    return (uintptr_t)-1;

  vmr_t* v = __vmr_alloc(addr, length, f, offset, npage, prot);
  if (!v)
    return (uintptr_t)-1;

  for (uintptr_t a = addr; a < addr + length; a += RISCV_PGSIZE)
  {
    pte_t* pte = __walk_create(a);
    kassert(pte);

    if (*pte)
      __do_munmap(a, RISCV_PGSIZE);

    *pte = (pte_t)v;
  }

  if (!demand_paging || (flags & MAP_POPULATE))
    for (uintptr_t a = addr; a < addr + length; a += RISCV_PGSIZE)
      kassert(__handle_page_fault(a, prot) == 0);

  return addr;
}

uintptr_t __do_mmap_user(uintptr_t addr, size_t length, int prot, int flags, file_t* f, off_t offset,pte_t * pagetable)
{
  size_t npage = (length-1)/RISCV_PGSIZE+1;
  if (flags & MAP_FIXED)
  {
    if ((addr & (RISCV_PGSIZE-1)) || !__valid_user_range(addr, length))
      return (uintptr_t)-1;
  }
  else if ((addr = __vm_alloc(npage)) == 0)
    return (uintptr_t)-1;

  vmr_t* v = __vmr_alloc(addr, length, f, offset, npage, prot);
  if (!v)
    return (uintptr_t)-1;

  for (uintptr_t a = addr; a < addr + length; a += RISCV_PGSIZE)
  {
    pte_t* pte = __walk_internal_user(pagetable,a,1);
    kassert(pte);

    if (*pte){
       panic("__do_mmap_user");
      //   __do_munmap(a, RISCV_PGSIZE);
    }
   

    *pte = (pte_t)v;
  }

  if (!demand_paging || (flags & MAP_POPULATE))
    for (uintptr_t a = addr; a < addr + length; a += RISCV_PGSIZE)
      kassert(__handle_page_fault_user(a, prot,pagetable) == 0);

  return addr;
}


int do_munmap(uintptr_t addr, size_t length)
{
  if ((addr & (RISCV_PGSIZE-1)) || !__valid_user_range(addr, length))
    return -EINVAL;

  spinlock_lock(&vm_lock);
    __do_munmap(addr, length);
  spinlock_unlock(&vm_lock);

  return 0;
}

uintptr_t do_mmap(uintptr_t addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  if (!(flags & MAP_PRIVATE) || length == 0 || (offset & (RISCV_PGSIZE-1)))
    return -EINVAL;

  file_t* f = NULL;
  if (!(flags & MAP_ANONYMOUS) && (f = file_get(fd)) == NULL)
    return -EBADF;

  spinlock_lock(&vm_lock);
    addr = __do_mmap(addr, length, prot, flags, f, offset);
    if (addr < current.brk_max)
      current.brk_max = addr;
  spinlock_unlock(&vm_lock);

  if (f) file_decref(f);
  return addr;
}

uintptr_t __do_brk(size_t addr)
{
  uintptr_t newbrk = addr;
  if (addr < current.brk_min)
    newbrk = current.brk_min;
  else if (addr > current.brk_max)
    newbrk = current.brk_max;

  if (current.brk == 0)
    current.brk = ROUNDUP(current.brk_min, RISCV_PGSIZE);

  uintptr_t newbrk_page = ROUNDUP(newbrk, RISCV_PGSIZE);
  if (current.brk > newbrk_page)
    __do_munmap(newbrk_page, current.brk - newbrk_page);
  else if (current.brk < newbrk_page)
    kassert(__do_mmap(current.brk, newbrk_page - current.brk, -1, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, 0, 0) == current.brk);
  current.brk = newbrk_page;

  return newbrk;
}

uintptr_t do_brk(size_t addr)
{
  spinlock_lock(&vm_lock);
    addr = __do_brk(addr);
  spinlock_unlock(&vm_lock);
  
  return addr;
}

uintptr_t do_mremap(uintptr_t addr, size_t old_size, size_t new_size, int flags)
{
  return -ENOSYS;
}

uintptr_t do_mprotect(uintptr_t addr, size_t length, int prot)
{
  uintptr_t res = 0;
  if ((addr) & (RISCV_PGSIZE-1))
    return -EINVAL;

  spinlock_lock(&vm_lock);
    for (uintptr_t a = addr; a < addr + length; a += RISCV_PGSIZE)
    {
      pte_t* pte = __walk(a);
      if (pte == 0 || *pte == 0) {
        res = -ENOMEM;
        break;
      }
  
      if (!(*pte & PTE_V)) {
        vmr_t* v = (vmr_t*)*pte;
        if((v->prot ^ prot) & ~v->prot){
          //TODO:look at file to find perms
          res = -EACCES;
          break;
        }
        v->prot = prot;
      } else {
        if (!(*pte & PTE_U) ||
            ((prot & PROT_READ) && !(*pte & PTE_R)) ||
            ((prot & PROT_WRITE) && !(*pte & PTE_W)) ||
            ((prot & PROT_EXEC) && !(*pte & PTE_X))) {
          //TODO:look at file to find perms
          res = -EACCES;
          break;
        }
        *pte = pte_create(pte_ppn(*pte), prot_to_type(prot, 1));
      }
    }
  spinlock_unlock(&vm_lock);

  flush_tlb();
  return res;
}

void __map_kernel_range(uintptr_t vaddr, uintptr_t paddr, size_t len, int prot)
{
 
  uintptr_t n = ROUNDUP(len, RISCV_PGSIZE) / RISCV_PGSIZE;
  uintptr_t offset = paddr - vaddr;
  for (uintptr_t a = vaddr, i = 0; i < n; i++, a += RISCV_PGSIZE)
  {
    pte_t* pte = __walk_create(a);
    kassert(pte);
    *pte = pte_create((a + offset) >> RISCV_PGSHIFT, prot_to_type(prot, 0));
  }
}

void __map_kernel_range_pgtbl(uintptr_t vaddr, uintptr_t paddr, size_t len, int prot,uintptr_t pagetable)
{
  uintptr_t n = ROUNDUP(len, RISCV_PGSIZE) / RISCV_PGSIZE;
  uintptr_t offset = paddr - vaddr;
  for (uintptr_t a = vaddr, i = 0; i < n; i++, a += RISCV_PGSIZE)
  {
    pte_t* pte = __walk_internal_user((pte_t *)pagetable,a,1);
    kassert(pte);
    *pte = pte_create((a + offset) >> RISCV_PGSHIFT, prot_to_type(prot, 0));
  }
}

void populate_mapping(const void* start, size_t size, int prot)
{
  uintptr_t a0 = ROUNDDOWN((uintptr_t)start, RISCV_PGSIZE);
  for (uintptr_t a = a0; a < (uintptr_t)start+size; a += RISCV_PGSIZE)
  {
    if (prot & PROT_WRITE)
      atomic_add((int*)a, 0);
    else
      atomic_read((int*)a);
  }
}

uintptr_t pk_vm_init()
{
  // HTIF address signedness and va2pa macro both cap memory size to 2 GiB
  mem_size = MIN(mem_size, 1U << 31);
  size_t mem_pages = mem_size >> RISCV_PGSHIFT;
  free_pages = MAX(8, mem_pages >> (RISCV_PGLEVEL_BITS-1));

  extern char _end;
  first_free_page = ROUNDUP((uintptr_t)&_end, RISCV_PGSIZE);
  first_free_paddr = first_free_page + free_pages * RISCV_PGSIZE;

  root_page_table = (void*)__page_alloc();
  __map_kernel_range(DRAM_BASE, DRAM_BASE, first_free_paddr - DRAM_BASE, PROT_READ|PROT_WRITE|PROT_EXEC);
 //add +++
  __map_kernel_range(first_free_paddr,first_free_paddr,(mem_pages-free_pages)*RISCV_PGSIZE,PROT_READ|PROT_WRITE|PROT_EXEC);
  current.mmap_max = current.brk_max =
    MIN(DRAM_BASE, mem_size - (first_free_paddr - DRAM_BASE));

  size_t stack_size = MIN(mem_pages >> 5, 2048) * RISCV_PGSIZE;
  size_t stack_bottom = __do_mmap(current.mmap_max - stack_size, stack_size, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0);
  kassert(stack_bottom != (uintptr_t)-1);
  current.stack_top = stack_bottom + stack_size;

  flush_tlb();
  write_csr(sptbr, ((uintptr_t)root_page_table >> RISCV_PGSHIFT) | SATP_MODE_CHOICE);
  *k_satp_p=(uint64_t)MAKE_SATP(root_page_table);
  uintptr_t kernel_stack_top = __page_alloc() + RISCV_PGSIZE;
 
  pmm_init();
  return kernel_stack_top;
}


pte_t *
__walk_internal_user(pte_t * pagetable, uint64_t addr, int create)
{

   pte_t* t = pagetable;

   for (int i = (VA_BITS - RISCV_PGSHIFT) / RISCV_PGLEVEL_BITS - 1; i > 0; i--) {
    size_t idx = pt_idx(addr, i);
    if (unlikely(!(t[idx] & PTE_V)))
      return create ? __continue_walk_create_user(addr, &t[idx],pagetable) : 0;
    t = (pte_t*)(pte_ppn(t[idx]) << RISCV_PGSHIFT);
  }
  return &t[pt_idx(addr, 0)];
}

int
mappages(pte_t* pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm)
{
  uint64_t a, last;
  pte_t *pte;

  a = ROUNDDOWN(va,RISCV_PGSIZE);
  last = ROUNDDOWN(va + size - 1,RISCV_PGSIZE);
  for(;;){
    if((pte = __walk_internal_user(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V){
       //printk("mappages remap %p  pte : %p\n ", a,*pte);
    }
  
       
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += RISCV_PGSIZE;
    pa += RISCV_PGSIZE;
  }
  return 0;
}


int
vmcopy(pte_t* old, pte_t* new)
{
  // panic("finish your code in vmcopy\n");
  // return 0;

  // 1、method-1
  //return uvmcopy(old, new);

  // 2、method-2
  pte_t *pte;
  pte_t *pte_new;
  uint64_t pa, i;
  int flags;
  uintptr_t mem;
  for(i = 0; i < current.mmap_max; i += RISCV_PGSIZE)
  {
    //case1 (this vaddr doesn't correspond pte)
    if((pte = __walk_internal_user(old, i, 0)) == 0)
      continue;
    //case2 (this vaddr correspond pte and PTE_V == 1)
    else if((*pte & PTE_V) == 1)
    {
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem =page2pa((void*)pmm_manager->alloc_pages(1))) == 0)
        goto err; 
      memcpy((char *)mem, (char*)pa, RISCV_PGSIZE);
      uintptr_t ppn =page2ppn(pa2page(mem));
      if(mappages(new, i, RISCV_PGSIZE, (uint64_t)mem, flags) != 0)
        goto err;
    }
    //case3 (this vaddr correspond pte and PTE_V == 0)
    else if((*pte & PTE_V) == 0)
    {
      pte_new = __walk_internal_user(new, i, 1);
      *pte_new = *pte;
    }
    //case4 (this vaddr corresponds user_Stack, copy Stack)
    else if(i == current.stack_top - RISCV_PGSIZE)
    {
      pa = PTE2PA(*pte);
      pte_new = __walk_internal_user(new, i, 1);
      uint64_t pa_new = PTE2PA(*pte_new);
      memcpy((char *)pa_new, (char*)pa, RISCV_PGSIZE);
    }
  }
  return 0;
err:
  return -1;
}

int
uvmcopy(pte_t* old, pte_t* new)
{
  pte_t *pte;
  pte_t *pte_new;
  uint64_t pa, i;
  int flags;
  uintptr_t mem;
  for(i = 0; i < current.mmap_max; i+=RISCV_PGSIZE){
    if((pte = __walk_internal_user(old, i, 0)) == 0)
       continue;
    if((*pte & PTE_V) == 0){
       pte_new=__walk_internal_user(new,i,1);
       *pte_new=*pte;
       continue;
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem =page2pa((void*)pmm_manager->alloc_pages(1))) == 0)
      goto err;
    
    memcpy((char *)mem, (char*)pa, RISCV_PGSIZE);
    uintptr_t ppn =page2ppn(pa2page(mem));
    if(mappages(new, i, RISCV_PGSIZE, (uint64_t)mem, flags) != 0){
      goto err;
    }
  }
  return 0;

err:
  return -1;
}

void vmprint_help(pte_t * pagetable,int levet, uint64_t * vpn){
      
      for(int i=0;i<512;i++){
        if(levet==0&&i>0){
          break;
        }
        vpn[levet]=i;
        pte_t pte=pagetable[i];
        if(pte==0)
            continue;
        //打印层次
           printk("  vm ");
        for(int j=0;j<=levet;j++){
          if(j!=levet)
            printk(".. ");
          else
            printk("..");
        }
        if(pte & PTE_V && (pte & (PTE_R|PTE_W|PTE_W))==0){
          //打印页目录
          printk(" %d: pte %p pa %p\n",i,(void *)pagetable[i],(void *)PTE2PA(pte));
          uint64_t child=PTE2PA(pte);
          vmprint_help((pte_t *)child,levet+1,vpn);
        }else
        {
          //打印叶子节点
            printk("va %p ",((vpn[0]<<18)|(vpn[1]<<9)|(vpn[2]))<<12);
            printk("%d: pte %p pa %p\n",i,(void *)pagetable[i],(void *)PTE2PA(pte));
        }
      
    }
}

void uvmprint_help(pte_t * pagetable,int levet, uint64_t * vpn){
      
      for(int i=0;i<512;i++){
        vpn[levet]=i;
        pte_t pte=pagetable[i];
        if(pte==0)
            continue;
        //打印层次
           printk("  vm ");
        for(int j=0;j<=levet;j++){
          if(j!=levet)
            printk(".. ");
          else
            printk("..");
        }
        if(pte & PTE_V && (pte & (PTE_R|PTE_W|PTE_W))==0){
          //打印页目录
          printk(" %d: pte %p pa %p\n",i,(void *)pagetable[i],(void *)PTE2PA(pte));
          uint64_t child=PTE2PA(pte);
          vmprint_help((pte_t *)child,levet+1,vpn);
        }else
        {
          //打印叶子节点
            printk("va %p ",((vpn[0]<<18)|(vpn[1]<<9)|(vpn[2]))<<12);
            printk("%d: pte %p pa %p\n",i,(void *)pagetable[i],(void *)PTE2PA(pte));
        }
      
    }
}

 void vmprint(pte_t *  pagetable) {
     uint64_t vpn[3];
    printk("  vm page table %p\n",(void *)pagetable);
    vmprint_help(pagetable,0,vpn);
    return;
 }

void uvmprint(pte_t *  pagetable) {
     uint64_t vpn[3];
    printk("  vm page table %p\n",(void *)pagetable);
    uvmprint_help(pagetable,0,vpn);
    return;
 }


 void printPage(intptr_t pageaddr,int vaddr){
   if(vaddr!=0){
     int64_t offset=vaddr|0x111;
     printk("page %p at offset %x value %p\n",pageaddr,offset,*(int64_t*)(pageaddr|offset));
     return;
   }

   for(int i=0;i<30;i++){
      int64_t offset=i*8;
      printk("page %p at offset %x value %p\n",pageaddr,offset,*(int64_t*)(pageaddr|offset));
   }
 }

 void printPlot(int prot){
    if (prot & PROT_READ) printk("PROT_READ ");
    if (prot & PROT_WRITE) printk("PROT_WRITE ");
    if (prot & PROT_EXEC) printk("PROT_EXEC ");
    printk("\n");
 }


// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64_t
walkaddr(pte_t * pagetable, uint64_t va)
{
  pte_t *pte;
  uint64_t pa;

  if(va >=  current.mmap_max)
    return 0;

  pte = __walk_internal_user(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pte_t * pagetable, char *dst, uint64_t srcva, uint64_t len)
{
  uint64_t n, va0, pa0;

  while(len > 0){
    va0 = ROUNDDOWN(srcva,RISCV_PGSIZE);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = RISCV_PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memcpy(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + RISCV_PGSIZE;
  }
  return 0;
}
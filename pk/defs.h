
#include <stdint.h>
#include "vm.h"
#include "elf.h"
#include "boot.h"
#include "file.h"

# if __riscv_xlen == 64
	typedef int64_t sint_t;
	typedef uint64_t uint_t;
#else
	typedef int32_t sint_t;
	typedef uint32_t uint_t;
#endif

typedef uint_t size_t;

typedef int bool;
/* to_struct - get the struct from a ptr
 * @ptr:    a struct pointer of member
 * @type:   the type of the struct this is embedded in
 * @member: the name of the member within the struct
 * */
#define to_struct(ptr, type, member)                               \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef size_t ppn_t;

//mmap.c


int vmcopy(pte_t* old, pte_t* new);
 void vmprint(pte_t *  pagetable) ;
pte_t * __walk_internal_user(pte_t * pagetable, uint64_t addr, int create);
int uvmcopy(pte_t* old, pte_t* new);
int mappages(pte_t* pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm);
void __map_kernel_range_pgtbl(uintptr_t vaddr, uintptr_t paddr, size_t len, int prot,uintptr_t pagetable);
pte_t* __walk_create_user(uintptr_t addr,pte_t * pagetable);
 void printPage(intptr_t pageaddr,int vaddr);
 void uvmprint(pte_t *  pagetable);
 void printPlot(int prot);
 int copyin(pte_t * pagetable, char *dst, uint64_t srcva, uint64_t len);

//defs.h
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)
#define pnn2pa(pnn,buf)   (pnn << RISCV_PGSHIFT)|((uintptr_t)buf & 0x111)
#define SATP_SV39 (8L << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t)pagetable) >> 12))
// #define __noinline __attribute__((noinline))


// shift a physical address to the right place for a PTE.
#define PA2PTE(pa) ((((uint64_t)pa) >> 12) << 10)


//proc.c
pte_t*  proc_pagetable();


//elf.c
void load_elf_user(const char* fn, elf_info* info,pte_t * pagetable);
uintptr_t __do_mmap_user(uintptr_t addr, size_t length, int prot, int flags, file_t* f, off_t offset,pte_t * pagetable);

//semap.c
struct semaphore;
struct semaphore*  alloc_sema(intptr_t va);
struct semaphore*  find_sema(intptr_t sema_va);
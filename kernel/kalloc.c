// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

// LRU list, swap bitmap 보호
struct spinlock swaplock;

// if bit is 1 -> swap slot 사용 중
static uchar *swap_bitmap;
static int swap_ready;

#define BLKS_PER_PG (PGSIZE / BSIZE)
#define NSWAP (SWAPMAX / BLKS_PER_PG)

static struct page *pa2page(uint64 pa);
static int page_on_lru(struct page *pg);
static void lru_remove_locked(struct page *pg);
static void lru_insert_tail_locked(struct page *pg);
static void lru_move_tail_locked(struct page *pg);
static struct page *select_victim_clock_locked(void);
static int swap_alloc_slot_locked(void);
static void swap_free_slot_locked(int slot);
static int swap_out(void);

static struct page *
pa2page(uint64 pa)
{
  return &pages[pa / PGSIZE];
}

static int
page_on_lru(struct page *pg)
{
  return pg->next != 0 && pg->prev != 0;
}

static void
lru_insert_tail_locked(struct page *pg)
{
  // newly mapped/swap-in -> swap 가능 page -> tail에 삽입한다.
  if (page_lru_head == 0)
  {
    page_lru_head = pg;
    pg->next = pg;
    pg->prev = pg;
  }
  else
  {
    struct page *tail = page_lru_head->prev;
    tail->next = pg;
    pg->prev = tail;
    pg->next = page_lru_head;
    page_lru_head->prev = pg;
  }

  num_lru_pages++;
}

static void
lru_remove_locked(struct page *pg)
{
  // unmap/swap-out -> resident pag 아님
  if (!page_on_lru(pg))
    return;

  if (pg->next == pg)
  {
    page_lru_head = 0;
  }
  else
  {
    pg->prev->next = pg->next;
    pg->next->prev = pg->prev;
    if (page_lru_head == pg)
      page_lru_head = pg->next;
  }

  pg->next = 0;
  pg->prev = 0;
  pg->pagetable = 0;
  pg->vaddr = 0;
  num_lru_pages--;
}

static void
lru_move_tail_locked(struct page *pg)
{
  // PTE_A가 켜진 page -> 2nd chance
  if (!page_on_lru(pg) || pg->next == pg)
    return;

  if (page_lru_head == pg)
    page_lru_head = pg->next;

  pg->prev->next = pg->next;
  pg->next->prev = pg->prev;

  struct page *tail = page_lru_head->prev;
  tail->next = pg;
  pg->prev = tail;
  pg->next = page_lru_head;
  page_lru_head->prev = pg;
}

void 
lru_add(pagetable_t pagetable, uint64 va, uint64 pa)
{
  struct page *pg = pa2page(pa);

  acquire(&swaplock);
  if (page_on_lru(pg))
    lru_remove_locked(pg);

  pg->pagetable = pagetable;
  pg->vaddr = (char *)PGROUNDDOWN(va);
  lru_insert_tail_locked(pg);
  release(&swaplock);
}

void 
lru_remove_pa(uint64 pa)
{
  struct page *pg = pa2page(pa);

  acquire(&swaplock);
  lru_remove_locked(pg);
  release(&swaplock);
}

static int
swap_alloc_slot_locked(void)
{
  // swap-out -> swap bitmap bit를 set
  for (int slot = 0; slot < NSWAP; slot++)
  {
    int byte = slot / 8;
    int bit = slot % 8;

    if ((swap_bitmap[byte] & (1 << bit)) == 0)
    {
      swap_bitmap[byte] |= (1 << bit);
      return slot;
    }
  }

  return -1;
}

static void
swap_free_slot_locked(int slot)
{
  // swapped page read/delete -> slot bit 0
  if (slot < 0 || slot >= NSWAP)
    panic("swap_free_slot");

  int byte = slot / 8;
  int bit = slot % 8;
  swap_bitmap[byte] &= ~(1 << bit);
}

void
swap_free_slot_by_pte(pte_t pte)
{
  int slot = PTE2PA(pte) / PGSIZE;

  acquire(&swaplock);
  swap_free_slot_locked(slot);
  release(&swaplock);
}

static struct page *
select_victim_clock_locked(void)
{
  // clock algorithm
  // QEMU가 세운 PTE_A가 reference bit
  if (page_lru_head == 0)
    return 0;

  while (1)
  {
    struct page *pg = page_lru_head;
    pte_t *pte = walk(pg->pagetable, (uint64)pg->vaddr, 0);

    if (pte == 0 || (*pte & PTE_V) == 0)
    {
      // LRU에는 resident user leaf page만 있어야 함
      lru_remove_locked(pg);
      if (page_lru_head == 0)
        return 0;
      continue;
    }

    if (*pte & PTE_A)
    {
      *pte &= ~PTE_A;
      sfence_vma(); // hw가 accessed bit 지워진 걸 보도록 TLB 비움
      lru_move_tail_locked(pg);
      continue;
    }

    return pg;
  }
}

static int
swap_out(void)
{
  struct page *victim;
  pte_t *pte;
  uint64 pa;
  uint flags;
  int slot;

  acquire(&swaplock);

  victim = select_victim_clock_locked();
  if (victim == 0)
  {
    release(&swaplock);
    return -1;
  }

  pte = walk(victim->pagetable, (uint64)victim->vaddr, 0);
  if (pte == 0 || (*pte & PTE_V) == 0)
  {
    release(&swaplock);
    return -1;
  }

  slot = swap_alloc_slot_locked();
  if (slot < 0)
  {
    release(&swaplock);
    return -1;
  }

  pa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);

  // 해당 page는 다음에서 memory resident 상태 x -> LRU에서 먼저 제거
  lru_remove_locked(victim);
  release(&swaplock);

  // disk I/O는 lock 잡지 않고 수행
  // write 끝나고 PPN field에 swap slot 번호 저장
  swapwrite(pa, slot);

  acquire(&swaplock);
  // slot * PGSIZE 저장
  flags &= ~(PTE_V | PTE_A);
  flags |= PTE_SWAP;
  *pte = PA2PTE((uint64)slot * PGSIZE) | flags;
  sfence_vma();
  release(&swaplock);

  kfree((void *)pa);
  return 0;
}

int 
swap_in(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint flags;
  uint64 mem;
  int slot;

  va = PGROUNDDOWN(va);
  if (va >= MAXVA)
    return -1;

  pte = walk(pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_SWAP) == 0 || (*pte & PTE_V))
    return -1;

  slot = PTE2PA(*pte) / PGSIZE;
  flags = PTE_FLAGS(*pte);

  mem = (uint64)kalloc();
  if (mem == 0)
    return -1;

  // PTE 이미 존재 -> PPN field만 바꿈
  swapread(mem, slot);

  acquire(&swaplock);
  swap_free_slot_locked(slot);
  flags &= ~PTE_SWAP;
  flags |= PTE_V;
  *pte = PA2PTE(mem) | flags;
  sfence_vma();
  release(&swaplock);

  lru_add(pagetable, va, mem);
  return 0;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&swaplock, "swap");
  freerange(end, (void *)PHYSTOP);

  // swap bitmap용 physical page(kernel page) 예약 -> not in user LRU list
  swap_bitmap = (uchar *)kalloc();
  if (swap_bitmap == 0)
    panic("kinit: swap bitmap");
  memset(swap_bitmap, 0, PGSIZE);
  swap_ready = 1;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  num_free_pages++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;
    num_free_pages--;
  }
  release(&kmem.lock);

  // free physical page X -> swap out
  if (r == 0 && swap_ready && mycpu()->noff == 0)
  {
    if (swap_out() == 0)
    {
      acquire(&kmem.lock);
      r = kmem.freelist;
      if (r)
      {
        kmem.freelist = r->next;
        num_free_pages--;
      }
      release(&kmem.lock);
    }
  }

  if (r)
    memset((char *)r, 5, PGSIZE);
  else if (swap_ready)
    printf("kalloc: out of memory\n");

  return (void *)r;
}
/*
 * OrgitoOS - Virtual Memory Manager (VMM) Implementation
 */

#include "vmm.h"
#include "pmm.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * Kernel image boundaries from the linker script (linker.ld). Addresses,
 * not variables — only their &-address is meaningful. [kernel_start,
 * rodata_start) is the kernel's .text: the only slice of the identity map
 * that stays user-executable (see vmm_init()).
 */
extern uint8_t kernel_start[];
extern uint8_t rodata_start[];

/* Current page directory */
static struct page_directory *current_directory = 0;

/* Kernel page directory */
static struct page_directory *kernel_directory = 0;

/* Helper macros for page directory/table indexing */
#define PD_INDEX(addr) (((uint32_t)(addr) >> 22) & 0x3FF)
#define PT_INDEX(addr) (((uint32_t)(addr) >> 12) & 0x3FF)
#define PAGE_ALIGN(addr) ((uint32_t)(addr) & 0xFFFFF000)

/* TLB flush for a single page */
static inline void tlb_flush_page(void *virt) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

/* TLB flush for entire address space */
static inline void tlb_flush_all(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3));
}

/*
 * Get or create a page table for a virtual address
 * NOTE: This function assumes physical memory is identity-mapped (virt == phys)
 *       which is true during Phase 0. For higher-half kernel, this will need updating.
 */
static struct page_table *get_page_table(struct page_directory *dir, void *virt, bool create) {
    uint32_t pd_index = PD_INDEX(virt);
    
    /* Check if page table exists */
    if (dir->tables[pd_index] != NULL) {
        return dir->tables[pd_index];
    }
    
    /* Create new page table if requested */
    if (create) {
        /* Allocate physical page for page table */
        void *phys = pmm_alloc_page();
        if (phys == NULL) {
            return NULL;
        }
        
        /* Clear the page table */
        struct page_table *pt = (struct page_table *)phys;
        for (uint32_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
            pt->entries[i] = 0;
        }
        
        /* Store page table pointer */
        dir->tables[pd_index] = pt;
        
        /* Set page directory entry.
         *
         * PTE_USER is set at the directory level so that individual
         * page-table entries decide user accessibility; without it the
         * supervisor bit in the PDE would override user PTEs and ring 3
         * could never execute (Phase 1 user-mode support). */
        dir->entries[pd_index] = ((uint32_t)phys & 0xFFFFF000) | 
                                  PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        
        return pt;
    }
    
    return NULL;
}

/*
 * Initialize the Virtual Memory Manager
 * NOTE: This assumes physical memory is identity-mapped during initialization.
 *       The first 4MB is identity-mapped to cover kernel code/data and VGA buffer.
 */
void vmm_init(void) {
    /*
     * Allocate the kernel page directory.
     *
     * NOTE: struct page_directory is larger than one page (it carries
     * both the 1024 hardware entries AND 1024 software table pointers),
     * so a single pmm_alloc_page() is NOT enough — allocating only one
     * page lets the directory's tables[]/entries[] writes overflow into
     * the next physical page, corrupting the page tables allocated
     * immediately afterward. Reserve enough contiguous pages for the
     * whole structure.
     *
     * The CPU only reads the first 4 KB (entries[]), which is page
     * aligned at the start of the allocation, so CR3 stays valid.
     */
    uint32_t dir_pages = (sizeof(struct page_directory) + PAGE_SIZE - 1) / PAGE_SIZE;

    void *dir_phys = pmm_alloc_pages(dir_pages);
    if (dir_phys == NULL) {
        /* No contiguous run available: leave paging disabled rather
         * than corrupt RAM. */
        return;
    }

    kernel_directory = (struct page_directory *)dir_phys;
    
    /* Clear page directory */
    for (uint32_t i = 0; i < PAGE_DIR_ENTRIES; i++) {
        kernel_directory->entries[i] = 0;
        kernel_directory->tables[i] = NULL;
    }
    
    /*
     * Identity-map ALL physical RAM, not just the first 4 MB.
     *
     * The PMM hands out free pages from anywhere in physical memory
     * (typically well above 4 MB on a normal boot). If only the first
     * 4 MB were mapped, the first allocation above that boundary would
     * fault the moment paging is active. Mapping the full RAM range up
     * front keeps virt == phys for every page the PMM can return,
     * including the page tables allocated below.
     *
     * A minimum of 4 MB is always mapped so kernel code/data and the
     * VGA buffer are covered even if the memory map reports less.
     */
    /*
     * Identity-map physical RAM so the PMM's allocations are reachable
     * once paging is on. We map all detected RAM, but cap the initial
     * map at 64 MiB: that comfortably covers kernel code/data, the early
     * page tables, and the 4 MiB kernel heap, while keeping the number
     * of page tables allocated here bounded and contiguous-friendly.
     * Higher regions can be mapped on demand later if needed.
     */
    uint32_t ram_end = pmm_get_max_address();
    if (ram_end < 0x400000) {
        ram_end = 0x400000;          /* floor: 4 MiB  */
    }
    if (ram_end > 0x4000000) {
        ram_end = 0x4000000;         /* cap: 64 MiB   */
    }
    /*
     * Kernel/user memory protection: the identity map is supervisor-only
     * by default (no PTE_USER), which covers the kernel heap, the PMM
     * bitmap, the process table (PCBs), and every process's kernel stack
     * (kmalloc'd from this same region) — none of it is reachable from
     * ring 3 anymore.
     *
     * The one carve-out is [kernel_start, rodata_start): the kernel's own
     * .text. OrgitoOS has no exec() yet (see docs/roadmap/roadmap.md), so
     * ring 3 "processes" are kernel-linked C functions
     * (kernel/user_programs.c) entered via enter_user_mode() — they must
     * be able to fetch their own code. They already talk to the kernel
     * exclusively through int 0x80 syscalls, never by touching
     * .rodata/.data/.bss directly, so nothing past rodata_start needs to
     * stay user-accessible.
     */
    vmm_identity_map_region(kernel_directory, 0, ram_end,
                           PTE_PRESENT | PTE_WRITABLE);

    /* The kernel reaches physical pages through this identity map (page
     * tables, a new process's pages while it is being built, fork()
     * copies). RAM above the map's cap would be unreachable once paging
     * is on, so hide it from the PMM instead of handing it out. */
    for (uint32_t addr = ram_end; addr < pmm_get_max_address(); addr += PAGE_SIZE) {
        pmm_mark_used((void *)addr);
    }
    vmm_identity_map_region(kernel_directory, kernel_start,
                           (uint32_t)rodata_start - (uint32_t)kernel_start,
                           PTE_PRESENT | PTE_WRITABLE | PTE_USER);

    /* Set as current directory */
    current_directory = kernel_directory;
    
    /* Load page directory into CR3 */
    vmm_switch_directory(kernel_directory);

    /* Turn paging on (CR0.PG). Without this, CR3 is loaded but ignored:
     * every address is used as a raw physical address, so per-process
     * mappings such as the user stack at PROCESS_USTACK_VIRT_BASE
     * (0x08000000) silently hit non-existent RAM and ring 3 code reads
     * garbage back from its own stack. CR0.WP makes the CPU honour
     * read-only pages in ring 0 too. */
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u | 0x00010000u;   /* PG | WP */
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/*
 * Create a new page directory
 */
struct page_directory *vmm_create_directory(void) {
    /* Same sizing requirement as vmm_init: the directory spans multiple
     * pages, so reserve a contiguous run rather than a single page. */
    uint32_t dir_pages = (sizeof(struct page_directory) + PAGE_SIZE - 1) / PAGE_SIZE;

    void *dir_phys = pmm_alloc_pages(dir_pages);
    if (dir_phys == NULL) {
        return NULL;
    }

    struct page_directory *dir = (struct page_directory *)dir_phys;

    /* Clear page directory */
    for (uint32_t i = 0; i < PAGE_DIR_ENTRIES; i++) {
        dir->entries[i] = 0;
        dir->tables[i] = NULL;
    }

    return dir;
}

/*
 * Shallow-clone the kernel directory's mappings into `dir`: PDE slots the
 * kernel directory has already populated (identity-mapped RAM, containing
 * kernel code/data and the shared heap) are copied by pointer, so `dir` sees
 * the exact same page tables as the kernel - no duplication, no divergence.
 * PDE slots the kernel directory has never touched (e.g. a high virtual
 * window reserved for a process's private user stack) are left NULL, so a
 * later vmm_map_page() on `dir` allocates a fresh table exclusive to `dir`.
 */
void vmm_clone_kernel_mappings(struct page_directory *dir) {
    if (dir == NULL || kernel_directory == NULL) {
        return;
    }
    for (uint32_t i = 0; i < PAGE_DIR_ENTRIES; i++) {
        dir->entries[i] = kernel_directory->entries[i];
        dir->tables[i]  = kernel_directory->tables[i];
    }
}

struct page_directory *vmm_kernel_directory(void) {
    return kernel_directory;
}

/*
 * Destroy a page directory
 */
void vmm_destroy_directory(struct page_directory *dir) {
    if (dir == NULL || dir == kernel_directory) {
        return;
    }

    /* Free only page tables private to `dir`. A cloned directory shares most
     * of its tables by pointer with kernel_directory (see
     * vmm_clone_kernel_mappings); freeing those here would pull the rug out
     * from under the kernel and every other process still using them. Only
     * tables that differ from the kernel directory's slot are exclusively
     * owned by `dir` and safe to reclaim. */
    for (uint32_t i = 0; i < PAGE_DIR_ENTRIES; i++) {
        if (dir->tables[i] != NULL &&
            (kernel_directory == NULL || dir->tables[i] != kernel_directory->tables[i])) {
            pmm_free_page(dir->tables[i]);
        }
    }

    /* Free the directory itself (it spans several contiguous pages) */
    pmm_free_pages(dir, (sizeof(struct page_directory) + PAGE_SIZE - 1) / PAGE_SIZE);
}

/*
 * Switch to a different page directory
 */
void vmm_switch_directory(struct page_directory *dir) {
    if (!dir) return;
    
    current_directory = dir;
    
    /* Load the page directory into CR3 */
    uint32_t phys_addr = (uint32_t)dir;
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys_addr));
}

/*
 * Map a virtual page to a physical frame
 */
int vmm_map_page(struct page_directory *dir, void *virt, uint32_t phys, uint32_t flags) {
    if (dir == NULL) {
        dir = current_directory;
    }
    
    /* Get or create page table */
    struct page_table *pt = get_page_table(dir, virt, true);
    if (pt == NULL) {
        return 0;
    }
    
    /* Get page table entry */
    uint32_t pt_index = PT_INDEX(virt);
    
    /* Map the page */
    pt->entries[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF);
    
    /* Flush TLB for this page */
    tlb_flush_page(virt);
    
    return 1;
}

/*
 * Unmap a virtual page
 */
void vmm_unmap_page(struct page_directory *dir, void *virt) {
    if (dir == NULL) {
        dir = current_directory;
    }
    
    /* Get page table */
    struct page_table *pt = get_page_table(dir, virt, false);
    if (pt == NULL) {
        return;
    }
    
    /* Get page table entry */
    uint32_t pt_index = PT_INDEX(virt);
    
    /* Clear the page table entry */
    pt->entries[pt_index] = 0;
    
    /* Flush TLB for this page */
    tlb_flush_page(virt);
}

/*
 * Get physical address for a virtual address
 */
uint32_t vmm_get_physical(struct page_directory *dir, void *virt) {
    if (dir == NULL) {
        dir = current_directory;
    }
    
    /* Get page table */
    struct page_table *pt = get_page_table(dir, virt, false);
    if (pt == NULL) {
        return 0;
    }
    
    /* Get page table entry */
    uint32_t pt_index = PT_INDEX(virt);
    uint32_t pte = pt->entries[pt_index];
    
    /* Check if page is present */
    if (!(pte & PTE_PRESENT)) {
        return 0;
    }
    
    /* Return physical address with offset */
    return (pte & 0xFFFFF000) | ((uint32_t)virt & 0xFFF);
}

/*
 * Identity map a region (virtual address == physical address)
 */
void vmm_identity_map_region(struct page_directory *dir, void *start, size_t size, uint32_t flags) {
    if (dir == NULL) {
        dir = current_directory;
    }
    
    /* Align start address down to page boundary */
    uint32_t virt = PAGE_ALIGN((uint32_t)start);
    uint32_t end = ((uint32_t)start + size + PAGE_SIZE - 1) & 0xFFFFF000;
    
    /* Map each page in the region */
    while (virt < end) {
        vmm_map_page(dir, (void *)virt, virt, flags);
        virt += PAGE_SIZE;
    }
}

/*
 * Map a region of memory
 */
void vmm_map_region(struct page_directory *dir, void *virt, uint32_t phys, size_t size, uint32_t flags) {
    if (dir == NULL) {
        dir = current_directory;
    }
    
    /* Align addresses to page boundaries */
    uint32_t virt_addr = PAGE_ALIGN((uint32_t)virt);
    uint32_t phys_addr = PAGE_ALIGN(phys);
    uint32_t end = ((uint32_t)virt + size + PAGE_SIZE - 1) & 0xFFFFF000;
    
    /* Map each page in the region */
    while (virt_addr < end) {
        vmm_map_page(dir, (void *)virt_addr, phys_addr, flags);
        virt_addr += PAGE_SIZE;
        phys_addr += PAGE_SIZE;
    }
}

/*
 * Page fault handler
 */
void vmm_page_fault_handler(void) {
    /* Get the faulting address from CR2 */
    uint32_t faulting_address;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(faulting_address));
    
    /* This is handled by the exception system now */
    /* The page fault exception (14) will be caught by exception_handler */
    (void)faulting_address;
}

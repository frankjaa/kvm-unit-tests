/*
 * MMU enable and page table manipulation functions
 *
 * Copyright (C) 2014, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <asm/setup.h>
#include <asm/thread_info.h>
#include <asm/cpumask.h>
#include <asm/mmu.h>
#include <asm/setup.h>
#include <asm/page.h>

#include "alloc_page.h"
#include "vmalloc.h"
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable.h>

#include <linux/compiler.h>

extern unsigned long etext;

pgd_t *mmu_idmap;

/* CPU 0 starts with disabled MMU */
static cpumask_t mmu_disabled_cpumask = { {1} };
unsigned int mmu_disabled_cpu_count = 1;

bool __mmu_enabled(void)
{
	int cpu = current_thread_info()->cpu;

	/*
	 * mmu_enabled is called from places that are guarding the
	 * use of exclusive ops (which require the mmu to be enabled).
	 * That means we CANNOT call anything from here that may use a
	 * spinlock, atomic bitop, etc., otherwise we'll recurse.
	 * [cpumask_]test_bit is safe though.
	 */
	return !cpumask_test_cpu(cpu, &mmu_disabled_cpumask);
}

void mmu_mark_enabled(int cpu)
{
	if (cpumask_test_and_clear_cpu(cpu, &mmu_disabled_cpumask))
		--mmu_disabled_cpu_count;
}

void mmu_mark_disabled(int cpu)
{
	if (!cpumask_test_and_set_cpu(cpu, &mmu_disabled_cpumask))
		++mmu_disabled_cpu_count;
}

extern void asm_mmu_enable(phys_addr_t pgtable);
void mmu_enable(pgd_t *pgtable)
{
	struct thread_info *info = current_thread_info();

	asm_mmu_enable(__pa(pgtable));

	info->pgtable = pgtable;
	mmu_mark_enabled(info->cpu);
}

extern void asm_mmu_disable(void);
void mmu_disable(void)
{
	unsigned long sp = current_stack_pointer;
	int cpu = current_thread_info()->cpu;

	assert_msg(__virt_to_phys(sp) == sp,
			"Attempting to disable MMU with non-identity mapped stack");

	mmu_mark_disabled(cpu);

	asm_mmu_disable();
}

static pteval_t *get_pte(pgd_t *pgtable, uintptr_t vaddr)
{
	pgd_t *pgd = pgd_offset(pgtable, vaddr);
	pmd_t *pmd = pmd_alloc(pgd, vaddr);
	pte_t *pte = pte_alloc(pmd, vaddr);

	return &pte_val(*pte);
}

static pteval_t *install_pte(pgd_t *pgtable, uintptr_t vaddr, pteval_t pte)
{
	pteval_t *p_pte = get_pte(pgtable, vaddr);

	WRITE_ONCE(*p_pte, pte);
	flush_tlb_page(vaddr);
	return p_pte;
}

static pteval_t *install_page_prot(pgd_t *pgtable, phys_addr_t phys,
				   uintptr_t vaddr, pgprot_t prot)
{
	pteval_t pte = phys;
	pte |= PTE_TYPE_PAGE | PTE_AF | PTE_SHARED;
	pte |= pgprot_val(prot);
	return install_pte(pgtable, vaddr, pte);
}

pteval_t *install_page(pgd_t *pgtable, phys_addr_t phys, void *virt)
{
	return install_page_prot(pgtable, phys, (uintptr_t)virt,
				 __pgprot(PTE_WBWA | PTE_USER));
}

phys_addr_t virt_to_pte_phys(pgd_t *pgtable, void *mem)
{
	return (*get_pte(pgtable, (uintptr_t)mem) & PHYS_MASK & -PAGE_SIZE)
		+ ((ulong)mem & (PAGE_SIZE - 1));
}

void mmu_set_range_ptes(pgd_t *pgtable, uintptr_t virt_offset,
			phys_addr_t phys_start, phys_addr_t phys_end,
			pgprot_t prot)
{
	phys_addr_t paddr = phys_start & PAGE_MASK;
	uintptr_t vaddr = virt_offset & PAGE_MASK;
	uintptr_t virt_end = phys_end - paddr + vaddr;

	for (; vaddr < virt_end; vaddr += PAGE_SIZE, paddr += PAGE_SIZE)
		install_page_prot(pgtable, paddr, vaddr, prot);
}

void mmu_set_range_sect(pgd_t *pgtable, uintptr_t virt_offset,
			phys_addr_t phys_start, phys_addr_t phys_end,
			pgprot_t prot)
{
	phys_addr_t paddr = phys_start & PGDIR_MASK;
	uintptr_t vaddr = virt_offset & PGDIR_MASK;
	uintptr_t virt_end = phys_end - paddr + vaddr;
	pgd_t *pgd;
	pgd_t entry;

	for (; vaddr < virt_end; vaddr += PGDIR_SIZE, paddr += PGDIR_SIZE) {
		pgd_val(entry) = paddr;
		pgd_val(entry) |= PMD_TYPE_SECT | PMD_SECT_AF | PMD_SECT_S;
		pgd_val(entry) |= pgprot_val(prot);
		pgd = pgd_offset(pgtable, vaddr);
		WRITE_ONCE(*pgd, entry);
		flush_tlb_page(vaddr);
	}
}

void *setup_mmu(phys_addr_t phys_end)
{
	uintptr_t code_end = (uintptr_t)&etext;
	struct mem_region *r;

	/* 0G-1G = I/O, 1G-3G = identity, 3G-4G = vmalloc */
	if (phys_end > (3ul << 30))
		phys_end = 3ul << 30;

#ifdef __aarch64__
	init_alloc_vpage((void*)(4ul << 30));
#endif

	mmu_idmap = alloc_page();

	for (r = mem_regions; r->end; ++r) {
		if (!(r->flags & MR_F_IO))
			continue;
		mmu_set_range_sect(mmu_idmap, r->start, r->start, r->end,
				   __pgprot(PMD_SECT_UNCACHED | PMD_SECT_USER));
	}

	/* armv8 requires code shared between EL1 and EL0 to be read-only */
	mmu_set_range_ptes(mmu_idmap, PHYS_OFFSET,
		PHYS_OFFSET, code_end,
		__pgprot(PTE_WBWA | PTE_RDONLY | PTE_USER));

	mmu_set_range_ptes(mmu_idmap, code_end,
		code_end, phys_end,
		__pgprot(PTE_WBWA | PTE_USER));

	mmu_enable(mmu_idmap);
	return mmu_idmap;
}

phys_addr_t __virt_to_phys(unsigned long addr)
{
	if (mmu_enabled()) {
		pgd_t *pgtable = current_thread_info()->pgtable;
		return virt_to_pte_phys(pgtable, (void *)addr);
	}
	return addr;
}

unsigned long __phys_to_virt(phys_addr_t addr)
{
	/*
	 * We don't guarantee that phys_to_virt(virt_to_phys(vaddr)) == vaddr, but
	 * the default page tables do identity map all physical addresses, which
	 * means phys_to_virt(virt_to_phys((void *)paddr)) == paddr.
	 */
	assert(!mmu_enabled() || __virt_to_phys(addr) == addr);
	return addr;
}

void mmu_clear_user(pgd_t *pgtable, unsigned long vaddr)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;

	if (!mmu_enabled())
		return;

	pgd = pgd_offset(pgtable, vaddr);
	assert(pgd_valid(*pgd));
	pmd = pmd_offset(pgd, vaddr);
	assert(pmd_valid(*pmd));

	if (pmd_huge(*pmd)) {
		pmd_t entry = __pmd(pmd_val(*pmd) & ~PMD_SECT_USER);
		WRITE_ONCE(*pmd, entry);
		goto out_flush_tlb;
	}

	pte = pte_offset(pmd, vaddr);
	assert(pte_valid(*pte));
	pte_t entry = __pte(pte_val(*pte) & ~PTE_USER);
	WRITE_ONCE(*pte, entry);

out_flush_tlb:
	flush_tlb_page(vaddr);
}

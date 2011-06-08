#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/modctl.h>
#include <sys/open.h>
#include <sys/kmem.h>
#include <sys/poll.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/atomic.h>
#include <sys/spl.h>
#include <sys/cpuvar.h>
#include <sys/segments.h>

#include "kvm_bitops.h"
#include "msr.h"
#include "kvm_vmx.h"
#include "irqflags.h"
#include "kvm_iodev.h"
#include "kvm_x86impl.h"
#include "kvm_host.h"
#include "kvm_x86host.h"
#include "kvm.h"

extern int lwp_sigmask(int, uint_t, uint_t, uint_t, uint_t);

unsigned long
kvm_dirty_bitmap_bytes(struct kvm_memory_slot *memslot)
{
	return (BT_SIZEOFMAP(memslot->npages));
}

struct kvm_vcpu *
kvm_get_vcpu(struct kvm *kvm, int i)
{
#ifdef XXX
	smp_rmb();
#endif
	return (kvm->vcpus[i]);
}

void
kvm_fx_save(struct i387_fxsave_struct *image)
{
	__asm__("fxsave (%0)":: "r" (image));
}

void
kvm_fx_restore(struct i387_fxsave_struct *image)
{
	__asm__("fxrstor (%0)":: "r" (image));
}

void
kvm_fx_finit(void)
{
	__asm__("finit");
}

uint32_t
get_rdx_init_val(void)
{
	return (0x600); /* P6 family */
}

void
kvm_inject_gp(struct kvm_vcpu *vcpu, uint32_t error_code)
{
	kvm_queue_exception_e(vcpu, GP_VECTOR, error_code);
}

void
kvm_sigprocmask(int how, sigset_t *setp, sigset_t *osetp)
{
	k_sigset_t kset;

	ASSERT(how == SIG_SETMASK);
	ASSERT(setp != NULL);

	sigutok(setp, &kset);

	if (osetp != NULL)
		sigktou(&curthread->t_hold, osetp);

	(void) lwp_sigmask(SIG_SETMASK,
	    kset.__sigbits[0], kset.__sigbits[1], kset.__sigbits[2], 0);
}

unsigned long long
native_read_tscp(unsigned int *aux)
{
	unsigned long low, high;
	__asm__ volatile(".byte 0x0f,0x01,0xf9"
		: "=a" (low), "=d" (high), "=c" (*aux));
	return (low | ((uint64_t)high << 32));
}

unsigned long long
native_read_msr(unsigned int msr)
{
	DECLARE_ARGS(val, low, high);

	__asm__ volatile("rdmsr" : EAX_EDX_RET(val, low, high) : "c" (msr));
	return (EAX_EDX_VAL(val, low, high));
}

void
native_write_msr(unsigned int msr, unsigned low, unsigned high)
{
	__asm__ volatile("wrmsr" : : "c" (msr),
	    "a"(low), "d" (high) : "memory");
}

unsigned long long
__native_read_tsc(void)
{
	DECLARE_ARGS(val, low, high);

	__asm__ volatile("rdtsc" : EAX_EDX_RET(val, low, high));

	return (EAX_EDX_VAL(val, low, high));
}

unsigned long long
native_read_pmc(int counter)
{
	DECLARE_ARGS(val, low, high);

	__asm__ volatile("rdpmc" : EAX_EDX_RET(val, low, high) : "c" (counter));
	return (EAX_EDX_VAL(val, low, high));
}

int
wrmsr_safe(unsigned msr, unsigned low, unsigned high)
{
	return (native_write_msr_safe(msr, low, high));
}

int
rdmsrl_safe(unsigned msr, unsigned long long *p)
{
	int err;

	*p = native_read_msr_safe(msr, &err);
	return (err);
}

unsigned long
read_msr(unsigned long msr)
{
	uint64_t value;

	rdmsrl(msr, value);
	return (value);
}

unsigned long
kvm_read_tr_base(void)
{
	unsigned short tr;
	__asm__("str %0" : "=g"(tr));
	return (segment_base(tr));
}

int
kvm_xcall_func(kvm_xcall_t func, void *arg)
{
	if (func != NULL)
		(*func)(arg);

	return (0);
}

void
kvm_xcall(processorid_t cpu, kvm_xcall_t func, void *arg)
{
	cpuset_t set;

	CPUSET_ZERO(set);

	if (cpu == KVM_CPUALL) {
		CPUSET_ALL(set);
	} else {
		CPUSET_ADD(set, cpu);
	}

	kpreempt_disable();
	xc_sync((xc_arg_t)func, (xc_arg_t)arg, 0, CPUSET2BV(set),
		(xc_func_t) kvm_xcall_func);
	kpreempt_enable();
}



unsigned short
kvm_read_fs(void)
{
	unsigned short seg;
	__asm__("mov %%fs, %0" : "=g"(seg));
	return (seg);
}

unsigned short
kvm_read_gs(void)
{
	unsigned short seg;
	__asm__("mov %%gs, %0" : "=g"(seg));
	return (seg);
}

unsigned short
kvm_read_ldt(void)
{
	unsigned short ldt;
	__asm__("sldt %0" : "=g"(ldt));
	return (ldt);
}

void
kvm_load_fs(unsigned short sel)
{
	__asm__("mov %0, %%fs" : : "rm"(sel));
}

void
kvm_load_gs(unsigned short sel)
{
	__asm__("mov %0, %%gs" : : "rm"(sel));
}

void
kvm_load_ldt(unsigned short sel)
{
	__asm__("lldt %0" : : "rm"(sel));
}


void
kvm_get_idt(struct descriptor_table *table)
{
	__asm__("sidt %0" : "=m"(*table));
}

void
kvm_get_gdt(struct descriptor_table *table)
{
	__asm__("sgdt %0" : "=m"(*table));
}

/*
 * Find the first cleared bit in a memory region.
 */
unsigned long
find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	const unsigned long *p = addr;
	unsigned long result = 0;
	unsigned long tmp;

	while (size & ~(64-1)) {
		if (~(tmp = *(p++)))
			goto found;
		result += 64;
		size -= 64;
	}
	if (!size)
		return (result);

	tmp = (*p) | (~0UL << size);
	if (tmp == ~0UL)	/* Are any bits zero? */
		return (result + size);	/* Nope. */
found:
	return (result + ffz(tmp));
}

int
zero_constructor(void *buf, void *arg, int tags)
{
	bzero(buf, (size_t)arg);
	return (0);
}

/*
 * Volatile isn't enough to prevent the compiler from reordering the
 * read/write functions for the control registers and messing everything up.
 * A memory clobber would solve the problem, but would prevent reordering of
 * all loads stores around it, which can hurt performance. Solution is to
 * use a variable and mimic reads and writes to it to enforce serialization
 */
static unsigned long __force_order;

unsigned long
native_read_cr0(void)
{
	unsigned long val;
	__asm__ volatile("mov %%cr0,%0\n\t" : "=r" (val), "=m" (__force_order));
	return (val);
}

unsigned long
native_read_cr4(void)
{
	unsigned long val;
	__asm__ volatile("mov %%cr4,%0\n\t" : "=r" (val), "=m" (__force_order));
	return (val);
}

unsigned long
native_read_cr3(void)
{
	unsigned long val;
	__asm__ volatile("mov %%cr3,%0\n\t" : "=r" (val), "=m" (__force_order));
	return (val);
}

inline unsigned long
get_desc_limit(const struct desc_struct *desc)
{
	return (desc->c.b.limit0 | (desc->c.b.limit << 16));
}

unsigned long
get_desc_base(const struct desc_struct *desc)
{
	return (unsigned)(desc->c.b.base0 | ((desc->c.b.base1) << 16) |
	    ((desc->c.b.base2) << 24));
}

inline page_t *
compound_head(page_t *page)
{
	/* XXX - linux links page_t together. */
	return (page);
}

inline void
get_page(page_t *page)
{
	page = compound_head(page);
}


page_t *
pfn_to_page(pfn_t pfn)
{
	return (page_numtopp_nolock(pfn));
}


inline void
kvm_clear_exception_queue(struct kvm_vcpu *vcpu)
{
	vcpu->arch.exception.pending = 0;
}

inline void
kvm_queue_interrupt(struct kvm_vcpu *vcpu, uint8_t vector, int soft)
{
	vcpu->arch.interrupt.pending = 1;
	vcpu->arch.interrupt.soft = soft;
	vcpu->arch.interrupt.nr = vector;
}

inline void
kvm_clear_interrupt_queue(struct kvm_vcpu *vcpu)
{
	vcpu->arch.interrupt.pending = 0;
}

int
kvm_event_needs_reinjection(struct kvm_vcpu *vcpu)
{
	return (vcpu->arch.exception.pending || vcpu->arch.interrupt.pending ||
	    vcpu->arch.nmi_injected);
}

inline int
kvm_exception_is_soft(unsigned int nr)
{
	return (nr == BP_VECTOR) || (nr == OF_VECTOR);
}

inline int
is_protmode(struct kvm_vcpu *vcpu)
{
	return (kvm_read_cr0_bits(vcpu, X86_CR0_PE));
}

int
is_long_mode(struct kvm_vcpu *vcpu)
{
#ifdef CONFIG_X86_64
	return (vcpu->arch.efer & EFER_LMA);
#else
	return (0);
#endif
}

inline int
is_pae(struct kvm_vcpu *vcpu)
{
	return (kvm_read_cr4_bits(vcpu, X86_CR4_PAE));
}

int
is_pse(struct kvm_vcpu *vcpu)
{
	return (kvm_read_cr4_bits(vcpu, X86_CR4_PSE));
}

int
is_paging(struct kvm_vcpu *vcpu)
{
	return (kvm_read_cr0_bits(vcpu, X86_CR0_PG));
}

uint64_t
native_read_msr_safe(unsigned int msr, int *err)
{
	DECLARE_ARGS(val, low, high);
	uint64_t ret = 0;
	on_trap_data_t otd;

	if (on_trap(&otd, OT_DATA_ACCESS) == 0) {
		ret = native_read_msr(msr);
		*err = 0;
	} else {
		*err = EINVAL; /* XXX probably not right... */
	}
	no_trap();

	return (ret);
}

/* Can be uninlined because referenced by paravirt */
int
native_write_msr_safe(unsigned int msr, unsigned low, unsigned high)
{
	int err = 0;
	on_trap_data_t otd;

	if (on_trap(&otd, OT_DATA_ACCESS) == 0) {
		native_write_msr(msr, low, high);
	} else {
		err = EINVAL;  /* XXX probably not right... */
	}
	no_trap();

	return (err);
}


/* XXX Where should this live */
page_t *
alloc_page(size_t size, int flag)
{
	caddr_t page_addr;
	pfn_t pfn;
	page_t *pp;

	if ((page_addr = kmem_zalloc(size, flag)) == NULL)
		return ((page_t *)NULL);

	pp = page_numtopp_nolock(hat_getpfnum(kas.a_hat, page_addr));
	return (pp);
}

int
kvm_vcpu_is_bsp(struct kvm_vcpu *vcpu)
{
	return (vcpu->kvm->bsp_vcpu_id == vcpu->vcpu_id);
}

/*
 * Often times we have pages that correspond to addresses that are in a users
 * virtual address space. Rather than trying to constantly map them in and out
 * of our address space we instead go through and use the kpm segment to
 * facilitate this for us. This always returns an address that is always in the
 * kernel's virtual address space.
 */
caddr_t
page_address(page_t *page)
{
	return (hat_kpm_mapin_pfn(page->p_pagenum));
}

uint32_t
bit(int bitno)
{
	return (1 << (bitno & 31));
}

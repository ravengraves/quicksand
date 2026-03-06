.intel_syntax noprefix
.text

.globl quicksand_now
.def quicksand_now; .scl 2; .type 32; .endef
quicksand_now:
	rdtsc
	shl rdx, 32
	or rax, rdx
	ret

#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);

/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *save_ptr;

	// 명령어 파싱: file_name을 공백을 기준으로 자름
	// 안자르면 스레드 네임에 인자까지 들어감
	file_name = strtok_r(file_name, " ", &save_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
	{

		palloc_free_page(fn_copy);
	}

	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();
	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{
	tid_t child_tid;
	memcpy(&thread_current()->parent_if, if_, sizeof(struct intr_frame));
	/* Clone current thread to new thread.*/
	child_tid = thread_create(name, PRI_DEFAULT, __do_fork, thread_current());
	// msg("자식 만듬!! :%d", child_tid);

	// 자식이 로드될 때까지 대기하기 위해서 방금 생성한 자식 스레드를 찾는다.
	struct thread *child = get_child_process(child_tid);

	// printf("exit_status : %d\n", child->exit_status);
	// printf("exit_tid : %d\n", child->tid);
	// 현재 스레드는 생성만 완료된 상태이다. 생성되어서 ready_list에 들어가고 실행될 때 __do_fork 함수가 실행된다.
	// __do_fork 함수가 실행되어 로드가 완료될 때까지 부모는 대기한다.
	sema_down(&child->fork_sema);
	// fork가 실패했을때 반환 tid 처리
	if (child->exit_status == -1)
	{
		list_remove(&child->child_elem);
		sema_up(&child->exit_sema);
		return TID_ERROR;
	}

	// 자식 프로세스의 pid를 반환
	return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
		return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page(parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
		return false;
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(void *aux)
{
	struct intr_frame if_;

	struct thread *current = thread_current();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct thread *parent = (struct thread *)aux;
	struct intr_frame *parent_if = &parent->parent_if;
	bool succ = true;
	/* 1. Read the cpu context to local stack. */
	memcpy(&if_, parent_if, sizeof(struct intr_frame));
	if_.R.rax = 0;
	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;
	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
	{
		goto error;
	}
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif
	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	for (int i = 3; i < 32; i++)
	{
		if (parent->fd_table[i] != NULL)
		{
			current->fd_table[i] = file_duplicate(parent->fd_table[i]);
		}
	}
	// printf("exit_status in fork : %d\n", current->exit_status);

	// fork 완료되면 깨우기
	sema_up(&current->fork_sema);
	process_init();
	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret(&if_);
error:
	// fork 실패했을 때 -1 처리
	current->exit_status = -1;
	sema_up(&current->fork_sema);
	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	char *file_name = f_name;
	bool success;
	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;

	memset(&_if, 0, sizeof _if); // intr_frame 구조체 초기화
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup();

	/* And then load the binary */
	success = load(file_name, &_if);
	/* If load failed, quit. */

	palloc_free_page(file_name);
	if (!success)
	{
		return -1;
	}

	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);
	/* Start switched process. */
	do_iret(&_if);
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid UNUSED)
{
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	struct thread *child = get_child_process(child_tid);
	if (child == NULL) // 자식이 아니면 -1 을 반환
	{
		return -1;
	}
	// printf("exit_status in wait1 : %d\n", child->exit_status);
	sema_down(&child->wait_sema);

	list_remove(&child->child_elem);
	tid_t a = child->exit_status;
	// printf("exit_status in wait2 : %d\n", child->exit_status);
	sema_up(&child->exit_sema);
	// printf("exit_status in wait3 : %d\n", child->exit_status);
	return a;
}

// struct thread *child_t_3 = thread_get_child(child_tid);
// printf("my : %d\n", child_t_3->exit_status);
// return thread_get_child(child_tid)->exit_status;

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	struct thread *cur = thread_current();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */

	if (cur->is_user)
	{
		printf("%s: exit(%d)\n", cur->name, cur->exit_status);
	}
	for (int i = 3; i < 32; i++)
	{
		file_close(cur->fd_table[i]);
		cur->fd_table[i] = NULL;
	}
	file_close(cur->running); // 현재 실행 중인 파일을 닫는다.
	process_cleanup();
	// printf("exit_status in exit1 : %d\n", cur->exit_status);
	sema_up(&cur->wait_sema);
	// printf("exit_status : %d\n", cur->exit_status);
	sema_down(&cur->exit_sema);
	// printf("exit_status in exit2 : %d\n", cur->exit_status);
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	char *token, *save_ptr;
	char *argv[128];
	int argc = 0;

	// 명령어 파싱: file_name을 공백을 기준으로 토큰화하여 argv에 저장
	token = strtok_r(file_name, " ", &save_ptr);
	while (token != NULL)
	{
		argv[argc] = token;
		argc++;
		// printf("token: %s\n", argv[argc - 1]);
		token = strtok_r(NULL, " ", &save_ptr);
	}

	// argc 값이 계산 되고 난 후 인자들의 주소를 담을 배열 크기를 정함
	uint64_t *argv_addresses[argc]; // 각 인자의 주소를 저장할 배열
	// argv[argc] = NULL;				// 인자 목록 끝에 NULL 추가
	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();

	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());
	/* Open executable file. */
	file = filesys_open(argv[0]);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", argv[0]);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					// printf("file_size : %d\n", read_bytes);
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
					// printf("file_size : %d\n", zero_bytes);
				}
				else
				{

					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
				{
					goto done;
				}
			}
			else
				goto done;
			break;
		}
	}

	if (!setup_stack(if_))
	{
		goto done;
	}
	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	// 스레드가 삭제될 때 파일을 닫을 수 있게 구조체에 파일을 저장해둔다.

	/* Set up stack. */
	// 8바이트 포인터
	uintptr_t esp = if_->rsp;
	// 1. rsp값을 낮춰가며 stack에 프로그램 이름과 인자 값을 넣어준다.
	for (int i = argc - 1; i >= 0; i--)
	{
		esp -= strlen(argv[i]) + 1;
		memcpy(esp, argv[i], strlen(argv[i]) + 1); // 인자 값을 스택에 복사
		// printf("rsp 값 : %s\n", argv[1]);
		argv_addresses[i] = esp;
	}
	// // 2. 8의 배수 바이트 수로 맞춰주기 위해 패딩값을 넣는다.
	while (esp % 8 != 0)
	{
		esp--;
		// 1바이트 짜리 포인터로 형변환
		*(uint8_t *)esp = 0;
	}
	// 3. NULL포인터 추가
	esp -= sizeof(char *);
	*(char **)esp = 0;

	// 위와 결과가 같음
	// *esp = 0;
	// 4.rsp값을 낮춰가며 stack에 프로그램 이름 주소와 인자 주소를 넣어준다.
	for (int i = argc - 1; i >= 0; i--)
	{
		esp -= sizeof(char *);
		memcpy(esp, &argv_addresses[i], sizeof(char *)); // 인자 값을 스택에 복사
	}
	/* 4-1. argv의 시작 주소 저장 */
	uintptr_t start_argv = esp;
	// 5.return address 값 stack에 넣기
	esp -= sizeof(void *);
	*(void **)esp = 0;
	//%rsi 가 argv 주소(argv[0]의 주소)를 가리키게 하고, %rdi 를 argc 로 설정
	if_->rsp = esp;
	if_->R.rdi = argc;
	if_->R.rsi = start_argv;

	t->running = file;
	// 현재 실행중인 파일은 수정할 수 없게 막는다.
	file_deny_write(file);

	success = true;
done:
	/* We arrive here whether the load is successful or not. */
	// file_close(file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
// #if 1
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, void *aux)
{

	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct load_info *load_info = (struct load_info *)aux;
	// 파일에서 데이터를 읽기 시작할 위치를 설정.
	file_seek(load_info->file, load_info->offset);
	// 해당 페이지의 커널 가상 주소를 얻음.
	void *kpage = page->frame->kva;
	// file_read를 사용하여 파일에서 데이터를 읽어와 페이지에 로드함.
	if (file_read(load_info->file, kpage, load_info->read_bytes) != (int)load_info->read_bytes)
	{
		return false;
	}
	// 페이지의 나머지 부분을 0으로 채움.
	memset(kpage + load_info->read_bytes, 0, load_info->zero_bytes);
	/* Add the page to the process's address space. */
	// msg("load_segment z : %d\n", load_info->zero_bytes);
	// if (pml4_get_page(t->pml4, load_info->upage) != NULL)
	// {
	// 	printf("fdgdfgdf %p\n", load_info->upage);
	// 	// if (!vm_claim_page(load_info->upage))
	// 	// {
	// 	// 	return false;
	// 	// }
	// }
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct load_info *aux = malloc(sizeof(struct load_info));
		if (aux == NULL)
		{
			return false;
		}
		aux->file = file;
		aux->offset = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->writable = writable;
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
		{
			free(aux);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		// 승우님 추가
		ofs += page_read_bytes;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	// bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);
	// printf("stack_bottom : %p\n", stack_bottom);
	// 주어진 stack_bottom 주소에 해당하는 페이지를 현재 스레드 페이지 테이블에 할당하고 메모리에 매핑
	if (!vm_alloc_page(VM_ANON | VM_STACK, stack_bottom, true))
	{
		return false;
	}
	if (!vm_claim_page(stack_bottom))
	{
		printf("페이지 할당실패!!!\n");
		return false;
	}
	if_->rsp = USER_STACK;
	// printf("안녕하세요! : %p\n", stack_bottom);
	return true;
}
#endif /* VM */

#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include <hash.h>
enum vm_type
{
	/* page not initialized */
	VM_UNINIT = 0,
	/* page not related to the file, aka anonymous page */
	VM_ANON = 1,
	/* page that realated to the file */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_STACK = (1 << 3),
	// VM_WRITABLE = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* The representation of "page".
 * This is kind of "parent class", which has four "child class"es, which are
 * uninit_page, file_page, anon_page, and page cache (project4).
 * DO NOT REMOVE/MODIFY PREDEFINED MEMBER OF THIS STRUCTURE. */
// page 구조체가 곧 vm_entry 역할을 함.
struct page
{
	const struct page_operations *operations;
	// 사용자 공간에서의 가상주소를 나타냄
	void *va; /* Address in terms of user space */
	// 이 페이지가 매핑된 물리 메모리 프레임을 가리키는 포인터
	struct frame *frame; /* Back reference for frame */

	/* Your implementation */
	bool writable;
	struct hash_elem hash_elem; // 해시 테이블에 삽입하기 위해 추가.
	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	union
	{
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" */
struct frame
{
	void *kva;
	struct page *page;
	struct thread *owner;
};

/* The function table for page operations.
 * This is one way of implementing "interface" in C.
 * Put the table of "method" into the struct's member, and
 * call it whenever you needed. */
struct page_operations
{
	bool (*swap_in)(struct page *, void *);
	bool (*swap_out)(struct page *);
	void (*destroy)(struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in((page), v)
#define swap_out(page) (page)->operations->swap_out(page)
#define destroy(page)                \
	if ((page)->operations->destroy) \
	(page)->operations->destroy(page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table
{
	struct hash vm;
};

#include "threads/thread.h"
void supplemental_page_table_init(struct supplemental_page_table *spt);
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src);
void supplemental_page_table_kill(struct supplemental_page_table *spt);
struct page *spt_find_page(struct supplemental_page_table *spt,
						   void *va);
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page);
void spt_remove_page(struct supplemental_page_table *spt, struct page *page);

void vm_init(void);
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user,
						 bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
									bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page(struct page *page);
bool vm_claim_page(void *va);
enum vm_type page_get_type(struct page *page);

bool is_stack_page(struct page *page);
// bool is_writable_page(struct page *page);
// load_segment함수에서 바이너리 파일을 로드할 때 필수적인 정보를 포함하는 구조체 정의
struct load_info
{
	struct file *file; // 파일 포인터
	off_t offset;	   // 파일에서 읽기 시작할 오프셋
	size_t read_bytes; // 파일에서 읽을 바이트 수
	size_t zero_bytes; // 0으로 채울 바이트 수
	// uint8_t *upage;
	bool writable; // 페이지가 쓰기 가능한지 여부
	// munmap 위함
	int cnt;
};

#endif /* VM_VM_H */

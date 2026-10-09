#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/palloc.h"

#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGS 128

struct parsed_command {
	int argc;
	char *argv[MAX_ARGS];
	char *program_name;
};

struct initd_aux {
	char *file_name;
	struct thread *parent;
	struct semaphore init_sema;
};

static void process_cleanup (void);
static bool load (char *file_name, struct intr_frame *if_);
static bool parse_command_line (char *cmdline, struct parsed_command *cmd);
static bool setup_arguments (struct intr_frame *if_,
		const struct parsed_command *cmd);
static void initd (void *f_name);
static void __do_fork (void *);

/* initd와 다른 프로세스를 위한 일반 프로세스 초기화 함수.
 *
 * [Phase 0] 의도적으로 비워 둔다.
 * fd_table 할당 시점은 호출 경로별로 명시적으로 둔다:
 *   - initd: 진입 직후 fdt_init() 호출
 *   - __do_fork: fdt_copy() 호출 (내부에서 fdt_init 수행)
 *   - process_exec: 기존 스레드의 fd_table을 그대로 유지 (재할당 X) */
static void
process_init (void) {
	struct thread *current = thread_current ();
	(void) current;
}

/* ====================================================================
 * [Phase 0] 파일 디스크립터 테이블 헬퍼
 * ==================================================================== */

bool
fdt_init (struct thread *t) {
	ASSERT (t != NULL);
	if (t->fd_table != NULL)
		return true;
	t->fd_table = palloc_get_page (PAL_ZERO);
	if (t->fd_table == NULL)
		return false;
	t->next_fd = 2;
	return true;
}

void
fdt_destroy (struct thread *t) {
	ASSERT (t != NULL);
	if (t->fd_table != NULL) {
		palloc_free_page (t->fd_table);
		t->fd_table = NULL;
	}
}

int
fdt_add (struct file *file) {
	struct thread *t = thread_current ();
	if (t->fd_table == NULL || file == NULL)
		return -1;

	/* next_fd를 hint로 시작해 빈 슬롯을 찾고, 끝까지 못 찾으면 2부터 다시 검사. */
	for (int fd = t->next_fd; fd < FDT_LIMIT; fd++) {
		if (t->fd_table[fd] == NULL) {
			t->fd_table[fd] = file;
			t->next_fd = fd + 1;
			return fd;
		}
	}
	for (int fd = 2; fd < t->next_fd; fd++) {
		if (t->fd_table[fd] == NULL) {
			t->fd_table[fd] = file;
			t->next_fd = fd + 1;
			return fd;
		}
	}
	return -1;
}

struct file *
fdt_get (int fd) {
	struct thread *t = thread_current ();
	if (t->fd_table == NULL)
		return NULL;
	if (fd < 2 || fd >= FDT_LIMIT)
		return NULL;
	return t->fd_table[fd];
}

void
fdt_remove (int fd) {
	struct thread *t = thread_current ();
	if (t->fd_table == NULL)
		return;
	if (fd < 2 || fd >= FDT_LIMIT)
		return;
	t->fd_table[fd] = NULL;
	if (fd < t->next_fd)
		t->next_fd = fd;
}

void
fdt_close_all (struct thread *t) {
	ASSERT (t != NULL);
	if (t->fd_table == NULL)
		return;
	for (int fd = 2; fd < FDT_LIMIT; fd++) {
		struct file *f = t->fd_table[fd];
		if (f != NULL) {
			/* file_close 자체는 자체적으로 동기화가 필요 없지만,
			 * filesys 메타 변경(inode write-back 등)이 있을 수 있으므로
			 * 굵은 락으로 직렬화. */
			lock_acquire (&filesys_lock);
			file_close (f);
			lock_release (&filesys_lock);
			t->fd_table[fd] = NULL;
		}
	}
}

bool
fdt_copy (struct thread *parent, struct thread *child) {
	ASSERT (parent != NULL && child != NULL);
	if (parent->fd_table == NULL)
		return true;
	if (!fdt_init (child))
		return false;

	for (int fd = 2; fd < FDT_LIMIT; fd++) {
		struct file *src = parent->fd_table[fd];
		if (src == NULL)
			continue;
		lock_acquire (&filesys_lock);
		struct file *dup = file_duplicate (src);
		lock_release (&filesys_lock);
		if (dup == NULL) {
			/* 복제 실패 시 호출자가 child를 정리(fdt_close_all 후 fdt_destroy)할 책임. */
			return false;
		}
		child->fd_table[fd] = dup;
	}
	child->next_fd = parent->next_fd;
	return true;
}

/* ====================================================================
 * [Phase 0] 자식 프로세스 관계 헬퍼
 * ====================================================================
 * 자료구조 hook만 제공한다. 차단·재우기·exit_status 회수는 B 담당이 채운다. */

void
child_register (struct thread *parent, struct thread *child) {
	ASSERT (parent != NULL && child != NULL);
	child->parent = parent;
	list_push_back (&parent->children, &child->child_elem);
}

struct thread *
child_find (struct thread *parent, tid_t tid) {
	ASSERT (parent != NULL);
	for (struct list_elem *e = list_begin (&parent->children);
			e != list_end (&parent->children);
			e = list_next (e)) {
		struct thread *c = list_entry (e, struct thread, child_elem);
		if (c->tid == tid)
			return c;
	}
	return NULL;
}

void
child_remove (struct thread *child) {
	ASSERT (child != NULL);
	list_remove (&child->child_elem);
}

/* FILE_NAME에서 로드한 "initd"라는 첫 번째 사용자 영역 프로그램을 시작한다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄될 수 있고,
 * 심지어 종료될 수도 있다. initd의 스레드 id를 반환하며, 스레드를 생성할 수
 * 없으면 TID_ERROR를 반환한다. 이 함수는 한 번만 호출되어야 한다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* FILE_NAME의 복사본을 만든다.
	 * 그렇지 않으면 호출자와 load() 사이에 경쟁 상태가 생긴다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	// 자식 스레드의 정보를 저장하기 위한 구조체임
	struct initd_aux aux;
	aux.file_name = fn_copy;
	aux.parent = thread_current ();
	sema_init (&aux.init_sema, 0);

	/* FILE_NAME을 실행할 새 스레드를 생성한다. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, &aux);
	if (tid == TID_ERROR) {
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}

	sema_down (&aux.init_sema);

	return tid;
}

/* 첫 번째 사용자 프로세스를 시작하는 스레드 함수. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif
	struct initd_aux *aux = f_name;
	char *file_name = aux->file_name;

	child_register (aux->parent, thread_current ());
	sema_up (&aux->init_sema);

	process_init ();

	/* [Phase 0] 사용자 프로세스 진입 직전 fd_table 할당. */
	if (!fdt_init (thread_current ()))
		PANIC ("Failed to allocate fd table for initd\n");

	if (process_exec (file_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`으로 복제한다. 새 프로세스의 스레드 id를 반환하며,
 * 스레드를 생성할 수 없으면 TID_ERROR를 반환한다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* 현재 스레드를 새 스레드로 복제한다. */
	struct thread *current = thread_current ();
	current->parent_if = *if_;
	tid_t tid = thread_create (name,
			PRI_DEFAULT, __do_fork, current);

	if (tid == TID_ERROR) {
		return TID_ERROR;
	}

	sema_down (&current->fork_sema);

	if (!current->fork_success) {
		return TID_ERROR;
	}

	return tid;
}

#ifndef VM
/* 이 함수를 pml4_for_each에 전달하여 부모의 주소 공간을 복제한다.
 * 이 코드는 프로젝트 2에서만 사용된다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: parent_page가 커널 페이지라면 즉시 반환한다. */
	if (is_kern_pte (pte)) {
		return true;
	}

	/* 2. 부모의 page map level 4에서 VA를 해석한다. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) {
		return false;
	}

	/* 3. TODO: 자식용 새 PAL_USER 페이지를 할당하고 결과를
	 *    TODO: NEWPAGE에 설정한다. */
	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL) {
		return false;
	}

	/* 4. TODO: 부모의 페이지를 새 페이지에 복제하고,
	 *    TODO: 부모 페이지가 쓰기 가능한지 확인한다(결과에 따라 WRITABLE을
	 *    TODO: 설정한다). */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable (pte);

	/* 5. WRITABLE 권한으로 주소 VA에 있는 자식의 페이지 테이블에
	 *    새 페이지를 추가한다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: 페이지 삽입에 실패하면 오류 처리를 한다. */
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트를 복사하는 스레드 함수.
 * 힌트) parent->tf에는 프로세스의 사용자 영역 컨텍스트가 들어 있지 않다.
 *       즉, process_fork의 두 번째 인자를 이 함수에 전달해야 한다. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: 어떻게든 parent_if를 전달한다. (즉, process_fork()의 if_) */
	struct intr_frame *parent_if = &parent->parent_if;
	bool succ = true;

	/* 1. cpu 컨텍스트를 로컬 스택으로 읽어 온다. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. PT를 복제한다. */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL) {
		succ = false;	// 실패 시 succ 상태를 false로 변경
		goto done;		// 일괄적인 완료 처리를 위해 done으로 레이블 변경
	}

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt)) {
		succ = false;
		goto done;
	}
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)) {
		succ = false;
		goto done;
	}
#endif

	/* TODO: 여기에 코드를 작성한다.
	 * TODO: 힌트) 파일 객체를 복제하려면 include/filesys/file.h의
	 * TODO:       `file_duplicate`을 사용한다. 이 함수가 부모의 리소스를
	 * TODO:       성공적으로 복제하기 전까지 부모는 fork()에서 반환해서는
	 * TODO:       안 된다는 점에 유의한다. */

	if (!fdt_copy (parent, current)) {
		succ = false;
		goto done;
	}

	// 복제가 성공한 뒤 부모의 children 리스트에 등록
	child_register (parent, current);

	process_init ();

	/* 마지막으로 새로 생성한 프로세스로 전환한다. */
done:
	parent->fork_success = succ;
	sema_up (&parent->fork_sema);

	if (succ) {
		if_.R.rax = 0;
		do_iret (&if_);
	}

	thread_exit ();
}

/* 현재 실행 컨텍스트를 f_name으로 전환한다.
 * 실패하면 -1을 반환한다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* 스레드 구조체 안의 intr_frame은 사용할 수 없다.
	 * 현재 스레드가 다시 스케줄될 때 실행 정보를 해당 멤버에 저장하기
	 * 때문이다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 현재 컨텍스트를 제거한다. */
	process_cleanup ();

	/* 그런 다음 바이너리를 로드한다. */
	success = load (file_name, &_if);

	/* 로드에 실패하면 종료한다. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* 전환된 프로세스를 시작한다. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* 스레드 TID가 종료될 때까지 기다린 뒤 종료 상태를 반환한다.
 * 커널에 의해 종료되었다면(즉, 예외 때문에 종료되었다면) -1을 반환한다.
 * TID가 유효하지 않거나, 호출 프로세스의 자식이 아니거나, 주어진 TID에 대해
 * process_wait()가 이미 성공적으로 호출된 적이 있다면 기다리지 않고 즉시
 * -1을 반환한다.
 *
 * 이 함수는 문제 2-2에서 구현될 것이다. 지금은 아무 일도 하지 않는다. */
int
process_wait (tid_t child_tid) {
	/* XXX: 힌트) process_wait(initd)가 반환되면 pintos가 종료되므로,
	 * XXX:       process_wait를 구현하기 전에는 여기에 무한 루프를
	 * XXX:       추가하는 것을 권장한다. */

	struct thread *child = child_find (thread_current (), child_tid);
	if (child == NULL || child->waited) {
		return -1;
	}

	child->waited = true;
	sema_down (&child->wait_sema);

	int status = child->exit_status;
	child_remove (child);
	sema_up (&child->exit_sema);

	return status;
}

/* 프로세스를 종료한다. 이 함수는 thread_exit()에서 호출된다.
 *
 * [Phase 0] 정리 책임 표준 (담당별 hook 위치):
 *   1) 종료 메시지 출력                           — 공통
 *   2) FD 테이블의 모든 파일 close + 페이지 해제   — 공통 (fdt_close_all + fdt_destroy)
 *   3) 자기 실행 파일에 file_allow_write + close — A 담당 (running_file 셋업 후)
 *   4) 부모에 종료 신호: exited=true, wait_sema up — B 담당
 *   5) 부모의 reap을 기다림(exit_sema down)        — B 담당
 *   6) 자식들 정리 / 분리 (orphan 처리)            — B 담당
 *   7) 페이지 디렉터리 제거 (process_cleanup)      — 공통, 마지막에 수행
 *
 * Phase 0에서는 1·2·7만 구현한다. 3~6은 각 담당이 본인 PR에서 채운다. */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	/* 1) 표준 종료 메시지. 사용자 프로세스만 출력 (kernel thread 제외).
	 *    pml4 != NULL을 사용자 프로세스 식별 조건으로 사용. */
	if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	/* 2) FD 테이블 정리. */
	fdt_close_all (curr);
	fdt_destroy (curr);

	//[TODO A] 실행 파일에 쓰기 허용 + close.
	 if (curr->running_file != NULL) {
	 lock_acquire (&filesys_lock);
	 file_allow_write (curr->running_file);
	 file_close (curr->running_file);
	 lock_release (&filesys_lock);
	 curr->running_file = NULL;
	 }
	

	/* 4)·5) [TODO B] 부모 reap 동기화. */
	
	/* 6) [TODO B] 자식 분리.
	*    자식들의 parent를 NULL로 설정하여 고아로 만든다.
	*    이미 종료되어 부모의 reap을 기다리는 자식은 exit_sema를 up하여 완전히 종료될 수 있도록 한다.
	*/
	struct list_elem *e = list_begin (&curr->children);
	while (e != list_end (&curr->children)) {
		struct list_elem *next = list_next (e);
		struct thread *child = list_entry (e, struct thread, child_elem);

		child->parent = NULL;
		child_remove (child);

		if (child->exited) {
			sema_up (&child->exit_sema);
		}

		e = next;
	}

	// 자식 스레드의 종료 동작
	curr->exited = true;
	if (curr->parent != NULL) {
		sema_up (&curr->wait_sema);
		sema_down (&curr->exit_sema);
	}

	/* 7) 페이지 디렉터리 제거. */
	process_cleanup ();
}

/* 현재 프로세스의 리소스를 해제한다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 디렉터리를 제거하고 커널 전용 페이지
	 * 디렉터리로 다시 전환한다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서는 올바른 순서가 매우 중요하다. 페이지 디렉터리를 전환하기
		 * 전에 cur->pagedir를 NULL로 설정해야 타이머 인터럽트가 프로세스
		 * 페이지 디렉터리로 다시 전환하지 못한다. 또한 프로세스의 페이지
		 * 디렉터리를 제거하기 전에 기본 페이지 디렉터리를 활성화해야 한다.
		 * 그렇지 않으면 현재 활성 페이지 디렉터리가 이미 해제되고 비워진
		 * 페이지 디렉터리가 될 수 있다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음 스레드에서 사용자 코드를 실행하도록 CPU를 설정한다.
 * 이 함수는 모든 컨텍스트 전환 때 호출된다. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 테이블을 활성화한다. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정한다. */
	tss_update (next);
}

/* ELF 바이너리를 로드한다. 다음 정의들은 ELF 명세 [ELF1]에서 거의
 * 그대로 가져온 것이다. */

/* ELF 타입. [ELF1] 1-2를 참고하라. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시한다. */
#define PT_LOAD    1            /* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링킹 정보. */
#define PT_INTERP  3            /* 동적 로더의 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 테이블. */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더. [ELF1] 1-4부터 1-8까지를 참고하라.
 * ELF 바이너리의 맨 앞에 나타난다. */
struct ELF64_hdr {
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

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME의 ELF 실행 파일을 현재 스레드에 로드한다.
 * 실행 파일의 진입점을 *RIP에 저장하고 초기 스택 포인터를 *RSP에 저장한다.
 * 성공하면 true를, 아니면 false를 반환한다. */
static bool
load (char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	struct parsed_command cmd;
	off_t file_ofs;
	bool success = false;
	int i;

	if (!parse_command_line (file_name, &cmd))
		goto done;

	// command line 파싱이 성공하면 현재 스레드의 이름을 프로그램명으로 바꾼다 (안 바꾸면 이름에 인자까지 들어갈 수 있음)
	strlcpy (thread_current ()->name, cmd.program_name, sizeof (thread_current ()->name));

	/* 페이지 디렉터리를 할당하고 활성화한다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* 실행 파일을 연다. */
	file = filesys_open (cmd.program_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", cmd.program_name);
		goto done;
	}
	file_deny_write (file);
	t->running_file = file;

	/* 실행 파일 헤더를 읽고 검증한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", cmd.program_name);
		goto done;
	}

	/* 프로그램 헤더를 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트를 무시한다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 일반 세그먼트.
						 * 앞부분은 디스크에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전체가 0이다.
						 * 디스크에서 아무것도 읽지 않는다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 스택을 설정한다. */
	if (!setup_stack (if_))
		goto done;

	if (!setup_arguments (if_, &cmd))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	success = true;

done:
	/* 로드 성공 여부와 관계없이 여기로 온다. */
	if (!success && file != NULL){	
		t->running_file = NULL;
		file_close (file);
	}
	return success;
}

/* Splits CMDLINE into executable name and argv tokens.
 * TODO(ap/parser): Implement whitespace tokenization and populate CMD. */

 //  (!parse_command_line (file_name, &cmd))
static bool
parse_command_line (char *cmdline, struct parsed_command *cmd) {
	ASSERT (cmdline != NULL); 
	ASSERT (cmd != NULL); 
	
	char *save = NULL;
	// struct parsed_command {
	// 	int argc;
	// 	char *argv[MAX_ARGS];
	// 	char *program_name;
	// };

	// 내가 인자 개수 count해 여기서?
	// argv 도 하나하나 넣어주고

	// memset 함수는 메모리의 연속된 바이트 구간을 같은 값으로 채우는 함수 
	// 채울 메모리의 시작 주소, 각 바이트에 넣을 값, 바이트 개수 
	memset (cmd, 0, sizeof *cmd);
	

	// 입력이 아무것도 없으면? 
	if (cmdline[0] == '\0')
		return false;

	// 공백 단위로 자르는거 잇어 C언어에도? -> strtok_r() : 문자열을 구분자 기준으로 잘라서 token을 하나씩 꺼내는 함수
	// str은 처음 자를 문자열, delim은 구분자, saveptr은 다음에 어디서부터 이어서 자를지 기억하는 포인터 
	// 처음 호출할 때만 원본 문자열을 넣고 그 다음부터는 첫 번째 인자에 NULL 넣음. 원본 문자열을 직접 바꿈. 문자열 안의 공백은 \0 으로 바꾸고 각 token은 원래 문자열 내부를 가리키는 포인터. 더 이상 자를 토큰이 없으면 “끝났다”는 신호로 `NULL`을 반환

	char *token = strtok_r(cmdline, " ", &save);

	// 입력이 공백만 있으면? 
	if (token == NULL) 
		return false;


	cmd->argv[0] = token;
	cmd->program_name = token; 

	int i = 1;
	
    while (token != NULL && i < MAX_ARGS) {
        token = strtok_r(NULL, " ", &save);
		if (token != NULL) { // *token != NULL 이 맞다고 생각했는데 token이 NULL일 경우 *token 접근하면 세그폴트 터질 수 있고 
			cmd->argv[i] = token;
			i++;
		}
    }

	if (token != NULL)
		return false;

	cmd->argc = i;
	return true;

	// AI를 너무 많이 써버렷다. 

}

/* 파싱된 인자를 사용자 스택에 복사한다.
 * TODO(ap/stack): 문자열, argv 포인터, null 센티널을 푸시하고 스택을
 * 정렬한 뒤 if_->R.rdi / if_->R.rsi를 설정한다. */
static bool
setup_arguments (struct intr_frame *if_, const struct parsed_command *cmd) {
	ASSERT (if_ != NULL);
	ASSERT (cmd != NULL);

	uintptr_t stack_bottom = USER_STACK - PGSIZE;
	char *arg_addrs[MAX_ARGS];	// argv 값들을 유저 공간 stack에 push한 이후 유저 공간의 argv 주소를 저장하는 영역에 주소값을 push할 때 쓸 변수

	// Stack에 push하기 (파싱된 문자열 인자들)
	for (int i = cmd->argc - 1; i >= 0; i--) {
		size_t len = strlen (cmd->argv[i]) + 1;

		if (if_->rsp < stack_bottom + len) {	// 계속해서 주어진 유저 공간이 초과되지는 않는지 검사한다.
			return false;
		}
		if_->rsp -= len;
		memcpy ((void *) if_->rsp, cmd->argv[i], len);	// 지금 현재 있는 cmd->argv[i] 값들은 커널 메모리에 존재하는 값이라서 유저 메모리 영역으로 '복사'를 해줘야 함. 커널 메모리를 유저가 쓰면 안 되기 때문이기도 하고 수명 문제도 있음

		arg_addrs[i] = (char *) if_->rsp;				// 유저 영역에 저장된 cmd->argv[i]들의 주소값을 저장
	}
	
	// word align (8bytes padding)
	// $rsp 위치가 8의 배수가 아니면 8의 배수로 맞춰야 함. 8바이트가 되기까지 부족한 만큼을 건너뛰기 위해 나머지 연산 사용
	if (if_->rsp < stack_bottom + if_->rsp % 8) {
			return false;
	}
	if_->rsp -= if_->rsp % 8;	// 8은 Memory Alignment 단위인 8바이트를 의미

	// argv[argc] 위치에 NULL 삽입
	if (if_->rsp < stack_bottom + sizeof (char *)) {
			return false;
	}
	if_->rsp -= sizeof (char *);	// argv[i]는 char * 크기를 가지므로 그만큼을 비워두고 저장한다.

	/*
	주소값을 값으로 가질 수 있는 타입은 무엇인가?
	-> 포인터
	char * -> char
	*(char **) -> (char *) = 주소값;
	*/

	*(char **) if_->rsp = NULL;		// char *의 값(메모리 주소값)을 넣기 위해서 char **가 갖는 값(char *)으로 접근하여 할당한다. 그렇지 않으면 해당 주소값 자체가 char 하나로(1바이트 값) 덮어 씌워진다.

	// argv[argc] ~ argv[i]까지 push: 유저 공간에 있는 argv의 주소값을 유저 공간의 스택 영역에 push한다
	for (int i = cmd->argc - 1; i >= 0; i--) {
		if (if_->rsp < stack_bottom + sizeof (char *)) {
			return false;
		}
		if_->rsp -= sizeof(char *);
		*(char **) if_->rsp = arg_addrs[i];
	}

	char **argv = (char **) if_->rsp;	// 레지스터에 삽입할 때 기존 argv처럼 쓸 수 있도록 제공해주기 위한 코드
	// ap/register-hook-and-tests 브랜치에서 할 작업: rdi = argc, rsi = argv
	if_->R.rdi = (uint64_t)cmd->argc;
	if_->R.rsi = (uint64_t)argv;
	// fake 반환 주소에 NULL push: _start() 함수의 초기 스택 프레임 모양을 일반 함수 호출처럼 맞추는 부분 (반환하는 거 없어서 사실 필요 없는데 다른 함수랑 똑같이 생기게 하려고 작성하는 코드)
	if (if_->rsp < stack_bottom + sizeof (void *)) {
			return false;
	}
	if_->rsp -= sizeof (void *);
	*(void **) if_->rsp = NULL;

	return true;

	// AI를 너무 많이 써버렷다...
}


/* PHDR이 FILE 안의 유효하고 로드 가능한 세그먼트를 설명하는지 확인하고,
 * 그렇다면 true를, 아니면 false를 반환한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr은 같은 페이지 오프셋을 가져야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 내부를 가리켜야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 최소한 p_filesz만큼 커야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트는 비어 있으면 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역의 시작과 끝은 모두 사용자 주소 공간 범위 안에
	   있어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 이 영역은 커널 가상 주소 공간을 가로질러 "wrap around"되면 안 된다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 페이지 0 매핑을 허용하지 않는다.
	   페이지 0을 매핑하는 것은 좋지 않을 뿐 아니라, 이를 허용하면 시스템 콜에
	   널 포인터를 전달한 사용자 코드가 memcpy() 등의 널 포인터 assertion을
	   통해 커널 패닉을 일으킬 가능성이 높다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 문제가 없다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 프로젝트 2 동안에만 사용된다.
 * 프로젝트 2 전체에서 사용할 함수를 구현하려면 #ifndef 매크로 밖에
 * 구현하라. */

/* load() 헬퍼. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화된다.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터 읽어야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가 쓸 수
 * 있어야 하고, 그렇지 않으면 읽기 전용이어야 한다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고 마지막 PAGE_ZERO_BYTES
		 * 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 페이지를 얻는다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지를 로드한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 프로세스의 주소 공간에 페이지를 추가한다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음으로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 채운 페이지를 매핑하여 최소 스택을 만든다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 사용자 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을 페이지
 * 테이블에 추가한다.
 * WRITABLE이 true이면 사용자 프로세스가 페이지를 수정할 수 있고,
 * 그렇지 않으면 읽기 전용이다.
 * UPAGE는 이미 매핑되어 있으면 안 된다.
 * KPAGE는 palloc_get_page()로 사용자 풀에서 얻은 페이지여야 할 것이다.
 * 성공하면 true를 반환하고, UPAGE가 이미 매핑되어 있거나 메모리 할당이
 * 실패하면 false를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 해당 가상 주소에 이미 페이지가 없는지 확인한 뒤, 그곳에 페이지를
	 * 매핑한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기부터의 코드는 프로젝트 3 이후에 사용된다.
 * 프로젝트 2에서만 사용할 함수를 구현하려면 위쪽 블록에 구현하라. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: 파일에서 세그먼트를 로드한다. */
	/* TODO: 이 함수는 주소 VA에서 첫 번째 페이지 폴트가 발생할 때 호출된다. */
	/* TODO: 이 함수를 호출할 때 VA를 사용할 수 있다. */
}

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에 로드한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이 초기화된다.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터 읽어야 한다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 한다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가 쓸 수
 * 있어야 하고, 그렇지 않으면 읽기 전용이어야 한다.
 *
 * 성공하면 true를 반환하고, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고 마지막 PAGE_ZERO_BYTES
		 * 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: lazy_load_segment에 정보를 전달하도록 aux를 설정한다. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* 다음으로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 스택 PAGE를 만든다. 성공하면 true를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택을 매핑하고 즉시 페이지를 claim한다.
	 * TODO: 성공하면 그에 맞게 rsp를 설정한다.
	 * TODO: 페이지가 스택임을 표시해야 한다. */
	/* TODO: 여기에 코드를 작성한다. */

	return success;
}
#endif /* VM */

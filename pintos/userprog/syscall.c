#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/flags.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "intrinsic.h"
#include "threads/palloc.h"

/* ====================================================================
 * [Phase 0] 시스템 콜 공통 레이어
 *
 * - 검증 헬퍼:        check_address / check_buffer / check_string
 * - 전역 자원:        filesys_lock
 * - 디스패치 표:      syscall_handler 안의 switch
 * - 미구현 syscall:   각 담당(A/B/C/D)이 채우도록 stub만 둔다.
 *
 * 담당자 가이드:
 *   A: halt, exit, exec
 *   B: fork, wait
 *   C: create, remove, open, close, filesize, seek, tell
 *   D: read, write
 *
 * 모든 stub은 sys_exit(-1)이 아니라 -1 반환으로 두었다.
 * 이유: 미구현 호출이 곧바로 프로세스를 죽여버리면 무관한 syscall을 부르는 다른
 * 테스트(특히 args-* 회귀)도 같이 죽는다. -1로 두면 호출자만 실패 신호를 받는다.
 * 단, halt/exit/exec/fork처럼 "정상 반환이 의미 없는" 콜은 unreachable 표시.
 * ==================================================================== */

#define MSR_STAR 0xc0000081         /* 세그먼트 선택자 MSR */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL 대상 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags에 대한 마스크 */

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void power_off (void);

size_t strlcpy (char *, const char *, size_t);

struct lock filesys_lock;

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* syscall_entry가 사용자 영역 스택을 커널 모드 스택으로 교체하기 전까지
	 * 인터럽트 서비스 루틴은 어떤 인터럽트도 처리해서는 안 된다.
	 * 따라서 FLAG_FL을 마스크했다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	/* [Phase 0] 파일시스템 굵은 락 초기화. */
	lock_init (&filesys_lock);
}

/* ====================================================================
 * 사용자 포인터 검증
 * ====================================================================
 *
 * 정책 (Phase 0 합의):
 *   - NULL, 커널 영역(KERN_BASE 이상), 페이지 미매핑 → sys_exit(-1)
 *   - 검증 실패는 syscall 진입 단계에서만 처리한다.
 *     (사용자 코드 자체의 page fault는 exception.c가 처리.)
 *   - check_buffer는 페이지 경계마다 검사하므로 size 0도 안전.
 *   - check_string은 NUL을 만날 때까지 1바이트씩 검사 (긴 문자열도 OK).
 *
 * NOTE: 더 빠른 방식(rdi 매핑 캐시 + 드라이ㄴ try/recover)도 있지만,
 *       Phase 0은 명료성을 우선해 pml4_get_page 기반으로 통일한다.
 *       성능이 문제되면 D 담당이 이후 최적화. */

void
check_address (const void *uaddr) {
	if (uaddr == NULL || !is_user_vaddr (uaddr))
		sys_exit (-1);
	if (pml4_get_page (thread_current ()->pml4, uaddr) == NULL)
		sys_exit (-1);
}

void
check_buffer (const void *buf, size_t size, bool writable) {
	if (size == 0)
		return;
	if (buf == NULL)
		sys_exit (-1);

	/* writable 검사용 헬퍼:
	 * pml4의 PTE를 직접 보는 API가 없으므로, writable이 true이면
	 * 후속 단계(D 담당)가 mmu.h의 pml4e/pte 조작으로 보강할 수 있도록
	 * 여기서는 매핑 존재 여부만 검사한다.
	 * (TODO[D]: 실제 writable 비트 검사 추가 — read 시스템콜의 buffer 검증) */

	const uint8_t *p = buf;
	const uint8_t *end = p + size;
	const uint8_t *page = pg_round_down (p);
	uint64_t *pml4 = thread_current ()->pml4;

	for (; page < end; page += PGSIZE) {	//페이지 유효한지 검사용
		if (!is_user_vaddr (page) || pml4_get_page (pml4, page) == NULL)
			sys_exit (-1);
		if (writable) { //작성시에 작성 가능 여부 확인용
			uint64_t *pte = pml4e_walk(pml4, (uint64_t)page, false);
			if((*pte&PTE_W) == 0)	//쓰기가 불가능한 페이지일 때
				sys_exit(-1);
		}
	}
}

void
check_string (const char *ustr) {
	if (ustr == NULL)
		sys_exit (-1);
	
	uint64_t *pml4 = thread_current ()->pml4;
	const char *p = ustr;
	const void *cur_page = NULL;

	for (;;) {
		const void *page = pg_round_down (p);
		if (page != cur_page) {
			if (!is_user_vaddr (p) || pml4_get_page (pml4, page) == NULL)
				sys_exit (-1);
			cur_page = page;
		}
		if (*p == '\0')
			return;
		p++;
	}
}

/* ====================================================================
 * sys_exit
 * ====================================================================
 * 다른 모듈도 호출할 수 있도록 외부 노출.
 * thread_exit() → process_exit()에서 종료 메시지/정리 처리. */
void
sys_exit (int status) {
	struct thread *curr = thread_current ();
	curr->exit_status = status;
	thread_exit ();
}

/* ====================================================================
 * 미구현 syscall stub
 * ====================================================================
 * 각 담당자는 본인 영역 함수를 정의·연결하면 된다.
 * stub은 호출 자체를 막지 않고 -1을 반환하여, 다른 테스트의 회귀를 방지. */

/* [A] halt: 정상 반환 없음. 미구현 동안에는 -1로 종료. */
static void
sys_halt (void) {
	/* TODO[A]: power_off() 호출. deadlock 상황등에 관한 일부 정보를 잃을 수 있으므로 이 호출은 드물게 사용해야함. 조건을 추가하라는 건가?*/
	//조건을 걸어야 될 것 같은데 뭘 걸어야 할지....
	//extern bool power_off_when_done;
	//void power_off (void) NO_RETURN;
	power_off();
}

/* [A] exec: 새 프로그램으로 치환. 성공하면 반환 없음. */
static int
sys_exec (const char *cmd_line UNUSED) {
	/* TODO[A]: process_exec 연결, 실패 시 -1 반환. */
	check_string(cmd_line);
	
	//palloc_get_page에 뭘 넣어야 할지 모르겠음. 그래서 임시로 0을 넣어놈.
	char *cmd_line_copy = palloc_get_page(0);
	if (cmd_line_copy == NULL)
		sys_exit(-1);

	strlcpy(cmd_line_copy, cmd_line, PGSIZE);

	if (process_exec(cmd_line_copy) == -1)
		sys_exit(-1);
	NOT_REACHED ();
}

/* [B] fork */
static int
sys_fork (const char *thread_name, struct intr_frame *if_) {
	/* TODO[B]: process_fork 연결. 부모 if_ 스냅샷 보관. */
	check_string (thread_name);
	return process_fork (thread_name, if_);
}

/* [B] wait */
static int
sys_wait (int pid) {
	/* TODO[B]: process_wait 연결. */
	return process_wait (pid);
}

/* [C] file meta */
static bool
sys_create_stub (const char *file, unsigned initial_size) {
	/* TODO[C] */
	check_string(file);

	lock_acquire(&filesys_lock);
	bool isCreated = filesys_create(file, initial_size);
	lock_release(&filesys_lock);

	return isCreated;
}

static bool
sys_remove_stub (const char *file) {
	/* TODO[C] */
	check_string(file);

	lock_acquire(&filesys_lock);
	bool isRemoved = filesys_remove(file);
	lock_release(&filesys_lock);

	return isRemoved;
}

static int
sys_open_stub (const char *file) {
	/* TODO[C] */
	check_string (file);

	lock_acquire(&filesys_lock);
	struct file *opened_file = filesys_open(file);
	lock_release(&filesys_lock);
	
	if (opened_file == NULL)
		return -1;
	
	int fd = fdt_add(opened_file);
	if (fd == -1){
		lock_acquire(&filesys_lock);
		file_close(opened_file);
		lock_release(&filesys_lock);
		return -1;
	}
	return fd;
}

static int
sys_filesize_stub (int fd) {
	/* TODO[C] */
	struct file *file = fdt_get(fd);
	if(file == NULL)
		return -1;
	
	lock_acquire(&filesys_lock);
	int leng = file_length(file);
	lock_release(&filesys_lock);

	return leng;
}

static void
sys_seek_stub (int fd, unsigned position) {
	/* TODO[C] */
	struct file *file = fdt_get(fd);
	if(file == NULL)
		return;
	
	lock_acquire(&filesys_lock);
	file_seek(file, position);
	lock_release(&filesys_lock);

}

static unsigned
sys_tell_stub (int fd) {
	/* TODO[C] */
	struct file *file = fdt_get(fd);
	if(file == NULL)
		return 0;
	lock_acquire(&filesys_lock);
	uint64_t result = file_tell(file);
	lock_release(&filesys_lock);
	return result;
}

static void
sys_close_stub (int fd) {
	/* TODO[C] */
	struct file *file = fdt_get(fd);
	if (file == NULL)
		return;
	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
	
	fdt_remove(fd); //파일 닫은 후에 fd table 칸 비우기

}	

/* [D] read/write
 *
 * NOTE: write(stdout)은 기존 코드가 args-* 테스트 통과를 위해 구현되어 있었음.
 * Phase 0에서는 그 동작을 보존(회귀 방지)하고, 본격 구현은 D가 같은 함수를
 * 확장하는 형태로 진행. fd!=1 또는 검증 강화는 D 담당. */

static int
sys_read_stub (int fd, void *buffer, unsigned int size) {
	/* TODO[D] */

	if (size <= 0)	//에러 처리
		return 0;
	if (fd == 0){	//키보드 입력
		check_buffer (buffer, size, true);
		for (unsigned int i = 0; i < size; i++){
			((uint8_t *)buffer)[i] = input_getc();
		}
		return size;
	}
	else if(fd >=2){
		check_buffer (buffer, size, true);
		lock_acquire(&filesys_lock);
		//파일 읽기
		struct file *file = fdt_get(fd);
		if(file == NULL){
			lock_release(&filesys_lock);
			return -1;
		}
		int result = file_read(file, buffer, size);
		lock_release(&filesys_lock);
		return result;
	}
	return -1;
}

static int
sys_write_partial (int fd, const void *buffer, unsigned int size) {
	/* args-* 회귀 보존을 위한 최소 구현. fd==1만 처리.
	 * D 담당이 본격 구현 시 buffer 검증 + filesys_lock + 일반 fd 분기 추가. */
	if (size <= 0)
		return 0;
	check_buffer (buffer, size, false);
	if (fd == 1 /* STDOUT_FILENO */) {

		putbuf (buffer, size);
		return (int) size;
	}
	else if(fd >=2){
		//파일 쓰기
		lock_acquire(&filesys_lock);

		struct file *file = fdt_get(fd);
		if (file == NULL){
			lock_release(&filesys_lock);
			return -1;
		}
		int result = file_write(file, buffer, size);
		lock_release(&filesys_lock);
		return result;
	}
	/* TODO[D]: 일반 파일 / stdin / 잘못된 fd 처리. */
	return -1;
}

/* ====================================================================
 * 디스패치
 * ====================================================================
 * x86-64 syscall 인자 순서: rdi, rsi, rdx, r10, r8, r9.
 * 반환값은 rax에 저장. */
void
syscall_handler (struct intr_frame *f) {
	uint64_t num = f->R.rax;
	uint64_t a1  = f->R.rdi;
	uint64_t a2  = f->R.rsi;
	uint64_t a3  = f->R.rdx;
	/* uint64_t a4 = f->R.r10;  (예약: 5/6번째 인자가 필요한 syscall이 생기면 사용) */

	switch (num) {
		/* --- A 담당 --- */
		case SYS_HALT:
			sys_halt ();
			break;
		case SYS_EXIT:
			sys_exit ((int) a1);
			break;
		case SYS_EXEC:
			f->R.rax = (uint64_t) sys_exec ((const char *) a1);
			break;

		/* --- B 담당 --- */
		case SYS_FORK:
			f->R.rax = (uint64_t) sys_fork ((const char *) a1, f);
			break;
		case SYS_WAIT:
			f->R.rax = (uint64_t) sys_wait ((int) a1);
			break;

		/* --- C 담당 --- */
		case SYS_CREATE:
			f->R.rax = (uint64_t) sys_create_stub ((const char *) a1,
													(unsigned) a2);
			break;
		case SYS_REMOVE:
			f->R.rax = (uint64_t) sys_remove_stub ((const char *) a1);
			break;
		case SYS_OPEN:
			f->R.rax = (uint64_t) sys_open_stub ((const char *) a1);
			break;
		case SYS_FILESIZE:
			f->R.rax = (uint64_t) sys_filesize_stub ((int) a1);
			break;
		case SYS_SEEK:
			sys_seek_stub ((int) a1, (unsigned) a2);
			break;
		case SYS_TELL:
			f->R.rax = (uint64_t) sys_tell_stub ((int) a1);
			break;
		case SYS_CLOSE:
			sys_close_stub ((int) a1);
			break;

		/* --- D 담당 --- */
		case SYS_READ:
			f->R.rax = (uint64_t) sys_read_stub ((int) a1, (void *) a2,
													(unsigned) a3);
			break;
		case SYS_WRITE:
			f->R.rax = (uint64_t) sys_write_partial ((int) a1,
													(const void *) a2,
													(unsigned) a3);
			break;

		/* 알 수 없는 syscall 번호: 비정상 종료. */
		default:
			sys_exit (-1);
			break;
	}
}

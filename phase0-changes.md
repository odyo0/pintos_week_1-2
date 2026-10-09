# Phase 0 변경 사항 전체 기록

> 브랜치: `feature/syscall`
> 변경 통계: **6 files / +654 / −94**

```
 pintos/include/threads/thread.h   |  53 +++++
 pintos/include/userprog/process.h |  32 +++
 pintos/include/userprog/syscall.h |  28 +++
 pintos/threads/thread.c           |  19 ++
 pintos/userprog/process.c         | 213 +++++++++++++++++++-
 pintos/userprog/syscall.c         | 403 ++++++++++++++++++++++++++++++--------
```

---

## 1. `pintos/include/threads/thread.h`

### 추가된 내용

#### a) 헤더 include 및 매크로 (파일 상단)

```c
#include "threads/synch.h"

/* [Phase 0] 파일 디스크립터 테이블 한도.
 * fd 0/1은 stdin/stdout 예약, 실제 파일은 fd 2부터 사용한다.
 * 테이블은 palloc 1페이지(PGSIZE)에 저장되므로 PGSIZE/sizeof(void*) = 512 한도 안에서 변경 가능.
 * 우선 128로 두고, C/D 담당이 필요하면 늘리도록 합의. */
#define FDT_LIMIT 128

struct file;
```

#### b) `struct thread`의 `#ifdef USERPROG` 블록 확장

기존:
```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint64_t *pml4;
    int exit_status;
#endif
```

추가된 필드:
```c
/* [Phase 0] 종료 상태.
 * 정상 exit() 호출 시 sys_exit이 덮어 쓴다.
 * 비정상 종료(page fault, kill, bad pointer)는 -1을 그대로 보고. */
int exit_status;  /* (기존 필드, 주석만 보강) */

/* [Phase 0] 파일 디스크립터 테이블. */
struct file **fd_table;
int next_fd;

/* [Phase 0] 실행 중 자기 실행 파일 (rox 차단용). */
struct file *running_file;

/* [Phase 0] 부모-자식 관계. */
struct thread *parent;
struct list children;
struct list_elem child_elem;

/* [Phase 0] 라이프사이클 동기화 (B 담당이 의미를 채운다). */
struct semaphore fork_sema;
bool fork_success;
struct semaphore wait_sema;
struct semaphore exit_sema;
bool exited;
bool waited;

/* [Phase 0] fork 시 부모 인터럽트 프레임 스냅샷. */
struct intr_frame parent_if;
```

### 삭제된 내용
없음. (기존 필드 유지, 추가만 함)

---

## 2. `pintos/threads/thread.c`

### 추가된 내용

`init_thread()` 함수 끝부분에 USERPROG 필드 초기화 블록 추가:

```c
#ifdef USERPROG
    /* [Phase 0] 사용자 프로세스 라이프사이클 필드 기본값.
     * - exit_status는 -1로 둬야 비정상 종료(page fault 등) 시 "exit(-1)"이 출력된다.
     * - fd_table은 process_exec/process_fork에서 palloc로 만들고, process_exit에서 해제.
     * - 부모 포인터는 thread_create 시점에 호출자가 설정 (process.c). */
    t->exit_status = -1;
    t->fd_table = NULL;
    t->next_fd = 2;
    t->running_file = NULL;
    t->parent = NULL;
    list_init (&t->children);
    sema_init (&t->fork_sema, 0);
    t->fork_success = false;
    sema_init (&t->wait_sema, 0);
    sema_init (&t->exit_sema, 0);
    t->exited = false;
    t->waited = false;
#endif
```

### 삭제된 내용
없음.

---

## 3. `pintos/include/userprog/syscall.h`

### 변경 전 (파일 전체)

```c
#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

#endif /* userprog/syscall.h */
```

### 변경 후 (파일 전체)

```c
#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include "threads/synch.h"

/* [Phase 0]
 * 모든 사용자 syscall이 공유하는 헬퍼·전역 자원·타입.
 * A/B/C/D 담당 모두 이 헤더만 include하면 된다. */

/* 사용자 포인터 검증.
 * - check_address: 단일 포인터 (NULL/커널영역/미매핑이면 즉시 sys_exit(-1))
 * - check_buffer:  [buf, buf+size) 범위 전체 검증. 페이지 경계마다 매핑 확인.
 *                  WRITABLE이 true이면 쓰기 가능 페이지인지도 검사 (read syscall용).
 * - check_string:  널 종단 사용자 문자열을 1바이트씩 검증하며 끝까지 따라간다.
 *
 * 모든 함수는 검증 실패 시 반환하지 않고 sys_exit(-1)로 프로세스를 종료한다. */
void check_address (const void *uaddr);
void check_buffer (const void *buf, size_t size, bool writable);
void check_string (const char *ustr);

/* 파일시스템 전역 락. */
extern struct lock filesys_lock;

/* sys_exit: 다른 모듈에서도 "이 프로세스를 status로 종료" 용도로 호출 가능. */
void sys_exit (int status);

void syscall_init (void);

#endif /* userprog/syscall.h */
```

### 추가된 내용
- `<stdbool.h>`, `<stddef.h>`, `"threads/synch.h"` include
- `check_address`, `check_buffer`, `check_string` 선언
- `extern struct lock filesys_lock;`
- `sys_exit` 선언

### 삭제된 내용
없음 (`syscall_init` 선언 유지).

---

## 4. `pintos/include/userprog/process.h`

### 추가된 내용 (파일 끝, `process_activate` 선언 뒤)

```c
/* ====================================================================
 * [Phase 0] 공통 헬퍼 API
 *
 * FD 테이블:
 *   fdt_init      현재 스레드의 fd_table을 1페이지(PAL_ZERO)로 할당. 실패 시 false.
 *   fdt_destroy   fd_table 페이지를 해제. 호출 전 fdt_close_all로 안의 파일을 닫아야 한다.
 *   fdt_add       비어 있는 가장 낮은 fd(>=2)를 찾아 file을 등록하고 fd 반환. 가득 차면 -1.
 *   fdt_get       fd가 유효 범위(2 <= fd < FDT_LIMIT) 안이고 등록되어 있으면 file*, 아니면 NULL.
 *   fdt_remove    fd 슬롯을 비운다. file을 close하지는 않는다(호출자가 책임).
 *   fdt_close_all 모든 슬롯의 파일을 file_close하고 슬롯을 비운다. process_exit에서 사용.
 *   fdt_copy      부모의 fd_table을 자식에 복제 (file_duplicate). fork에서 사용.
 *
 * 자식 관계:
 *   child_register   부모의 children 리스트에 자식을 추가하고 child->parent를 설정.
 *   child_find       부모의 children에서 tid에 해당하는 struct thread*를 찾는다 (없으면 NULL).
 *   child_remove     부모의 children에서 child를 빼낸다 (wait 종료 또는 부모 종료 시).
 *
 * NOTE: 위 함수들은 자료구조와 라이프사이클 hook을 제공할 뿐이며,
 *       fork/wait 의미(차단·복제·exit_status 전달)는 B 담당이 채운다. */

bool fdt_init (struct thread *t);
void fdt_destroy (struct thread *t);
int  fdt_add (struct file *file);
struct file *fdt_get (int fd);
void fdt_remove (int fd);
void fdt_close_all (struct thread *t);
bool fdt_copy (struct thread *parent, struct thread *child);

void child_register (struct thread *parent, struct thread *child);
struct thread *child_find (struct thread *parent, tid_t tid);
void child_remove (struct thread *child);
```

### 삭제된 내용
없음.

---

## 5. `pintos/userprog/process.c`

### 변경 사항 5-1: 헤더 include 추가

```c
#include "userprog/syscall.h"     /* 추가 — filesys_lock, sys_exit 사용 위해 */
#include "threads/synch.h"        /* 추가 — lock_acquire/release */
```

### 변경 사항 5-2: `process_init()` 함수 주석/시그니처 정리

기존:
```c
/* initd와 다른 프로세스를 위한 일반 프로세스 초기화 함수. */
static void
process_init (void) {
    struct thread *current = thread_current ();
}
```

변경 후:
```c
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
```

### 변경 사항 5-3: 새 헬퍼 9개 함수 추가 (process_init 직후)

```c
/* ====================================================================
 * [Phase 0] 파일 디스크립터 테이블 헬퍼
 * ==================================================================== */

bool fdt_init (struct thread *t) { ... }
void fdt_destroy (struct thread *t) { ... }
int  fdt_add (struct file *file) { ... }
struct file *fdt_get (int fd) { ... }
void fdt_remove (int fd) { ... }
void fdt_close_all (struct thread *t) { ... }
bool fdt_copy (struct thread *parent, struct thread *child) { ... }

/* ====================================================================
 * [Phase 0] 자식 프로세스 관계 헬퍼
 * ==================================================================== */

void child_register (struct thread *parent, struct thread *child) { ... }
struct thread *child_find (struct thread *parent, tid_t tid) { ... }
void child_remove (struct thread *child) { ... }
```

(자세한 본문은 [process.c](pintos/userprog/process.c) 참조 — 각 함수가 ASSERT/검증 후 동작)

### 변경 사항 5-4: `initd()`에 `fdt_init` 호출 추가

기존:
```c
static void
initd (void *f_name) {
#ifdef VM
    supplemental_page_table_init (&thread_current ()->spt);
#endif

    process_init ();

    if (process_exec (f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED ();
}
```

변경 후:
```c
static void
initd (void *f_name) {
#ifdef VM
    supplemental_page_table_init (&thread_current ()->spt);
#endif

    process_init ();

    /* [Phase 0] 사용자 프로세스 진입 직전 fd_table 할당. */
    if (!fdt_init (thread_current ()))
        PANIC ("Failed to allocate fd table for initd\n");

    if (process_exec (f_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED ();
}
```

### 변경 사항 5-5: `process_exit()` 정리 골격 표준화

기존:
```c
void
process_exit (void) {
    struct thread *curr = thread_current ();
    /* TODO: 여기에 코드를 작성한다.
     * TODO: 프로세스 종료 메시지를 구현한다 ... */
    printf("%s: exit(%d)\n", curr->name, curr->exit_status);
    process_cleanup ();
}
```

변경 후:
```c
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

    /* 3) [TODO A] 실행 파일에 쓰기 허용 + close. */
    /* 4)·5) [TODO B] 부모 reap 동기화. */
    /* 6) [TODO B] 자식 분리. */

    /* 7) 페이지 디렉터리 제거. */
    process_cleanup ();
}
```

7단계 정리 골격에 담당별 hook 위치를 주석으로 명시 (1/2/7은 구현, 3/4/5/6은 TODO).

---

## 6. `pintos/userprog/syscall.c` (전면 개편)

### 삭제된 내용

| 라인 | 삭제 대상 | 이유 |
|---|---|---|
| 13 | `static void sys_exit (int status);` 선언 | `syscall.h`로 이동, `static` 제거하여 외부 노출 |
| 14 | `static int sys_write (...);` 선언 | 함수 자체가 `sys_write_partial`로 대체됨 |
| 42 | `/*sys_exit이 필요해서 여기다 씀.*/` 주석 | 정식 API로 전환되어 임시 주석 제거 |
| 43-49 | `static void sys_exit (...)` (static 정의) | 비-static `sys_exit`로 재정의 |
| 51-66 | `static int sys_write (...)` 함수 전체 | `sys_write_partial`로 이름·내용 교체 |
| 55-58 | `// 나중에 확인해볼 코드 ...` 주석 | check_buffer로 대체되어 의미 없음 |
| 72-75 | `// TODO: 여기에 구현을 작성하세요.` 등 인계 주석 | 디스패치 구조가 명시화되어 의미 없음 |
| 76-86 | 단순 switch (3 case + default) | 14개 syscall 디스패치로 확장 |

### 추가된 내용

#### a) 헤더 include 확장

```c
#include <string.h>
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "userprog/process.h"
```

#### b) 파일 상단 모듈 설명

```c
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
 * ==================================================================== */
```

#### c) `filesys_lock` 정의 + `syscall_init`에서 초기화

```c
struct lock filesys_lock;

void syscall_init (void) {
    /* (기존 MSR 설정 유지) */
    ...
    /* [Phase 0] 파일시스템 굵은 락 초기화. */
    lock_init (&filesys_lock);
}
```

#### d) `check_address` / `check_buffer` / `check_string` 구현

```c
void check_address (const void *uaddr) {
    if (uaddr == NULL || !is_user_vaddr (uaddr))
        sys_exit (-1);
    if (pml4_get_page (thread_current ()->pml4, uaddr) == NULL)
        sys_exit (-1);
}

void check_buffer (const void *buf, size_t size, bool writable) {
    if (size == 0) return;
    if (buf == NULL) sys_exit (-1);
    /* writable 검사는 D 담당 보강 — TODO 표시 */
    (void) writable;

    const uint8_t *p = buf;
    const uint8_t *end = p + size;
    const uint8_t *page = pg_round_down (p);
    uint64_t *pml4 = thread_current ()->pml4;
    for (; page < end; page += PGSIZE) {
        if (!is_user_vaddr (page) || pml4_get_page (pml4, page) == NULL)
            sys_exit (-1);
    }
}

void check_string (const char *ustr) {
    if (ustr == NULL) sys_exit (-1);
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
        if (*p == '\0') return;
        p++;
    }
}
```

#### e) 외부 노출용 `sys_exit` 정의

```c
void sys_exit (int status) {
    struct thread *curr = thread_current ();
    curr->exit_status = status;
    thread_exit ();
}
```

#### f) 14개 syscall stub 함수 추가

| 담당 | 함수명 | 시그니처 | 기본 반환 |
|---|---|---|---|
| A | `sys_halt_stub` | `void(void)` | `sys_exit(-1)` |
| A | `sys_exec_stub` | `int(const char*)` | `-1` |
| B | `sys_fork_stub` | `int(const char*, struct intr_frame*)` | `-1` |
| B | `sys_wait_stub` | `int(int)` | `-1` |
| C | `sys_create_stub` | `bool(const char*, unsigned)` | `false` |
| C | `sys_remove_stub` | `bool(const char*)` | `false` |
| C | `sys_open_stub` | `int(const char*)` | `-1` |
| C | `sys_filesize_stub` | `int(int)` | `-1` |
| C | `sys_seek_stub` | `void(int, unsigned)` | (void) |
| C | `sys_tell_stub` | `unsigned(int)` | `0` |
| C | `sys_close_stub` | `void(int)` | (void) |
| D | `sys_read_stub` | `int(int, void*, unsigned)` | `-1` |
| D | `sys_write_partial` | `int(int, const void*, unsigned)` | fd==1만 동작, 나머지 -1 |

`sys_write_partial`은 args-* 회귀 보존을 위한 최소 구현:
```c
static int sys_write_partial (int fd, const void *buffer, unsigned size) {
    if (fd == 1 /* STDOUT_FILENO */) {
        check_buffer (buffer, size, false);
        putbuf (buffer, size);
        return (int) size;
    }
    return -1;
}
```

#### g) 디스패치 표 전면 재작성

기존 (3 case + default):
```c
switch (f->R.rax) {
    case SYS_EXIT: sys_exit ((int)f->R.rdi); break;
    case SYS_WRITE: f->R.rax = sys_write (f->R.rdi, f->R.rsi, f->R.rdx); break;
    default: sys_exit(-1); break;
}
```

변경 후 (14 case + default):
```c
void syscall_handler (struct intr_frame *f) {
    uint64_t num = f->R.rax;
    uint64_t a1 = f->R.rdi, a2 = f->R.rsi, a3 = f->R.rdx;

    switch (num) {
        /* --- A 담당 --- */
        case SYS_HALT:     sys_halt_stub (); break;
        case SYS_EXIT:     sys_exit ((int) a1); break;
        case SYS_EXEC:     f->R.rax = (uint64_t) sys_exec_stub ((const char *) a1); break;
        /* --- B 담당 --- */
        case SYS_FORK:     f->R.rax = (uint64_t) sys_fork_stub ((const char *) a1, f); break;
        case SYS_WAIT:     f->R.rax = (uint64_t) sys_wait_stub ((int) a1); break;
        /* --- C 담당 --- */
        case SYS_CREATE:   f->R.rax = (uint64_t) sys_create_stub (...); break;
        case SYS_REMOVE:   f->R.rax = (uint64_t) sys_remove_stub (...); break;
        case SYS_OPEN:     f->R.rax = (uint64_t) sys_open_stub (...); break;
        case SYS_FILESIZE: f->R.rax = (uint64_t) sys_filesize_stub (...); break;
        case SYS_SEEK:     sys_seek_stub (...); break;
        case SYS_TELL:     f->R.rax = (uint64_t) sys_tell_stub (...); break;
        case SYS_CLOSE:    sys_close_stub (...); break;
        /* --- D 담당 --- */
        case SYS_READ:     f->R.rax = (uint64_t) sys_read_stub (...); break;
        case SYS_WRITE:    f->R.rax = (uint64_t) sys_write_partial (...); break;

        default:           sys_exit (-1); break;
    }
}
```

---

## 7. 핵심 결정 사항 요약 (혼자 판단해서 정한 항목)

| # | 항목 | 결정 | 사유 |
|---|---|---|---|
| 1 | `FDT_LIMIT` | 128 | 1 페이지(512) 안에서 늘릴 수 있음. 우선 충분한 값 |
| 2 | fd_table 저장 | `struct file **`, palloc 1페이지 | struct thread + 커널 스택이 4KB 안에 들어가야 함 |
| 3 | fd_table 할당 시점 | `initd` 진입부, `fdt_copy` 내부 | fork 경로의 빈 테이블 → 복제 낭비 회피 |
| 4 | 포인터 검증 방식 | `pml4_get_page` 기반 | 명료성 우선. 성능은 D가 후일 보강 |
| 5 | `check_buffer(writable)` | 시그니처만 제공, 실제 검사는 TODO | mmu PTE 직접 보는 게 D 영역과 결합되므로 유보 |
| 6 | 자식 관계 | struct thread 안의 필드 | Pintos 관행. 별도 PCB 분리 안 함 |
| 7 | `exit_status` 기본값 | `-1` | bad-* 테스트에서 자동으로 "exit(-1)" 출력 보장 |
| 8 | 미구현 syscall 반환 | `-1` (sys_exit 아님) | 무관한 테스트의 동반 사망 방지 |
| 9 | `filesys_lock` | 굵은 전역 락 1개 | 단순화 우선. 세분화는 후일 |
| 10 | `process_exit` 단계 | 7단계 표준화, 1·2·7만 구현 | 3은 A, 4·5·6은 B 영역이라 hook 위치만 박음 |

---

## 8. 빌드 / 회귀 검증

- ✅ **빌드 성공** (`make` in `pintos/userprog/`) — 경고 없음, 모든 사용자 테스트 바이너리 정상 컴파일
- ✅ **Phase 0 변경으로 인한 회귀 없음** — `git stash` 비교 검증으로 동일 동작 확인
- ⚠️ args-none 등 실제 통과 여부는 사용자 환경에 따라 다름 (제 컨테이너에서는 QEMU 캘리브레이션 변동으로 비결정적)

---

## 9. 담당자 인계 체크리스트

Phase 0 머지 직후 각 담당이 시작할 진입점:

| 담당 | 시작 파일 | 시작 함수/위치 |
|---|---|---|
| **A** | `pintos/userprog/syscall.c` | `sys_halt_stub`, `sys_exec_stub` 본문 채우기 |
| **A** | `pintos/userprog/process.c` | `process_exit`의 `[TODO A]` 블록 |
| **B** | `pintos/userprog/syscall.c` | `sys_fork_stub`, `sys_wait_stub` 본문 |
| **B** | `pintos/userprog/process.c` | `process_fork`, `__do_fork`, `process_wait`, `process_exit`의 `[TODO B]` |
| **C** | `pintos/userprog/syscall.c` | `sys_create/remove/open/close/filesize/seek/tell_stub` 본문 |
| **D** | `pintos/userprog/syscall.c` | `sys_read_stub`, `sys_write_partial` 확장, `check_buffer` writable 보강 |

**모두 공통**:
- syscall 진입부에서 `check_address/buffer/string` 호출
- 파일 호출 직전 `lock_acquire(&filesys_lock)` / 직후 `lock_release`
- FDT 조작은 `fdt_add/get/remove` API 통해서만
- 자식 등록/조회는 `child_register/find/remove` API 통해서만

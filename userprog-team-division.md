# Pintos userprog 테스트 분업 계획

> 대상: `pintos/tests/userprog/` 하위 테스트 중 `args-none.ck`, `args-single.ck`, `args-multiple.ck`, `args-many.ck`, `args-dbl-space.ck`를 **제외**한 나머지

---

## 1. 큰 틀 — 테스트 5개 카테고리 분류

테스트들을 **syscall 카테고리 기준**으로 분류했다. 각 팀원이 맡은 syscall 구현과 테스트가 1:1로 매칭되도록 하기 위함.

### 1) 프로세스 제어 (halt / exit / exec / wait) — 11개
- `halt.c`, `exit.c`
- `exec-*`: arg, bad-ptr, boundary, missing, once, read
- `wait-*`: bad-pid, killed, simple, twice

### 2) fork & 멀티프로세스 — 8개
- `fork-*`: boundary, close, multiple, once, read, recursive
- `multi-*`: child-fd, recurse

### 3) 파일 메타 syscall (create / open / close) — 17개
- `create-*`: bad-ptr, bound, empty, exists, long, normal, null
- `open-*`: bad-ptr, boundary, empty, missing, normal, null, twice
- `close-*`: bad-fd, normal, twice

### 4) I/O syscall (read / write) — 12개
- `read-*`: bad-fd, bad-ptr, boundary, normal, stdout, zero
- `write-*`: bad-fd, bad-ptr, boundary, normal, stdin, zero

### 5) 견고성 / 메모리 보호 (rox / bad pointer) — 9개
- `bad-jump`, `bad-jump2`, `bad-read`, `bad-read2`, `bad-write`, `bad-write2`
- `rox-*`: simple, child, multichild

---

## 2. 종속성 분석

병렬 작업 100% 가능 여부: **불가**. 강한 종속성이 있다.

### 종속 다이어그램

```
[1] 프로세스 제어 ──── wait ←──→ fork ──── [2] fork
        │                                      │
        ↓ exit 시 정리                          ↓ FD 복제
        └──→ FD 테이블 ←──────────────────────┘
                  ↑                            ↑
                  │                            │
            [3] create/open/close   ←──→  [4] read/write
                  │                            │
                  └──── 포인터 검증 ────────────┘
                              ↑
                        [5] 견고성/rox
```

### 강결합 지점

- **wait ↔ fork**: wait는 자식 PCB / 세마포어 없이는 구현 불가. 1·2번은 사실상 한 명이 같이 보거나, 자식 PCB 구조를 먼저 합의해야 함.
- **open/close ↔ read/write**: 같은 FD 테이블을 만진다. 3·4번은 FD 자료구조가 합의되지 않으면 동시 작업 불가.
- **fork ↔ open/close**: fork할 때 부모 FD를 자식에 복제(`file_duplicate`)해야 함. 2번이 3번 결과물에 의존.
- **rox ↔ exec**: `file_deny_write` 호출 위치(load 시점)가 exec 구현에 들어감. 5번 rox는 1번 exec에 의존.
- **모든 bad-* / 잘못된 포인터 테스트**: `check_address` 한 함수가 모든 syscall에 박혀야 통과.

---

## 3. Phase 0 — 전원 합의 사항

병렬 작업의 전제 조건. 팀이 **함께** 결정하고 머지한다.

### 3.1 포인터 검증 함수 시그니처
- `check_address(void *)`, `check_buffer(void *, size_t)`, `check_string(const char *)` 형태
- NULL / 커널 영역 / 매핑 안 된 페이지 처리 방식
- 잘못된 포인터일 때 `exit(-1)` 호출인지, page fault 핸들러에서 잡을지

### 3.2 FD 테이블 구조
- `struct thread`(또는 PCB)의 어디에, 배열인지 리스트인지, 크기, 0/1은 stdin/stdout 예약
- `add_file_to_fd_table()`, `fd_to_file()`, `remove_fd()` API 이름

### 3.3 자식 프로세스 PCB 구조
- 자식 리스트, `sema_load`(load 성공/실패 동기화), `sema_wait`(종료 대기), `exit_status`, `is_waited`
- fork도 이 구조 위에 올라감

### 3.4 filesys_lock
- 전역 lock 하나로 갈지

### 3.5 process_exit 정리 책임 범위
- FD 전부 close, 자식 정리, exit_status 부모에 전달, `file_allow_write` 등

---

## 4. 4인 분업표 (Phase 0 제외)

| 담당 | 영역 | syscall | 테스트 수 | 난이도 | 볼륨 |
|---|---|---|---|---|---|
| **A** | 프로세스 진입/종료 + 메모리 보호 | `halt`, `exit`, `exec` | 14 | ★★★ | 중 |
| **B** | 자식 프로세스 + 견고성 | `fork`, `wait` | 15 | ★★★★★ | 큼 |
| **C** | 파일 메타 | `create`, `remove`, `open`, `close`, `filesize`, `seek`, `tell` | 17 | ★★ | 큼 |
| **D** | I/O | `read`, `write` | 12 | ★★★ | 중 |

---

### A — 프로세스 진입/종료 + bad-pointer
**브랜치**: `feat/process-control`

- **syscall**: `halt`, `exit`, `exec`
- **테스트** (14):
  - `halt`, `exit`
  - `exec-arg`, `exec-bad-ptr`, `exec-boundary`, `exec-missing`, `exec-once`, `exec-read` (6)
  - `bad-jump`, `bad-jump2`, `bad-read`, `bad-read2`, `bad-write`, `bad-write2` (6)
- **난이도** ★★★ — halt/exit는 쉬움. exec(`process_exec` + 인자 전달)이 중심. `bad-*`는 잘못된 사용자 주소로 점프/접근 → page fault → `exit(-1)` 흐름이라 A의 exit 정리 로직과 같은 결.
- **볼륨** 중 — 테스트 14개

### B — fork + wait + multi + rox
**브랜치**: `feat/fork-wait`

- **syscall**: `fork`, `wait`
- **테스트** (15):
  - `wait-bad-pid`, `wait-killed`, `wait-simple`, `wait-twice` (4)
  - `fork-boundary`, `fork-close`, `fork-multiple`, `fork-once`, `fork-read`, `fork-recursive` (6)
  - `multi-child-fd`, `multi-recurse` (2)
  - `rox-simple`, `rox-child`, `rox-multichild` (3)
- **난이도** ★★★★★ — 페이지 테이블 복사(`duplicate_pte`), FD 복제, 부모/자식 양쪽 반환값, `sema_load`/`sema_wait` sync. **가장 시간 많이 듦**.
- **볼륨** 큼 — rox는 `file_deny_write`(A의 exec와 협업) + 자식 sync(본인 영역)라 B로 묶음.

### C — 파일 메타 syscall
**브랜치**: `feat/file-meta`

- **syscall**: `create`, `remove`, `open`, `close`, `filesize`, `seek`, `tell`
- **테스트** (17):
  - `create-bad-ptr`, `create-bound`, `create-empty`, `create-exists`, `create-long`, `create-normal`, `create-null` (7)
  - `open-bad-ptr`, `open-boundary`, `open-empty`, `open-missing`, `open-normal`, `open-null`, `open-twice` (7)
  - `close-bad-fd`, `close-normal`, `close-twice` (3)
- **난이도** ★★ — `filesys_create`/`filesys_open` 호출 + FD 테이블 관리. 로직 단순.
- **볼륨** 큼 — 테스트 수 최다. **두 번째로 빨리 머지돼야** B(fork-close, multi-child-fd)와 D(read/write)가 풀림.

### D — I/O syscall
**브랜치**: `feat/file-io`

- **syscall**: `read`, `write`
- **테스트** (12):
  - `read-bad-fd`, `read-bad-ptr`, `read-boundary`, `read-normal`, `read-stdout`, `read-zero` (6)
  - `write-bad-fd`, `write-bad-ptr`, `write-boundary`, `write-normal`, `write-stdin`, `write-zero` (6)
- **난이도** ★★★ — 본체는 단순(`file_read`/`file_write`)하지만 `boundary`(페이지 경계 버퍼), `bad-ptr`(다중 페이지 검증), stdin/stdout 특수 케이스가 까다로움.
- **볼륨** 중 — 테스트 12개

---

## 5. 머지 순서

```
[Phase 0 공동 인프라 머지 완료]
        │
        ├──→ A (exec, exit) ────────────┐
        │                                │
        ├──→ C (FD 테이블) ──┬──→ D (I/O) │
        │                   │            │
        │                   └──→ B (fork, FD 복제) ──→ B (rox: A 머지 후)
```

### 현실적 진행 가이드

1. **A와 C가 먼저 빠르게 1차 PR** → 나머지 두 명 unblock
2. **D는 C 머지 후 본격 작업** (그 전엔 stub)
3. **B는 fork 자체에 시간이 가장 많이 들므로 처음부터 시작**하되, FD 복제·rox는 C/A 머지 이후

---

## 6. 팀 간 싱크 포인트

| 조합 | 공유 자원 | 합의 사항 |
|---|---|---|
| **A ↔ B** | `process_exit` 정리 로직 | 누가 자식 정리, 누가 exit_status 전달 |
| **A ↔ B** | rox (`file_deny_write`) | A가 exec/load에서 deny, B가 rox 테스트 검증 |
| **B ↔ C** | fork 시 FD 복제 (`file_duplicate`) | C가 `fd_table_copy()` API 제공, B가 호출 |
| **C ↔ D** | FD 테이블 + `filesys_lock` | C가 `fd_to_file()` API 확정, D가 read/write에서 호출 |
| **A ↔ D** | `bad-*` vs 사용자 버퍼 검증 | page fault 처리(A) / syscall 진입 검증(D) 경계 명확화 |

---

## 7. 요약

- **카테고리 5개** → 자연스러운 syscall 단위 분류
- **Phase 0 공동 작업** → 포인터 검증 / FD 테이블 / 자식 PCB / filesys_lock / process_exit 합의
- **4인 분업** → A(프로세스+bad), B(fork+wait+rox), C(파일 메타), D(I/O)
- **브랜치** → `feat/process-control`, `feat/fork-wait`, `feat/file-meta`, `feat/file-io`
- **머지 우선순위** → A·C 먼저 → D → B 순으로 의존성 해소
- **싱크 핵심** → A·B(process_exit, rox), B·C(FD 복제), C·D(FD 테이블), A·D(포인터 검증 경계)

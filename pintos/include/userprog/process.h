#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

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

#endif /* userprog/process.h */

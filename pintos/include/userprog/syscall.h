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

/* 파일시스템 전역 락.
 * 모든 파일 syscall(create/remove/open/close/read/write/seek/tell/filesize/exec load 등)은
 * 이 락을 획득한 뒤 filesys_* / file_* 호출. 단순화를 위해 굵은(global) 락 한 개로 통일. */
extern struct lock filesys_lock;

/* sys_exit: 다른 모듈에서도 "이 프로세스를 status로 종료" 용도로 호출 가능하게 노출.
 * (check_address 실패, syscall 진입 검증 실패 등) */
void sys_exit (int status);

void syscall_init (void);

#endif /* userprog/syscall.h */

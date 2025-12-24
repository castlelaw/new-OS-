#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h" 
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "console.h"
#include "threads/pte.h"

// 페이지 크기 (4KB) 정의
#define PGSIZE 0x1000 

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

/* 함수 선언 */
// 아래 두 헬퍼 함수 선언 추가
static void check_user_vaddr (const void *vaddr);
static void check_user_buffer (const void *buffer, unsigned size);
static void check_user_string (const char *str);

/* 파일 시스템 접근을 위한 락. (filesys 디렉터리가 스레드 안전하지 않으므로 필요) */
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

/* 공통 종료 헬퍼: exit_status 설정 + thread_exit */
static void
exit_process (int status)
{
  struct thread *cur = thread_current ();
  cur->exit_status = status;
  thread_exit ();
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock); // 파일 시스템 락 초기화
}

/* * 유효하지 않은 사용자 가상 주소에 접근 시 현재 프로세스를 종료하는 헬퍼 함수
 * 단일 주소 vaddr이 유효한 사용자 주소 공간 내에 있고, 페이지 테이블에 매핑되어 있는지 확인.
 */
static void
check_user_vaddr (const void *vaddr)
{
  struct thread *cur = thread_current();
  
  // 널 포인터이거나, 커널 가상 주소 공간에 있거나, 유효한 사용자 주소가 아닌 경우
  if (vaddr == NULL || !is_user_vaddr(vaddr))
    exit_process(-1);
  
  // 주소가 페이지 테이블에 매핑되어 있지 않은 경우 (페이지 폴트 방지)
  if(pagedir_get_page (cur->pagedir, vaddr) == NULL)
    exit_process(-1);
}

// 버퍼 전체의 유효성 검사를 수행하는 함수
/* * 버퍼 시작 주소부터 size만큼의 전체 주소 범위가 유효한지 확인.
 * 버퍼가 걸쳐 있는 모든 페이지의 시작 주소를 검사합니다.
 */
static void
check_user_buffer (const void *buffer, unsigned size)
{
  if (buffer == NULL)
    exit_process(-1);

  /* size==0이면 시작 주소만이라도 유효한지 확인 */
  if (size == 0) {
    check_user_vaddr(buffer);
    return;
  }

  const uint8_t *start = (const uint8_t *) buffer;
  const uint8_t *end   = start + size - 1;

  /* overflow 방지: end가 start보다 작아지면 wrap-around */
  if (end < start)
    exit_process(-1);

  /* start가 속한 페이지부터 end가 속한 페이지까지 페이지 단위로 검사 */
  for (uint8_t *p = pg_round_down((void *) start);
       p <= (uint8_t *) end;
       p += PGSIZE)
  {
    check_user_vaddr(p);
  }
}
// 널 종료 문자열 전체의 유효성 검사를 수행하는 함수
/*
 * 널 종료 문자열 str의 모든 문자가 유효한 사용자 주소 공간에 매핑되어 있는지 확인.
 */
static void
check_user_string (const char *str)
{
  if (str == NULL)
    exit_process(-1);
    
  while (true)
  {
    // 현재 문자의 유효성 검사 (매핑 여부 확인)
    check_user_vaddr (str);
    
    // 널 종료 문자를 찾으면 종료
    if (*str == '\0')
      break;
      
    str++;
  }
}

static void
syscall_handler (struct intr_frame *f)
{
  int *esp = f->esp;

 
  check_user_buffer(esp, 4);

  int syscall_no = esp[0];

  int status;
  const char *cmd_line;

  switch (syscall_no)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      /* 인자 1: status = esp[1] */
      check_user_vaddr (&esp[1]);
      status = esp[1];
      if (thread_current()->is_user_process)
        printf("%s: exit(%d)\n", thread_current()->name, status);
      exit_process (status);
      break;

    case SYS_EXEC:
      /* 인자 1: cmd_line 포인터 = esp[1] */
      check_user_vaddr (&esp[1]);             /* 포인터가 있는 곳 */
      cmd_line = (const char *) esp[1];       /* 실제 문자열 주소 */
      
      // [수정] check_user_vaddr (cmd_line); 대신 문자열 전체 검사 함수 사용
      /* 문자열 전체의 유효성 검사 */
      check_user_string (cmd_line);

      lock_acquire (&filesys_lock);
      f->eax = process_execute (cmd_line);    /* 자식 tid */
      lock_release (&filesys_lock);
      break;

    case SYS_WRITE:
      {
        /* 인자 1,2,3: fd, buffer, size */
        check_user_vaddr (&esp[1]);      /* fd 가 올라있는 주소 */
        check_user_vaddr (&esp[2]);      /* buffer 포인터가 있는 주소 */
        check_user_vaddr (&esp[3]);      /* size 값이 있는 주소 */

        int fd = esp[1];
        const void *buffer = (const void *) esp[2];
        unsigned size = (unsigned) esp[3];

        // 버퍼의 시작/끝만 검사하는 대신, 버퍼 전체 페이지 검사 함수 사용
        /* 버퍼 전체의 유효성 검사: 시작부터 끝까지 매핑된 페이지 확인 */
        check_user_buffer (buffer, size);

        if (fd == STDOUT_FILENO)   /* 1 */
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else
          {
            /* 파일 시스템 관련 처리가 필요. (현재는 지원하지 않음) */
            f->eax = -1;
          }
      }
      break;

    default:
      exit_process (-1);
      break;
    }
}

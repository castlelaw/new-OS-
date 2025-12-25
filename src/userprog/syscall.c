#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "console.h"
#include "userprog/process.h"
#include "filesys/filesys.h"

/* [P2-1 FIX] 전역 락 정의 (process.c와 공유) */
struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

/* 유저 포인터 검증 함수들 */
static void check_user_vaddr (const void *uaddr);
static void check_user_buffer (const void *buffer, unsigned size);
static void check_user_string (const char *str);

/* 유저 스택에서 인자 가져오기 */
static int32_t get_user_i32 (const void *uaddr);
static void *get_user_ptr (const void *uaddr);

/* 전방 선언 */
void exit (int status);

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* 주소 유효성 검사: 유저 영역인지 + 매핑된 페이지인지 */
static void
check_user_vaddr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    {
      exit (-1);
    }

  if (pagedir_get_page (thread_current ()->pagedir, uaddr) == NULL)
    {
      exit (-1);
    }
}

/* 버퍼 전체 영역 검사 (페이지 경계 포함) */
static void
check_user_buffer (const void *buffer, unsigned size)
{
  if (size == 0)
    return;

  const uint8_t *start = (const uint8_t *) buffer;
  const uint8_t *end = start + size - 1;

  check_user_vaddr (start);

  for (const uint8_t *p = start; p <= end; p = (const uint8_t *) pg_round_down (p + PGSIZE))
    {
       check_user_vaddr (p); 
    }
    
  check_user_vaddr (end);
}

/* 문자열 검사 (NULL 만날 때까지) */
static void
check_user_string (const char *str)
{
  check_user_vaddr (str);

  for (const char *p = str; ; p++)
    {
      check_user_vaddr (p);
      if (*p == '\0')
        break;
    }
}

static int32_t
get_user_i32 (const void *uaddr)
{
  check_user_buffer (uaddr, sizeof (int32_t));
  return *(const int32_t *) uaddr;
}

static void *
get_user_ptr (const void *uaddr)
{
  check_user_buffer (uaddr, sizeof (void *));
  return *(void * const *) uaddr;
}

/* syscall.c 내에서 사용할 간편 exit 함수 */
void
exit (int status)
{
  struct thread *cur = thread_current ();
  /* [FIX] cur->exited 제거됨 */
  cur->exit_status = status;
  printf ("%s: exit(%d)\n", cur->name, status);
  thread_exit ();
}

static void
syscall_handler (struct intr_frame *f)
{
  /* 스택 포인터(esp) 자체의 유효성 검사 */
  check_user_vaddr (f->esp);

  int syscall_no = get_user_i32 (f->esp);

  switch (syscall_no)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();

    case SYS_EXIT:
      {
        int status = get_user_i32 ((uint8_t *) f->esp + 4);
        exit (status);
        NOT_REACHED ();
      }

    case SYS_WRITE:
      {
        int fd = get_user_i32 ((uint8_t *) f->esp + 4);
        const void *buf = get_user_ptr ((uint8_t *) f->esp + 8);
        unsigned size = (unsigned) get_user_i32 ((uint8_t *) f->esp + 12);

        check_user_buffer (buf, size);

        if (fd == 1)
          {
            putbuf (buf, size);
            f->eax = (int) size;
          }
        else
          {
            f->eax = -1; 
          }
        break;
      }

    case SYS_EXEC:
      {
        const char *cmd_line = get_user_ptr ((uint8_t *) f->esp + 4);
        check_user_string (cmd_line);

        /* filesys_lock 없이 호출 (deadlock 방지) */
        tid_t tid = process_execute (cmd_line);
        
        f->eax = (tid == TID_ERROR) ? -1 : (int) tid;
        break;
      }

    case SYS_WAIT:
      {
        tid_t tid = (tid_t) get_user_i32 ((uint8_t *) f->esp + 4);
        f->eax = process_wait (tid);
        break;
      }

    default:
      exit (-1);
    }
}
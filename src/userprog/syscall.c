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

  /* 시작 주소 검사 */
  check_user_vaddr (start);

  /* 페이지 경계마다 검사 */
  for (const uint8_t *p = start; p <= end; p = (const uint8_t *) pg_round_down (p + PGSIZE))
    {
       /* 루프 시작부에서 p를 검사하지 않는 이유는 
          pg_round_down 로직상 start가 포함된 페이지의 다음 페이지부터 검사하기 위함 
          혹은 start 자체를 이미 검사했으므로 중복 최소화 */
       check_user_vaddr (p); 
    }
    
  /* 마지막 주소 검사 */
  check_user_vaddr (end);
}

/* 문자열 검사 (NULL 만날 때까지) */
static void
check_user_string (const char *str)
{
  /* 문자열 시작 주소 1차 검증 */
  check_user_vaddr (str);

  for (const char *p = str; ; p++)
    {
      /* 매 바이트마다 검사하는 것은 비효율적일 수 있으나 가장 안전함.
         페이지 경계만 검사하도록 최적화 가능하지만 P2-1에서는 안전제일. */
      check_user_vaddr (p);
      if (*p == '\0')
        break;
    }
}

/* 헬퍼: 4바이트 정수 읽기 */
static int32_t
get_user_i32 (const void *uaddr)
{
  check_user_buffer (uaddr, sizeof (int32_t));
  return *(const int32_t *) uaddr;
}

/* 헬퍼: 포인터(주소값) 읽기 */
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
  cur->exited = true;
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
            /* P2-1에서는 fd 1(stdout)만 처리. 나머지는 무시하거나 에러 */
            f->eax = -1; // 혹은 0
          }
        break;
      }

    case SYS_EXEC:
      {
        const char *cmd_line = get_user_ptr ((uint8_t *) f->esp + 4);
        check_user_string (cmd_line);

        /* [CRITICAL FIX]
           여기서 lock_acquire(&filesys_lock)을 하면 안 됨!
           process_execute() -> sema_down()으로 자식을 기다리는데,
           자식은 load() -> lock_acquire()를 시도하므로 Deadlock 발생함.
           
           process_execute 내부 로직과 process.c의 load 함수가 
           이미 동기화 처리를 하고 있으므로 바로 호출.
        */
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
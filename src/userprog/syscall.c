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

struct lock filesys_lock;        /* [P2-1 FIX] static 제거: process.c와 공유 */

static void syscall_handler (struct intr_frame *);

/* 유저 포인터 검증 */
static void check_user_vaddr (const void *uaddr);
static void check_user_buffer (const void *buffer, unsigned size);
static void check_user_string (const char *str);

static int32_t get_user_i32 (const void *uaddr);
static void *get_user_ptr (const void *uaddr);

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
check_user_vaddr (const void *uaddr)
{
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    {
      struct thread *cur = thread_current ();
      cur->exited = true;
      cur->exit_status = -1;
      thread_exit ();
    }

  if (pagedir_get_page (thread_current ()->pagedir, uaddr) == NULL)
    {
      struct thread *cur = thread_current ();
      cur->exited = true;
      cur->exit_status = -1;
      thread_exit ();
    }
}

static void
check_user_buffer (const void *buffer, unsigned size)
{
  if (size == 0)
    return;

  const uint8_t *start = buffer;
  const uint8_t *end = start + size - 1;

  /* [P2-1 FIX] 중간 페이지까지 전부 검사 */
  for (const uint8_t *p = start; p <= end; p = (const uint8_t *) pg_round_down (p + PGSIZE))
    check_user_vaddr (p);

  check_user_vaddr (end);
}

static void
check_user_string (const char *str)
{
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

static void
syscall_handler (struct intr_frame *f)
{
  int syscall_no = get_user_i32 (f->esp);

  switch (syscall_no)
    {
    case SYS_HALT:
      shutdown_power_off ();
      NOT_REACHED ();

    case SYS_EXIT:
      {
        int status = get_user_i32 ((uint8_t *) f->esp + 4);
        struct thread *cur = thread_current ();
        cur->exited = true;
        cur->exit_status = status;
        thread_exit ();
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
          f->eax = -1;
        break;
      }

    case SYS_EXEC:
      {
        const char *cmd_line = get_user_ptr ((uint8_t *) f->esp + 4);
        check_user_string (cmd_line);

        /* [P2-1 FIX] process_execute가 load 동기화 후 성공/실패를 TID로 준다 */
        lock_acquire (&filesys_lock);
        tid_t tid = process_execute (cmd_line);
        lock_release (&filesys_lock);

        f->eax = (tid == TID_ERROR) ? -1 : (int) tid;
        break;
      }

    default:
      {
        struct thread *cur = thread_current ();
        cur->exited = true;
        cur->exit_status = -1;
        thread_exit ();
        NOT_REACHED ();
      }
    }
}

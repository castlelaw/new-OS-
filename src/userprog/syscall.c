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

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

static void check_user_vaddr (const void *vaddr);

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

/* * 유효하지 않은 사용자 가상 주소에 접근 시 현재 프로세스를 종료하는 헬퍼 함수*/
static void
check_user_vaddr (const void *vaddr)
{
  struct thread *cur = thread_current();
  // 널 포인터이거나, 커널 가상 주소 공간에 있거나, 유효한 사용자 주소가 아닌 경우
  if (vaddr == NULL || !is_user_vaddr(vaddr))
    exit_process(-1);
  if(pagedir_get_page (cur->pagedir, vaddr) == NULL)
    exit_process(-1);

}

static void
syscall_handler (struct intr_frame *f)
{
  int *esp = f->esp;   /* 유저 스택 포인터 */
  int syscall_no;

  /* syscall 번호가 있는 주소부터 유효성 검사 */
  check_user_vaddr (esp);
  syscall_no = esp[0];

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
      exit_process (status);
      break;

    case SYS_EXEC:
      /* 인자 1: cmd_line 포인터 = esp[1] */
      check_user_vaddr (&esp[1]);                /* 포인터가 있는 곳 */
      cmd_line = (const char *) esp[1];          /* 실제 문자열 주소 */
      check_user_vaddr (cmd_line);               /* 문자열 시작 주소 */

      lock_acquire (&filesys_lock);
      f->eax = process_execute (cmd_line);       /* 자식 tid */
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

        /* 버퍼의 앞/뒤만 확인 (너무 과하게 검사 X) */
        if (buffer == NULL)
          exit_process (-1);

        check_user_vaddr (buffer);                 /* 첫 바이트 */
        if (size > 0)
          check_user_vaddr ((const uint8_t *)buffer + size - 1);  /* 마지막 바이트 */

        if (fd == 1)       /* STDOUT_FILENO */
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else
          {
            /* 아직 다른 fd 는 지원 안 함: 0 반환 정도로 충분 */
            f->eax = 0;
          }
      }
      break;

    default:
      exit_process (-1);
      break;
    }
}

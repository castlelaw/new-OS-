#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user programs. */
void
exception_init (void)
{
  /* Exceptions that are fatal to the user process. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill, "#BR BOUND Range Exceeded Exception");

  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill, "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill, "#XF SIMD Floating-Point Exception");

  /* Page fault exception. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f)
{
  /* The interrupt frame's code segment indicates whether the
     exception was caused by a user process or the kernel. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      {
        struct thread *cur = thread_current ();
        cur->exited = true;
        cur->exit_status = -1;
        /* process_exit()에서 출력하도록 위임 */
        printf ("%s: exit(%d)\n", cur->name, -1);
        thread_exit ();
      }

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they're handled
         elsewhere.) */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen. */
      PANIC ("Interrupt in unknown segment");
    }
}

/* Page fault handler. */
static void
page_fault (struct intr_frame *f)
{
  bool not_present;  /* True: not-present page, False: writing r/o page. */
  bool write;        /* True: access was write, False: access was read. */
  bool user;         /* True: access by user, False: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip). */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* Project 2-1:
     가상 메모리(Swap, Stack Growth) 구현 전이므로,
     User Mode에서 발생한 Page Fault는 모두 잘못된 메모리 접근입니다.
     따라서 즉시 프로세스를 종료합니다. */

  if (user)
    {
      struct thread *cur = thread_current ();
      cur->exited = true;
      cur->exit_status = -1;
      printf ("%s: exit(%d)\n", cur->name, -1);
      thread_exit ();
    }

  /* 커널 모드에서의 Page Fault 발생 시
     Project 2-1에서는 syscall.c에서 포인터를 미리 검증하므로
     이곳에 도달했다면 커널 버그이거나 검증 누락입니다. */
  
  /* 디버깅 정보를 출력하고 커널 패닉 */
  printf ("Page fault at %p: %s error %s page in %s context.\n",
          fault_addr,
          not_present ? "not present" : "rights violation",
          write ? "writing" : "reading",
          user ? "user" : "kernel");
  kill (f);
}
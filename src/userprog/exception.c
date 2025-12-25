#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

void
exception_init (void)
{
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

  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

static void
kill (struct intr_frame *f)
{
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* [P2-1 FIX] 유저 예외는 출력 없이 exit(-1)로 종료 */
      {
        struct thread *cur = thread_current ();
        cur->exited = true;
        cur->exit_status = -1;
        thread_exit ();
      }

    case SEL_KCSEG:
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      PANIC ("Interrupt in unknown segment");
    }
}

static void
page_fault (struct intr_frame *f)
{
  void *fault_addr;
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  intr_enable ();
  page_fault_cnt++;

  /* [P2-1 FIX] 출력 금지, 유저/커널 구분해서 처리 */
  if (f->cs == SEL_UCSEG)
    {
      struct thread *cur = thread_current ();
      cur->exited = true;
      cur->exit_status = -1;
      thread_exit ();
    }

  /* 커널에서 page fault면 버그 */
  intr_dump_frame (f);
  PANIC ("Kernel page fault");
}

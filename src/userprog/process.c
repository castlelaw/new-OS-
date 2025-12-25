#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

extern struct lock filesys_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool push_arguments (void **esp, const char *cmdline);

/* [Fix] install_page 함수 원형 선언 추가 (implicit declaration 에러 해결) */
static bool install_page (void *upage, void *kpage, bool writable);

/* 프로세스 실행 간 동기화를 위한 구조체 */
struct exec_info {
  char *cmdline;
  struct semaphore done;
  bool success;
};

/* Starts a new thread running a user program loaded from FILENAME. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Parse program name for thread_create */
  char name_copy[16];
  strlcpy (name_copy, file_name, sizeof name_copy);
  char *save_ptr;
  char *prog_name = strtok_r (name_copy, " ", &save_ptr);

  /* Prepare execution info for synchronization */
  struct exec_info *info = malloc (sizeof (struct exec_info));
  if (info == NULL) 
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  
  info->cmdline = fn_copy;
  info->success = false;
  sema_init (&info->done, 0);

  /* Create the thread */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, info);
  
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      free (info);
      return TID_ERROR;
    }

  /* Wait for child to load successfully or fail */
  sema_down (&info->done);

  if (!info->success)
    tid = TID_ERROR;

  free (info);
  return tid;
}

/* A thread function that loads a user process and starts it running. */
static void
start_process (void *aux_)
{
  struct exec_info *info = aux_;
  char *cmdline = info->cmdline; // 이것은 palloc된 페이지임
  struct intr_frame if_;
  bool success;

#ifdef USERPROG
  /* Initialize thread fields */
  struct thread *cur = thread_current ();
  cur->exited = false;
  cur->exit_status = -1;
  /* [Fix] load_completed 제거됨 */
  cur->load_success = false;
#endif

  /* Initialize interrupt frame */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Load executable. */
  /* 파일 시스템 접근 시 Lock 사용 */
  lock_acquire (&filesys_lock);
  success = load (cmdline, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);

  /* Load 성공 시 Argument Passing 수행 */
  if (success) 
    {
      if (!push_arguments (&if_.esp, cmdline))
        success = false;
    }

  /* Clean up cmdline page */
  palloc_free_page (cmdline);

  /* Signal parent */
  info->success = success;
#ifdef USERPROG
  /* [Fix] load_completed 제거됨 */
  cur->load_success = success;
#endif
  sema_up (&info->done);

  /* If load failed, quit. */
  if (!success) 
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in threads/intr-stubs.S).
     Because intr_exit takes all of its arguments on the stack in
     the form of a `struct intr_frame', we just point the stack
     pointer (%esp) to our stack frame and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status. */
int
process_wait (tid_t child_tid UNUSED)
{
  /* Project 2-1 Requirement: Infinite loop */
  struct thread *child = get_thread (child_tid);

  if (child == NULL)
    return -1;

  /* 자식이 종료될 때까지 대기 (sema_up이 불릴 때까지) */
  sema_down (&child->wait_sema);

  /* 자식의 종료 상태 반환 */
  int status = child->exit_status;
  
  /* 자식 스레드가 완전히 사라지기 전에 exit_status를 확보했다고 가정 */
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

#ifdef USERPROG
  /* 종료 메시지 출력: 프로세스 이름과 exit code */
  /* 로드에 성공한 유저 프로세스만 출력 */
  if (cur->pagedir != NULL && cur->load_success) 
    {
      printf ("%s: exit(%d)\n", cur->name, cur->exit_status);
    }
    
  /* 실행 중인 파일 닫기 (쓰기 권한 복구) */
  if (cur->bin_file != NULL)
    {
      file_close (cur->bin_file);
      cur->bin_file = NULL;
    }
  sema_up (&cur->wait_sema);
#endif

  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing interrupts. */
  tss_update ();
}

/* ELF headers and types */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* cmdline에서 프로그램 이름만 추출 */
  char *fn_copy = palloc_get_page (0);
  if (fn_copy == NULL) goto done;
  strlcpy (fn_copy, file_name, PGSIZE);
  char *save_ptr;
  char *prog_name = strtok_r (fn_copy, " ", &save_ptr);

  /* Open executable file. */
  file = filesys_open (prog_name);
  palloc_free_page (fn_copy); // 이름 추출 후 해제

  if (file == NULL)
    {
      printf ("load: %s: open failed\n", prog_name);
      goto done;
    }

  /* [P2-1 & P2-2 Required] Executable write protection */
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", prog_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;

      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (success) 
    {
      /* 로드 성공 시 파일 포인터를 스레드에 저장해 쓰기 방지 유지 */
      t->bin_file = file; 
    }
  else 
    {
      file_close (file);
    }
  return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     It's not profitable to use this for code or data. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Argument Passing Helper */
static bool
push_arguments (void **esp, const char *cmdline)
{
  char *copy = palloc_get_page (0);
  if (copy == NULL)
    return false;
  strlcpy (copy, cmdline, PGSIZE);

  char *argv[64]; // 제한: 인자는 최대 64개로 가정
  int argc = 0;
  char *token, *save_ptr;

  /* 1. Parse arguments */
  for (token = strtok_r (copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
       if (argc >= 64) 
        {
          palloc_free_page (copy);
          return false; 
        }
       argv[argc++] = token;
    }
    
  if (argc == 0) // Should not happen with current logic, but safe check
    {
      palloc_free_page (copy);
      return false;
    }

  void *sp = *esp;
  char *arg_addrs[64]; // 스택에 복사된 문자열의 주소 저장

  /* 2. Push strings (reverse loop not strictly needed for strings, 
        but good for keeping order in memory) */
  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1; // null terminator 포함
      sp = (uint8_t *) sp - len;
      memcpy (sp, argv[i], len);
      arg_addrs[i] = sp;
    }

  /* 3. Word Align */
  uintptr_t sp_val = (uintptr_t) sp;
  size_t pad = sp_val % 4;
  if (pad)
    {
      sp = (uint8_t *) sp - pad;
      memset (sp, 0, pad);
    }

  /* 4. Push NULL sentinel for argv[argc] */
  sp = (uint8_t *) sp - sizeof (char *);
  *(char **) sp = NULL;

  /* 5. Push argv pointers (Right-to-Left) */
  for (int i = argc - 1; i >= 0; i--)
    {
      sp = (uint8_t *) sp - sizeof (char *);
      *(char **) sp = arg_addrs[i];
    }

  /* 6. Push argv pointer (pointer to argv[0]) */
  char **argv0 = (char **) sp;
  sp = (uint8_t *) sp - sizeof (char **);
  *(char ***) sp = argv0;

  /* 7. Push argc */
  sp = (uint8_t *) sp - sizeof (int);
  *(int *) sp = argc;

  /* 8. Push fake return address */
  sp = (uint8_t *) sp - sizeof (void *);
  *(void **) sp = NULL;

  *esp = sp;
  palloc_free_page (copy);
  return true;
}
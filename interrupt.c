/*
 * interrupt.c -
 */
#include <hardware.h>
#include <io.h>
#include <interrupt.h>
#include <types.h>
#include <segment.h>

#include <zeos_interrupt.h>

#include <shared.h>
#include <libc.h>
#include <cbuffer.h>

/* este include y struct externo son para llamar al task_switch desde aqui */
#include <sched.h>
extern struct task_struct *idle_task;

Gate idt[IDT_ENTRIES];
Register    idtR;

void writeMSR(int number, int value);
int get_fault_eip();

struct cbuffer keyboard_buffer;
struct list_head keyboard_blocked;

char char_map[] =
{
  '\0','\0','1','2','3','4','5','6',
  '7','8','9','0','\'','¡','\0','\0',
  'q','w','e','r','t','y','u','i',
  'o','p','`','+','\0','\0','a','s',
  'd','f','g','h','j','k','l','ñ',
  '\0','º','\0','ç','z','x','c','v',
  'b','n','m',',','.','-','\0','*',
  '\0','\0','\0','\0','\0','\0','\0','\0',
  '\0','\0','\0','\0','\0','\0','\0','7',
  '8','9','-','4','5','6','+','1',
  '2','3','0','\0','\0','\0','<','\0',
  '\0','\0','\0','\0','\0','\0','\0','\0',
  '\0','\0'
};

void setInterruptHandler(int vector, void (*handler)(), int maxAccessibleFromPL)
{
  /***********************************************************************/
  /* THE INTERRUPTION GATE FLAGS:                          R1: pg. 5-11  */
  /* ***************************                                         */
  /* flags = x xx 0x110 000 ?????                                        */
  /*         |  |  |                                                     */
  /*         |  |   \ D = Size of gate: 1 = 32 bits; 0 = 16 bits         */
  /*         |   \ DPL = Num. higher PL from which it is accessible      */
  /*          \ P = Segment Present bit                                  */
  /***********************************************************************/
  Word flags = (Word)(maxAccessibleFromPL << 13);
  flags |= 0x8E00;    /* P = 1, D = 1, Type = 1110 (Interrupt Gate) */

  idt[vector].lowOffset       = lowWord((DWord)handler);
  idt[vector].segmentSelector = __KERNEL_CS;
  idt[vector].flags           = flags;
  idt[vector].highOffset      = highWord((DWord)handler);
}

void setTrapHandler(int vector, void (*handler)(), int maxAccessibleFromPL)
{
  /***********************************************************************/
  /* THE TRAP GATE FLAGS:                                  R1: pg. 5-11  */
  /* ********************                                                */
  /* flags = x xx 0x111 000 ?????                                        */
  /*         |  |  |                                                     */
  /*         |  |   \ D = Size of gate: 1 = 32 bits; 0 = 16 bits         */
  /*         |   \ DPL = Num. higher PL from which it is accessible      */
  /*          \ P = Segment Present bit                                  */
  /***********************************************************************/
  Word flags = (Word)(maxAccessibleFromPL << 13);

  //flags |= 0x8F00;    /* P = 1, D = 1, Type = 1111 (Trap Gate) */
  /* Changed to 0x8e00 to convert it to an 'interrupt gate' and so
     the system calls will be thread-safe. */
  flags |= 0x8E00;    /* P = 1, D = 1, Type = 1110 (Interrupt Gate) */

  idt[vector].lowOffset       = lowWord((DWord)handler);
  idt[vector].segmentSelector = __KERNEL_CS;
  idt[vector].flags           = flags;
  idt[vector].highOffset      = highWord((DWord)handler);
}


void setIdt()
{
  /* Program interrups/exception service routines */
  idtR.base  = (DWord)idt;
  idtR.limit = IDT_ENTRIES * sizeof(Gate) - 1;
  
  set_handlers();

  /* ADD INITIALIZATION CODE FOR INTERRUPT VECTOR */
  setInterruptHandler(14, page_fault_handler_custom, 0);
  setInterruptHandler(32, clock_handler, 0);
  setInterruptHandler(33, keyboard_handler, 0);
  setTrapHandler(0x80, system_call_handler, 3);

  writeMSR(0x174, __KERNEL_CS);
  writeMSR(0x175, INITIAL_ESP);
  writeMSR(0x176, (int)&system_call_handler_fast);

  set_idt_reg(&idtR);
}

void keyboard_routine()
{
    update_user_to_system_ticks();
    
    unsigned char data = inb(0x60);
    if (!cbuffer_full(&keyboard_buffer) && (data & 0x80) == 0) {
        unsigned char c = char_map[data & 0x7F];
        if (!list_empty(&keyboard_blocked)) {
            struct list_head *e = list_first(&keyboard_blocked);
            struct task_struct *t = list_entry(e, struct task_struct, list);
            t->keyboard_read = c;
            update_process_state_rr(t, &readyqueue);
        } else {
            cbuffer_push(&keyboard_buffer, c);
        }
    }
    
    update_system_to_user_ticks();
}

void clock_routine()
{
    ++zeos_ticks;
    zeos_show_clock();

    struct list_head *e, *n;
    struct task_struct *t;
    list_for_each_safe(e, n, &keyboard_blocked) {
	t = list_entry(e, struct task_struct, list);
        t->timeout--;
    	if (t->timeout <= 0) {
            update_process_state_rr(t, &readyqueue);
	}
    }
    
    schedule();
}

void page_fault_routine_custom()
{
    char addr_buf[32];
    int addr = get_fault_eip();
    itoh(addr, addr_buf);

    printk("Process generates a PAGE FAULT exception at EIP: ");
    printk(addr_buf);
    while(1);
}

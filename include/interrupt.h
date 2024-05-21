/*
 * interrupt.h - Definició de les diferents rutines de tractament d'exepcions
 */

#ifndef __INTERRUPT_H__
#define __INTERRUPT_H__

#include <types.h>

#define IDT_ENTRIES 256

extern Gate idt[IDT_ENTRIES];
extern Register idtR;

extern struct cbuffer keyboard_buffer;
extern struct list_head keyboard_blocked;

void setInterruptHandler(int vector, void (*handler)(), int maxAccessibleFromPL);
void setTrapHandler(int vector, void (*handler)(), int maxAccessibleFromPL);

void setIdt();

void page_fault_handler_custom();
void page_fault_routine_custom();

void keyboard_handler();
void keyboard_routine();

void clock_handler();
void clock_routine();

void system_call_handler();
void system_call_handler_fast();

#endif  /* __INTERRUPT_H__ */

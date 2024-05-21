/*
 * libc.h - macros per fer els traps amb diferents arguments
 *          definició de les crides a sistema
 */
 
#ifndef __LIBC_H__
#define __LIBC_H__

#include <stats.h>

int write(int fd, char *buffer, int size);

int gettime();

void itoa(int a, char *b);

int strlen(char *a);

int getpid();

int fork();

void exit();

void perror();

int get_stats(int pid, struct stats *st);

int waitKey(char *b, int timeout);

int gotoXY (int x, int y);

int changeColor (int fg, int bg);

int clrscr (char *b);

int threadCreateWithStack (void (*function)(void *arg), int N, void *parameter);

struct sem_t *semCreate(int initial_value);

int semWait(struct sem_t *s);

int semSignal(struct sem_t *s);

int semDestroy(struct sem_t *s);

char *memRegGet(int num_pages);

int memRegDel(char *m);

#endif  /* __LIBC_H__ */

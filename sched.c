/*
 * sched.c - initializes struct for task 0 anda task 1
 */

#include <sched.h>
#include <mm.h>
#include <io.h>
#include <stats.h>
#include <devices.h>
#include <libc.h>

union task_union task[NR_TASKS]
__attribute__((__section__(".data.task")));

struct task_struct *list_head_to_task_struct(struct list_head *l)
{
    return list_entry(l, struct task_struct, list);
}

struct list_head freequeue;
struct list_head readyqueue;

struct task_struct *idle_task;

unsigned int global_PID = 0;
unsigned int global_TID = 1;

int quantum;

struct sem_t semaphores[SEM_NUM];
struct list_head sem_freequeue;

void writeMSR(int number, int value);
unsigned int get_ebp();
void set_esp(unsigned int new_esp);

/* get_DIR - Returns the Page Directory address for task 't' */
page_table_entry * get_DIR (struct task_struct *t) 
{
    return t->dir_pages_baseAddr;
}

/* get_PT - Returns the Page Table address for task 't' */
page_table_entry * get_PT (struct task_struct *t) 
{
    return (page_table_entry *)(((unsigned int)(t->dir_pages_baseAddr->bits.pbase_addr))<<12);
}


int allocate_DIR(struct task_struct *t) 
{
    int pos;

    pos = ((int)t-(int)task)/sizeof(union task_union);

    t->dir_pages_baseAddr = (page_table_entry*) &dir_pages[pos]; 

    return 1;
}

void cpu_idle(void)
{
    __asm__ __volatile__("sti": : :"memory");

    while(1);
}

void init_idle (void)
{
    struct list_head *e = list_first(&freequeue);
    list_del(e);
    struct task_struct *t = list_entry(e, struct task_struct, list);
    t->PID = global_PID++;
    t->TID = 0;
    INIT_STATS(&t->stats);
    INIT_LIST_HEAD(&t->list);
    t->called_to_die = 0;
    t->temp_stack_size = 0;
    INIT_LIST_HEAD(&t->mem_list);
    set_quantum(t, 100);
    allocate_DIR(t);
        
    union task_union *u = (union task_union *)t;
    u->stack[KERNEL_STACK_SIZE-2] = 0; // ebp
    u->stack[KERNEL_STACK_SIZE-1] = (unsigned long)cpu_idle; // @ret
    t->kernel_esp = (long unsigned int)&u->stack[KERNEL_STACK_SIZE-2];
    idle_task = t;
}

void init_task1(void)
{
    struct list_head *e = list_first(&freequeue);
    list_del(e);
    struct task_struct *t = list_entry(e, struct task_struct, list);
    t->PID = global_PID++;
    t->TID = 0;
    INIT_STATS(&t->stats);
    INIT_LIST_HEAD(&t->list);
    t->called_to_die = 0;
    t->temp_stack_size = 0;
    INIT_LIST_HEAD(&t->mem_list);
    set_quantum(t, 100);
    allocate_DIR(t);
    set_user_pages(t);

    union task_union *u = (union task_union *)t;
    tss.esp0 = (long unsigned int)&u->stack[KERNEL_STACK_SIZE];
    writeMSR(0x175, (int)&u->stack[KERNEL_STACK_SIZE]);
    quantum = t->quantum;
    set_cr3(t->dir_pages_baseAddr);
}


void init_sched()
{
    INIT_LIST_HEAD(&readyqueue);
    INIT_LIST_HEAD(&freequeue);
    for(int i=0; i<NR_TASKS; ++i) {
        list_add_tail(&task[i].task.list, &freequeue);
    }
    init_semaphores();
}

struct task_struct* current()
{
    int ret_value;
  
    __asm__ __volatile__(
                         "movl %%esp, %0"
                         : "=g" (ret_value)
                         );
    return (struct task_struct*)(ret_value&0xfffff000);
}

void inner_task_switch(union task_union *t) {
    tss.esp0 = (int)&t->stack[KERNEL_STACK_SIZE];
    writeMSR(0x175, (int)&t->stack[KERNEL_STACK_SIZE]);
    set_cr3(get_DIR(&t->task));
    quantum = get_quantum(&t->task);
    current()->kernel_esp = get_ebp();
    update_system_to_ready_ticks();
    set_esp(t->task.kernel_esp);
    update_ready_to_system_ticks();
}

int ret_from_fork() {
    return 0;
}

struct task_struct* get_process_by_pid (int pid) {
    if (pid >= global_PID) return NULL; // no existe el proceso con PID = pid
    if (current()->PID == pid) return current();

    struct list_head *l;
    list_for_each(l, &readyqueue) {
        struct task_struct *t = list_entry(l, struct task_struct, list);
        if (t->PID == pid) return t;
    }
    return NULL;
}

void update_sched_data_rr (void) {
    --quantum;
}

int needs_sched_rr (void) {
    if (quantum == 0) {
        if (!list_empty(&readyqueue)) return 1;
        else quantum = get_quantum(current());
    }
    return 0;
}

void update_process_state_rr (struct task_struct *t, struct list_head *dst_queue) {
    if (current() != t) list_del(&t->list);
    if (dst_queue != NULL) list_add_tail(&t->list, dst_queue);
}

void sched_next_rr (void) {
    if (list_empty(&readyqueue)) {
        /* printk("CAMBIANDO A IDLE. "); */
        task_switch((union task_union *)idle_task);
    } else {
        struct list_head *e = list_first(&readyqueue);
        struct task_struct *t = list_entry(e, struct task_struct, list);
        
        /* printk("CAMBIANDO A THREAD (PID: "); */
        /* char info[8]; */
        /* itoa(t->PID, info); */
        /* printk(info); */
        /* printk(", TID: "); */
        /* itoa(t->TID, info); */
        /* printk(info); */
        /* printk("). "); */

        update_process_state_rr(t, NULL);
        task_switch((union task_union *)t);
    }
}

void schedule (void) {
    update_sched_data_rr();
    if (needs_sched_rr()) {
        /* printk("TOCA CAMBIAR DE THREAD. "); */
        if (current() != idle_task)
            update_process_state_rr(current(), &readyqueue);
        sched_next_rr();
    }
}

int get_quantum (struct task_struct *t) {
    return t->quantum;
}

void set_quantum (struct task_struct *t, int new_quantum) {
    t->quantum = new_quantum;
}

void init_semaphores (void) {
    INIT_LIST_HEAD(&sem_freequeue);
    for (int i=0; i<SEM_NUM; ++i)
        list_add_tail(&(semaphores[i].sem_list), &sem_freequeue);
}

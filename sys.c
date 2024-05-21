/*
 * sys.c - Syscalls implementation
 */
#include <devices.h>
#include <utils.h>
#include <io.h>
#include <mm.h>
#include <mm_address.h>
#include <sched.h>
#include <stats.h>
#include <interrupt.h>
#include <cbuffer.h>

#define LECTURA 0
#define ESCRIPTURA 1

#define WRITE_MAX 256

extern unsigned char pid_list[NR_PIDS];

int check_fd(int fd, int permissions)
{
    if (fd!=1) return -9; /*EBADF*/
    if (permissions!=ESCRIPTURA) return -13; /*EACCES*/
    return 0;
}

int sys_ni_syscall()
{
    return -38; /*ENOSYS*/
}

int sys_getpid()
{
    return current()->PID;
}

int sys_fork()
{
    // pillar un PCB libre
    struct list_head *e = list_first(&freequeue);
    if (list_empty(e)) return -1;
    list_del(e);
    struct task_struct *t = list_entry(e, struct task_struct, list);

    // copiar el task_union del padre al hijo
    copy_data(current(), t, sizeof(union task_union));
    allocate_DIR(t);

    page_table_entry *child_PT = get_PT(t), *parent_PT = get_PT(current());
    page_table_entry *parent_DIR = get_DIR(current());
    int new_frame, pag;

    // copiar paginas del kernel
    for (pag=0; pag<NUM_PAG_KERNEL; pag++) {
        child_PT[(KERNEL_START>>12)+pag] = parent_PT[(KERNEL_START>>12)+pag];
    }

    // copiar paginas del codigo
    for (pag=0; pag<NUM_PAG_CODE; pag++) {
        child_PT[PAG_LOG_INIT_CODE+pag] = parent_PT[PAG_LOG_INIT_CODE+pag];      
    }

    // buscar entrada libre de la TP
    int temp_entry=-1;
    pag=TOTAL_PAGES-1;
    while (pag>(KERNEL_START>>12)+NUM_PAG_KERNEL && temp_entry<0) {
        if (parent_PT[pag].entry == 0) temp_entry = pag;
        --pag;
    }
    if (temp_entry<0) {	
        list_add(e, &freequeue);
        return -1;
    }  

    // alojar paginas de datos para el hijo y copiar los del padre
    for (pag=0; pag<NUM_PAG_DATA; pag++) {
        new_frame = alloc_frame();
        if (new_frame<0) {
            // desalojar frames en caso de error
            for (pag = pag-1; pag >= 0; --pag) {
                free_frame(child_PT[PAG_LOG_INIT_DATA+pag].bits.pbase_addr);
                del_ss_pag(child_PT, PAG_LOG_INIT_DATA+pag);
            }
            del_ss_pag(parent_PT, temp_entry);
            set_cr3(parent_DIR);
            list_add(e, &freequeue);
            return -1;
        }
      
        set_ss_pag(child_PT, PAG_LOG_INIT_DATA+pag, new_frame);
        set_ss_pag(parent_PT, temp_entry, new_frame);
      
        void *parent_page = (void *)((PAG_LOG_INIT_DATA+pag)<<12);
        void *child_page = (void *)(temp_entry<<12);
        copy_data(parent_page, child_page, PAGE_SIZE);
      
        set_cr3(parent_DIR); // flush TLB
    }

    // copiar las regiones de memoria que el padre reservo con memRegGet
    struct list_head *l;
    list_for_each(l, &current()->mem_list) {
        struct heap_control *hc = list_entry(l, struct heap_control, list);
        int region_pag = (int)hc>>12;
        for (int i=0; i<hc->size; ++i) {
            new_frame = alloc_frame();
            
            set_ss_pag(child_PT, region_pag+i, new_frame);
            set_ss_pag(parent_PT, temp_entry, new_frame);
            
            void *parent_page = (void *)((region_pag+i)<<12);
            void *child_page = (void *)(temp_entry<<12);
            copy_data(parent_page, child_page, PAGE_SIZE);
            
            set_cr3(parent_DIR); // flush TLB
        }
    }
   
    // copiar la pila temporal de los threads (task1 y sus hijos no entrarán aqui)
    int parent_stack_size = current()->temp_stack_size;
    int parent_stack_init = current()->temp_stack_page;
    for (pag=0; pag<parent_stack_size; pag++) {
       	new_frame = alloc_frame();
       	if (new_frame<0) {
            // desalojar frames en caso de error
            for (pag = pag-1; pag >= 0; --pag) {
                free_frame(get_frame(child_PT, parent_stack_init-pag));
                del_ss_pag(child_PT, parent_stack_init-pag);
            }
            for (pag = 0; pag < NUM_PAG_DATA; ++pag) {
                free_frame(get_frame(child_PT, PAG_LOG_INIT_DATA+pag));
                del_ss_pag(child_PT, PAG_LOG_INIT_DATA+pag);
            }
            del_ss_pag(parent_PT, temp_entry);
            set_cr3(parent_DIR);
            list_add(e, &freequeue);
            return -1;
        }
       	set_ss_pag(child_PT, parent_stack_init-pag, new_frame);
       	set_ss_pag(parent_PT, temp_entry, new_frame);
     
       	void *parent_page = (void *)((parent_stack_init-pag)<<12);
       	void *child_page = (void *)(temp_entry<<12);
       	copy_data(parent_page, child_page, PAGE_SIZE);
     
       	set_cr3(parent_DIR); // flush TLB
    }

    del_ss_pag(parent_PT, temp_entry);
    set_cr3(parent_DIR);
  
    // cambiar campos del task_struct del hijo no comunes con el padre
    t->PID = global_PID++;
    t->TID = 0;
    INIT_STATS(&t->stats);
  
    // preparar la pila del hijo
    union task_union *u = (union task_union *)t;
    // unsigned int pos = KERNEL_STACK_SIZE-1 - (5+11+1); /* ctx HW + ctx SW + @ret al handler */
    //int pos = KERNEL_STACK_SIZE-18; /* ctx HW + ctx SW + @ret al handler = 17 */
    int offset = 18; //17?
    u->stack[KERNEL_STACK_SIZE-offset-1] = 0; // ebp
    u->stack[KERNEL_STACK_SIZE-offset] = (unsigned long)ret_from_fork; // @ret
    t->kernel_esp = (unsigned long int)&u->stack[KERNEL_STACK_SIZE-offset-1];
    
    // insertar hijo en readyqueue
    list_add_tail(&t->list, &readyqueue);
    
    return t->PID;
}

void sys_exit()
{
    int pag;
    page_table_entry *current_PT = get_PT(current());

    if (current()->TID == 0) {
        for (pag=0; pag<NUM_PAG_DATA; pag++) {
            free_frame(get_frame(current_PT, PAG_LOG_INIT_DATA+pag));
            del_ss_pag(current_PT, PAG_LOG_INIT_DATA+pag);
        }
    }

    struct list_head *e, *n;
    struct heap_control *hc;
    list_for_each_safe(e, n, &current()->mem_list) {
        hc = list_entry(e, struct heap_control, list);
        list_del(e);
        int region_pag = (int)hc>>12;
        for (int i=0; i<hc->size; ++i) {
            free_frame(get_frame(current_PT, region_pag+pag));
            del_ss_pag(current_PT, region_pag+pag);
        }
    }
    
    int base_pag = current()->temp_stack_page;
    int stack_size = current()->temp_stack_size;
    for (pag=0; pag<stack_size; pag++) {
        free_frame(get_frame(current_PT, base_pag-pag));
        del_ss_pag(current_PT, base_pag-pag);
    }

    /* printk("exit realizado (PID: "); */
    /* char info[20]; */
    /* itoa(current()->PID, info); */
    /* printk(info); */
    /* printk(", TID: "); */
    /* itoa(current()->TID, info); */
    /* printk(info); */
    /* printk("). "); */
        
    current()->PID = -1;
    current()->TID = -1;
    update_process_state_rr(current(), &freequeue);
    sched_next_rr();
}

int sys_write(int fd, char *buffer, int size)
{
    // comprobaciones
    int valido = check_fd(fd, ESCRIPTURA);
    if (valido < 0) return valido;
    if (buffer == NULL) return -1;
    if (size < 0) return -1;

    // copiar datos del usuario y escribir
    char sys_buffer[WRITE_MAX];
    int s = 0, written = 0, returned, to_write;
    while (s < size) {
        if (size-s <= WRITE_MAX) to_write = size-s;
        else to_write = WRITE_MAX;
        
        copy_data(&buffer[s], sys_buffer, to_write);
        s += to_write;
        
        switch (fd) {
        case 1:
            returned = sys_write_console(sys_buffer, to_write);
            break;
        }

        if (returned < 0) return returned;
        else written += returned;
    }

    return written;
}

int sys_gettime()
{
    return zeos_ticks;
}

int sys_get_stats (int pid, struct stats *st)
{
    struct task_struct *p = get_process_by_pid(pid);
    if (p == NULL) return -1;
    if (st == NULL) return -1;
    copy_data(&p->stats, st, sizeof(struct stats));
    
    return 0;
}

int sys_waitKey(char *b, int timeout) {
    if (b == NULL) return -1;
    if (!cbuffer_empty(&keyboard_buffer)) {
        current()->keyboard_read = cbuffer_pop(&keyboard_buffer);
        copy_to_user(&(current()->keyboard_read), b, sizeof(char));
        return 0;
    } else {
        // pasar de segundos a ticks (1 s -> 18 ticks)
        current()->timeout = timeout*18;
        update_process_state_rr(current(), &keyboard_blocked);
        sched_next_rr();

        if (current()->timeout > 0) {
            copy_to_user(&(current()->keyboard_read), b, sizeof(char));
            return 0;
        }
    }
    return -1;
}

int sys_gotoXY (int x, int y) {    
    if (x < 0 || x >= 80 || y < 0 || y >= 25) return -1;
    return point_to(x, y, point.fg, point.bg);
}

int sys_changeColor (int fg, int bg) {
    if (fg < 0 || bg < 0) return -1;
    return point_to(point.x, point.y, fg, bg);
}

int sys_clrscr (char *b) {
    return set_screen(b);
}

int sys_threadCreateWithStack(void (*function)(void *arg), int N, void *parameter, void *wrapper) {
    if (function == NULL || N < 0 || wrapper == NULL) return -1;
    /* printk("creando thread. "); */
    
    // pillar un TCB (PCB) libre
    if (list_empty(&freequeue)) return -1;
    struct list_head *e = list_first(&freequeue);
    list_del(e);
    struct task_struct *t = list_entry(e, struct task_struct, list);
    page_table_entry *PT = get_PT(current());

    // copiar el task_union
    copy_data(current(), t, sizeof(union task_union));

    // alojar la pila para el nuevo thread
    
    // paso 1) buscar region de la TP consecutiva de N posiciones
    int pag = TOTAL_PAGES-1, found_pag = -1;
    while (pag>PAG_LOG_INIT_DATA+NUM_PAG_DATA+N && found_pag<0) {
        if (PT[pag].entry == 0) {
            // comprobar N-1 siguientes posiciones de la TP
            int offset = 1;
            while (offset < N && PT[pag-offset].entry == 0) ++offset;
            if (offset == N) found_pag = pag;
            else pag -= offset;
        }
        else pag -= 1;
    }

    // paso 2) alojar frames si se ha encontado la region
    if (found_pag<0) {
        list_add(e, &freequeue);
        return -1;
    }
    for (pag=0; pag<N; ++pag) {
        int new_frame = alloc_frame();
        if (new_frame<0) {
            // desalojar frames en case de error
            for (pag = pag-1; pag >= 0; --pag) {
                free_frame(get_frame(PT, found_pag-pag));
                del_ss_pag(PT, found_pag-pag);
            }
            list_add(e, &freequeue);
            set_cr3(get_DIR(current()));
            return -1;
        }
        set_ss_pag(PT, found_pag-pag, new_frame);
    }

    // cambiar campos diferentes del task_struct
    INIT_STATS(&t->stats);
    t->TID = global_TID++;
    t->temp_stack_page = found_pag;
    t->temp_stack_size = N;
    if (list_empty(&current()->mem_list)) INIT_LIST_HEAD(&t->mem_list);

    // preparar la pila
    int base_addr = (found_pag+1)<<12;
    
    // paso 1) pila de sistema 
    union task_union *u = (union task_union *)t;
    u->stack[KERNEL_STACK_SIZE-5] = (unsigned long)wrapper; // eip
    u->stack[KERNEL_STACK_SIZE-2] = base_addr - 2*sizeof(void *); // esp
    int stack_offset = 18;
    t->kernel_esp = (unsigned long int)&u->stack[KERNEL_STACK_SIZE-stack_offset];
    
    // paso 2) pila de usuario (pinta travieso, es para el threadCallWrapper)
    *(void **)(base_addr - sizeof(void *)) = parameter;
    *(void **)(base_addr - 2*sizeof(void *)) = function;

    // insertar en readyqueue
    /* update_process_state_rr(t, &readyqueue); */
    list_add_tail(&t->list, &readyqueue);

    /* printk("thread creado (PID: "); */
    /* char info[20]; */
    /* itoa(t->PID, info); */
    /* printk(info); */
    /* printk(", TID: "); */
    /* itoa(t->TID, info); */
    /* printk(info); */
    /* printk("). "); */
       
    return 0;
}

struct sem_t *sys_semCreate (int initial_value) {
    if (list_empty(&sem_freequeue)) return NULL;
    struct list_head *e = list_first(&sem_freequeue);
    list_del(e);
    struct sem_t *new_sem = list_entry(e, struct sem_t, sem_list);
    new_sem->count = initial_value;
    INIT_LIST_HEAD(&new_sem->blocked);
    return new_sem;
}

int sys_semWait (struct sem_t *s) {
    if (s == NULL) return -1;
    s->count--;
    if (s->count < 0) {
        update_process_state_rr(current(), &s->blocked);
        sched_next_rr();
        if (current()->called_to_die) return -1;
    }
    return 0;
}

int sys_semSignal (struct sem_t *s) {
    if (s == NULL) return -1;
    s->count++;
    if (s->count <= 0 && !list_empty(&s->blocked)) {
        struct list_head *e = list_first(&s->blocked);
        struct task_struct *t = list_entry(e, struct task_struct, list);
        update_process_state_rr(t, &readyqueue);
    }
    return 0;
}

int sys_semDestroy (struct sem_t *s) {
    if (s == NULL) return -1;

    struct list_head *e, *n;
    struct task_struct *t;
    list_for_each_safe(e, n, &s->blocked) {
	t = list_entry(e, struct task_struct, list);
        t->called_to_die = 1;
        update_process_state_rr(t, &readyqueue);
    }
    
    list_add_tail(&s->sem_list, &sem_freequeue);
    return 0;
}

char* sys_memRegGet(int num_pages) {
    if (num_pages<=0) return NULL;
    page_table_entry *PT = get_PT(current());

    // buscar entradas consecutivas libres de la TP
    int pag = PAG_LOG_INIT_DATA+NUM_PAG_DATA, found_pag = -1;
    while (pag<TOTAL_PAGES-num_pages && found_pag<0) {
        if (PT[pag].entry == 0) {
            int offset = 1;
            while (offset < num_pages && PT[pag+offset].entry == 0) ++offset;
            if (offset == num_pages) found_pag = pag;
            else pag += offset;
        }
        else pag += 1;
    }

    // alojar frames para las paginas
    if (found_pag<0) return NULL;
    for (pag=0; pag<num_pages; ++pag) {
        int new_frame = alloc_frame();
        if (new_frame<0) {
            for (pag = pag-1; pag>=0; --pag) {
                free_frame(get_frame(PT, found_pag+pag));
                del_ss_pag(PT, found_pag+pag);   
            }
            set_cr3(get_DIR(current()));
            return NULL;
        }
        set_ss_pag(PT, found_pag+pag, new_frame);
    }

    // inicializar parte de memoria de control
    struct heap_control *hc = (struct heap_control *)(found_pag<<12);
    INIT_LIST_HEAD(&hc->list);
    list_add_tail(&hc->list, &current()->mem_list);
    hc->size = num_pages;

    return (char *)((found_pag<<12)+sizeof(struct heap_control));
}

int sys_memRegDel(char *m) {
    if (m == NULL) return -1;

    page_table_entry *PT = get_PT(current());

    struct heap_control *hc = (struct heap_control *)(m-sizeof(struct heap_control));
    list_del(&hc->list);
    
    int pag, init_pag = (int)m>>12;
    for (pag = 0; pag<hc->size; pag++) {
        free_frame(get_frame(PT, init_pag+pag));
        del_ss_pag(PT, init_pag+pag);
    }
    set_cr3(get_DIR(current()));

    return 0;
}

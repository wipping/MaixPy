/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George on behalf of Pycom Ltd
 * Copyright (c) 2017 Pycom Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "py/mpthread.h"
#include "mpthreadport.h"

#include "task.h"

#if MICROPY_PY_THREAD

#define MP_THREAD_MIN_STACK_SIZE                        (4 * 1024)
#define MP_THREAD_DEFAULT_STACK_SIZE                    (MP_THREAD_MIN_STACK_SIZE + 1024)
#define MP_THREAD_PRIORITY                              4

// this structure forms a linked list, one node per active thread
typedef struct _thread_t {
    TaskHandle_t id;        // system id of thread
    int ready;              // whether the thread is ready and running
    void *arg;              // thread Python args, a GC root pointer
    void *stack;            // pointer to the stack
    size_t stack_len;       // number of words in the stack
    struct _thread_t *next;
} thread_t;

// the mutex controls access to the linked list
STATIC mp_thread_mutex_t thread_mutex;
STATIC thread_t thread_entry0;
STATIC thread_t *thread; // root pointer, handled by mp_thread_gc_others
STATIC uint32_t thread_num; // 
void mp_thread_init(void *stack, uint32_t stack_len) {
    mp_thread_set_state(&mp_state_ctx.thread);
    // create the first entry in the linked list of all threads
    thread = &thread_entry0;
    thread->id = xTaskGetCurrentTaskHandle();
    thread->ready = 1;
    thread->arg = NULL;
    thread->stack = stack;
    thread->stack_len = stack_len;
    thread->next = NULL;
    mp_thread_mutex_init(&thread_mutex);
   // MP_THREAD_GIL_EXIT();
}

void mp_thread_gc_others(void) {
    mp_thread_mutex_lock(&thread_mutex, 1);
    for (thread_t *th = thread; th != NULL; th = th->next) {
        gc_collect_root((void**)&th, 1);
        gc_collect_root(&th->arg, 1); // probably not needed
        if (th->id == xTaskGetCurrentTaskHandle()) {
            continue;
        }
        if (!th->ready) {
            continue;
        }
        gc_collect_root(th->stack, th->stack_len); // probably not needed
    }
    mp_thread_mutex_unlock(&thread_mutex);
}

mp_state_thread_t *mp_thread_get_state(void) {
    return pvTaskGetThreadLocalStoragePointer(NULL, 1);
}

void mp_thread_set_state(void *state) {
    vTaskSetThreadLocalStoragePointer(NULL, 1, state);
}

void mp_thread_start(void) {
    mp_thread_mutex_lock(&thread_mutex, 1);
    for (thread_t *th = thread; th != NULL; th = th->next) {
        if (th->id == xTaskGetCurrentTaskHandle()) {
            th->ready = 1;
            break;
        }
    }
    mp_thread_mutex_unlock(&thread_mutex);
}

// TODO: wether need to store thread entry function into a global variable (in this way,we can access it)
// when thread exit, it will be deleted by mp_thread_finish
void *(*ext_thread_entry)(void*) = NULL;
void func_entry(void *arg) {
    if (ext_thread_entry) {
        ext_thread_entry(arg);
    }
    vTaskDelete(NULL);
    for (;;);
}

void mp_thread_create_ex(void *(*entry)(void*), void *arg, size_t *stack_size, int priority, char *name) {
    //TODO: store thread entry function into a global variable so we can access it.
    // !!!There is a bug here,func_entry can't work in thread If func_entry and ext_thread_entry are defined as a static variable
    ext_thread_entry = entry;
    if (*stack_size == 0) {
        *stack_size = MP_THREAD_DEFAULT_STACK_SIZE; // default stack size
    } else if (*stack_size < MP_THREAD_MIN_STACK_SIZE) {
        *stack_size = MP_THREAD_MIN_STACK_SIZE; // minimum stack size
    }

    // allocate TCB, stack and linked-list node (must be outside thread_mutex lock)
    StackType_t *stack = NULL;
    thread_t *th = m_new_obj(thread_t);

    mp_thread_mutex_lock(&thread_mutex, 1);

    // create thread
    TaskHandle_t thread_id;
    TaskStatus_t task_status;
	//todo add schedule processor
    xTaskCreateAtProcessor(0, // processor
						   func_entry, // function entry
						   name, //task name
						   *stack_size / sizeof(StackType_t), //stack_deepth
						   arg, //function arg
						   priority, //task priority,please don't change this parameter,because it will impack function running
						   &thread_id);//task handle
    //printf("[MAIXPY]: thread_id %p created \n",thread_id);
    if (thread_id == NULL) {
        m_del_obj(thread_t,th);
        mp_thread_mutex_unlock(&thread_mutex);
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, "can't create thread"));
    }
	vTaskGetInfo(thread_id,&task_status,(BaseType_t)pdTRUE,(eTaskState)eInvalid);
	stack = task_status.pxStackBase;
    // adjust the stack_size to provide room to recover from hitting the limit
    *stack_size -= 1024;

    // add thread to linked list of all threads
    th->id = thread_id;
    th->ready = 0;
    th->arg = arg;
    th->stack = stack;
	//stack_len may be a bug,because that k210 addr width is 64 bit ,but addr width is 32bit
	//the StackType_t type is a type of the uintprt_t,uintprt_t in k210 is 64bit 
    th->stack_len = *stack_size / sizeof(StackType_t);
    th->next = thread;
    thread = th;
    mp_thread_mutex_unlock(&thread_mutex);

}

void mp_thread_create(void *(*entry)(void*), void *arg, size_t *stack_size) {
    uint8_t thread_name[30] = {0};
    sprintf(thread_name,"mp_thread%d",thread_num);
    mp_thread_create_ex(entry, arg, stack_size, MP_THREAD_PRIORITY, thread_name);
    thread_num++;
}


void mp_thread_mutex_init(mp_thread_mutex_t *mutex) {
    mutex->handle = xSemaphoreCreateMutexStatic(&mutex->buffer);
}

int mp_thread_mutex_lock(mp_thread_mutex_t *mutex, int wait) {
    return (pdTRUE == xSemaphoreTake(mutex->handle, wait ? portMAX_DELAY : 0));
}

void mp_thread_mutex_unlock(mp_thread_mutex_t *mutex) {
    xSemaphoreGive(mutex->handle);
}

void mp_thread_finish(void) {
    mp_thread_mutex_lock(&thread_mutex, 1);
    thread_t *th = thread;
    for (th = thread; th != 0; th = th->next) {
        if (th->id == xTaskGetCurrentTaskHandle()) {
            th->ready = 0;
            break;
        }
    }
    mp_thread_mutex_unlock(&thread_mutex);
}

//TODO:wether need to delete threand to free memory
/* ------------------------------------------------ */
// void mp_thread_delete(thread_t *pre_th,thread_t *th) {
//     if(pre_th == 0)//if th is head of thread list
//     {
//         thread = th->next;
//     }
//     else
//     {
//         pre_th->next = th->next;
//     }
//     printf("[MAIXPY]: mp_thread_delete test 1 \n");
//     m_del_obj(thread_t,th);
//     printf("[MAIXPY]: mp_thread_delete test 2 \n");
// }
// void mp_thread_clear(void) {
//     printf("[MAIXPY]: mp_thread_clear test 1\n");
//     mp_thread_mutex_lock(&thread_mutex, 1);
//     thread_t *th = NULL;
//     thread_t *pre_th = NULL;
//     for (th = thread; th != 0; th = th->next) {
//         if (th->id == xTaskGetCurrentTaskHandle()) {
//             if(th->ready == 0 && th->next != NULL)
//             {
//                 printf("[MAIXPY]: clear thread\n");
//                 mp_thread_delete(pre_th,th);
//                 printf("[MAIXPY]: finish mp_thread_delete\n");
//                 break;
//             }
//         }
//         pre_th = th;
//     }
//     printf("[MAIXPY]: mp_thread_clear test 2\n");
//     mp_thread_mutex_unlock(&thread_mutex);
// }

/* ------------------------------------------------ */
void mp_thread_deinit(void) {
    mp_thread_mutex_lock(&thread_mutex, 1);
    for (thread_t *th = thread; th != 0; th = th->next) {
        // don't delete the current task
        if (th->id == xTaskGetCurrentTaskHandle()) {
            // printf("[MAIXPY]: main thread %p\n",th->id);
            continue;
        }
        // printf("[MAIXPY]: deinit thread %p\n",th->id);
        if(0 != th->ready)
            vTaskDelete(th->id);
        m_del_obj(thread_t,th);
    }
    mp_thread_mutex_unlock(&thread_mutex);
    // allow FreeRTOS to clean-up the threads
    vTaskDelay(2);
}

#else

#endif // MICROPY_PY_THREAD

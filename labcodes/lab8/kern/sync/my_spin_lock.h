#ifndef __MY_LOCK_H__
#define __MY_LOCK_H__

#include <sem.h>
extern semaphore_t global_print_lock;

void print_lock();
void print_unlock();

#endif
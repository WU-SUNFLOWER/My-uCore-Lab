#include <my_spin_lock.h>
#include <sched.h>

semaphore_t global_print_lock;

void inline print_lock() {
    down(&global_print_lock);
}

void inline print_unlock() {
    up(&global_print_lock);
}
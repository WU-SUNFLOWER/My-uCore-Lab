#include <my_spin_lock.h>
#include <sched.h>

// 加锁函数
void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&(lock->locked), 1)) {
        schedule();
    }
}

// 解锁函数
void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&(lock->locked));
}

// 初始化一个全局的自旋锁
spinlock_t global_print_lock = {0}; // 全局锁初始化为未锁定状态

// 这个结构体用于表示自旋锁
typedef struct {
    volatile int locked;
} spinlock_t;

void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

extern spinlock_t global_print_lock;
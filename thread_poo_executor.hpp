#ifndef __THREAD_POOL_EXECUTOR__
#define __THREAD_POOL_EXECUTOR__
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <chrono>

#define TIMEOUT std::cv_status::timeout
typedef std::function<void()> TASK;
typedef std::lock_guard<std::mutex> LOCK_GUARD;
typedef std::unique_lock<std::mutex> UNIQUE_LOCK;
typedef std::chrono::seconds SECOND_UNIT;

class BlockingTaskQueue
{
private:
    volatile bool is_exit = false;
    volatile size_t current_size = 0;
    size_t max_size = 0;
    std::queue<TASK> queue;
    std::mutex put_lock;
    std::mutex get_lock;
    std::mutex general_lock;
    std::condition_variable put_condition;
    std::condition_variable get_condition;
    void notify_get();
    void notify_put();

public:
    explicit BlockingTaskQueue(size_t size) : max_size(size){};
    ~BlockingTaskQueue(){};
    bool is_empty();
    bool is_full();
    size_t size();
    void put(TASK &&task);
    TASK &get();
    TASK &poll(const SECOND_UNIT timeout);
    void exit();
};

bool BlockingTaskQueue::is_empty()
{
    LOCK_GUARD lk(this->general_lock);
    return this->current_size == 0;
}

bool BlockingTaskQueue::is_full()
{
    LOCK_GUARD lk(this->general_lock);
    return this->current_size >= this->max_size;
}

size_t BlockingTaskQueue::size()
{
    LOCK_GUARD lk(this->general_lock);
    return this->current_size;
}

void BlockingTaskQueue::notify_put()
{
    LOCK_GUARD lk(this->general_lock);
    this->current_size--;
    this->put_condition.notify_one();
}

void BlockingTaskQueue::notify_get()
{
    LOCK_GUARD lk(this->general_lock);
    this->current_size++;
    this->get_condition.notify_one();
}

void BlockingTaskQueue::put(TASK &&task)
{
    UNIQUE_LOCK unique_put_lock(this->put_lock);
    while ((!this->is_exit) && this->is_full())
    {
        this->put_condition.wait(unique_put_lock);
    }
    if (!this->is_exit)
    {
        this->queue.emplace(std::forward<TASK>(task));
        this->notify_get();
    }
}

TASK &BlockingTaskQueue::get()
{
    UNIQUE_LOCK unique_get_lock(this->get_lock);
    static TASK task_holder;
    while ((!this->is_exit) && this->is_empty())
    {
        this->get_condition.wait(unique_get_lock);
    }
    if (this->is_exit)
    {
        task_holder = nullptr;
        return task_holder;
    }
    task_holder = std::move(this->queue.front());
    this->queue.pop();
    this->notify_put();
    return task_holder;
}

TASK &BlockingTaskQueue::poll(const SECOND_UNIT timeout)
{
    UNIQUE_LOCK unique_get_lock(this->get_lock);
    static TASK task_holder;
    while (this->is_empty())
    {
        if (this->is_exit || (this->get_condition.wait_for(unique_get_lock, timeout) == TIMEOUT))
        {
            task_holder = nullptr;
            return task_holder;
        }
    }
    if (this->is_exit)
    {
        task_holder = nullptr;
        return task_holder;
    }
    task_holder = std::move(this->queue.front());
    this->queue.pop();
    this->notify_put();
    return task_holder;
}

void BlockingTaskQueue::exit()
{
    this->is_exit = true;
    this->get_condition.notify_all();
    this->put_condition.notify_all();
}


class ThreadPoolExecutor
{
private:
    typedef struct THREAD_POOL_DATA
    {
        volatile size_t is_working = 0;
        volatile size_t worker_size = 0;
        size_t core_size = 1;
        size_t max_size = 1;
        size_t queue_size = 1;
        unsigned long keep_alive_seconds = 8;
    } POOL_DATA;

    enum POOL_STATE
    {
        WORKING,
        WAITTING,
        EXITING,
        TERMINATED
    };

    std::mutex pool_lock;
    std::mutex task_lock;
    std::mutex join_lock;
    std::condition_variable join_condition;
    volatile POOL_DATA datas;
    volatile POOL_STATE pool_state = WAITTING;
    BlockingTaskQueue *task_queue;
    SECOND_UNIT alive_time;

    size_t get_worker_size();
    void minus_worker_size();
    void add_work_state();
    void minus_work_state();
    void set_state(const POOL_STATE &state);
    void create_worker();
    void thread_loop();
    void get_task(TASK *result);

public:
    explicit ThreadPoolExecutor(size_t core_size, size_t max_size, size_t queue_size, unsigned long keep_alive_second);
    ~ThreadPoolExecutor() { delete this->task_queue; };
    void prestart_all_core();
    void execute(TASK &&task);
    void join();
    void exit();
};

ThreadPoolExecutor::ThreadPoolExecutor(size_t core_size, size_t max_size, size_t queue_size, unsigned long keep_alive_second)
{
    this->datas.core_size = core_size;
    this->datas.max_size = max_size;
    this->datas.queue_size = queue_size;
    this->datas.keep_alive_seconds = keep_alive_second;
    this->alive_time = SECOND_UNIT(this->datas.keep_alive_seconds);
    this->task_queue = new BlockingTaskQueue(queue_size);
}

void ThreadPoolExecutor::prestart_all_core()
{
    for (size_t i = 0; i < this->datas.core_size; i++)
    {
        this->create_worker();
    }
}

void ThreadPoolExecutor::create_worker()
{
    LOCK_GUARD lk(this->pool_lock);
    this->datas.worker_size++;
    std::thread(&ThreadPoolExecutor::thread_loop, this).detach();
}

size_t ThreadPoolExecutor::get_worker_size()
{
    LOCK_GUARD lk(this->pool_lock);
    return this->datas.worker_size;
}

void ThreadPoolExecutor::minus_worker_size()
{
    LOCK_GUARD lk(this->pool_lock);
    this->datas.worker_size--;
    if (this->datas.worker_size == 0)
    {
        this->pool_state = TERMINATED;
    }
}

void ThreadPoolExecutor::add_work_state()
{
    LOCK_GUARD lk(this->pool_lock);
    this->datas.is_working++;
    this->set_state(WORKING);
}

void ThreadPoolExecutor::minus_work_state()
{
    LOCK_GUARD lk(this->pool_lock);
    this->datas.is_working--;
    if (this->datas.is_working == 0)
    {
        this->set_state(WAITTING);
        this->join_condition.notify_all();
    }
}

void ThreadPoolExecutor::set_state(const POOL_STATE &state)
{
    if ((this->pool_state != EXITING) && (this->pool_state != TERMINATED))
    {
        this->pool_state = state;
    }
}

void ThreadPoolExecutor::thread_loop()
{
    TASK current;
    while (true)
    {
        this->get_task(&current);
        if ((current == nullptr) || (this->pool_state == EXITING))
        {
            break;
        }
        this->add_work_state();
        current();
        this->minus_work_state();
    }
}

void ThreadPoolExecutor::get_task(TASK *result)
{
    while (true)
    {
        *result = std::move(this->task_queue->poll(this->alive_time));
        {
            LOCK_GUARD lk(this->task_lock);
            if (this->pool_state == EXITING)
            {
                *result = nullptr;
                this->minus_worker_size();
                break;
            }
            if ((*result == nullptr) && (this->get_worker_size() > this->datas.core_size))
            {
                this->minus_worker_size();
                break;
            }
            else if (*result != nullptr)
            {
                break;
            }
        }
    }
}

void ThreadPoolExecutor::execute(TASK &&task)
{
    if (this->task_queue->is_full())
    {
        if (this->get_worker_size() < this->datas.max_size)
        {
            this->create_worker();
        }
    }
    this->task_queue->put(std::forward<TASK>(task));
}

void ThreadPoolExecutor::join()
{
    if (this->pool_state != EXITING)
    {
        UNIQUE_LOCK lk(this->join_lock);
        while (!((this->pool_state == WAITTING) && (this->task_queue->is_empty())))
        {
            this->join_condition.wait(lk);
        }
    }
}

void ThreadPoolExecutor::exit()
{
    if (this->pool_state != TERMINATED)
    {
        this->pool_state = EXITING;
    }
    this->task_queue->exit();
    while (this->pool_state != TERMINATED)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

#endif

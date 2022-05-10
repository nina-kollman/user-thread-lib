#include <iostream>
#include <vector>
#include <set>
#include <armadillo>
#include <cstdio>
#include <csignal>
#include <signal.h>
#include <ctime>
#include <cstdlib>
#include <csetjmp>
#include <unistd.h>
#include <map>
#include <queue>
#include "uthreads.h"
#include <algorithm>


/**Macros**/
#define JB_SP 6
#define JB_PC 7
#define INIT_ERROR "thread library error: quantum_usecs parameter need to be positive"
#define SLEEP_ERROR "thread library error: sleep of main thread"
#define TERMINATION_ERROR_1 "thread library error: termination of main thread"
#define TERMINATION_ERROR_2 "thread library error: tried to terminate an nonexistent thread"
#define BLOCK_ERROR_1 "thread library error: tried to block the main thread"
#define BLOCK_ERROR_2 "thread library error: tried to block an nonexistent thread"
#define TIMER_ERROR "system error: timer error"
#define SIGCATION_ERROR "system error: sigcation system call failed"
#define SIG_ADD_SET_ERROR "system error: sigaddset system call failed"
#define SIGPROCMASK_ERROR "system error: sigprocmask system call failed"
#define RESUME_ERROR "system error: tried to resume an nonexistent thread"
#define GET_QUANTUM_ERROR "system error: tried to get the num quantum of an nonexistent thread"
#define EMPTY_SET_ERROR "system error: sigemptyset call failed"
#define FAILURE (-1)
#define SUCCESS 0


/**Data Structures and Globals**/
typedef unsigned long address_t;
typedef void (*thread_entry_point)(void);
using namespace std;

enum thread_state {
    READY,
    BLOCKED,
    RUNNING
};

typedef struct{
    int id;
    thread_state state;
    int num_of_quantum;
    char *stack;
    void (*thread_func) ();
    sigjmp_buf env;
}thread;



std::vector<int> *free_ids;
std::vector <int> *taken_ids;
std::map<int, thread*> *thread_map;
std::multimap<int,int> *sleep_map;
std::deque<int> *ready_queue;
sigset_t maskSignals{};
//queue<int> ready_queue;
int gotit = 0;
int total_quantum;
struct itimerval timer;
struct sigaction sa;
thread *running_thread;
//sigjmp_buf env[MAX_THREAD_NUM];

void add_thread_to_ready_queue(int  cur_thread);

/**
 * Frees the stack memory of a given thread
 * @param curr_thread_to_free given thread to the thread stack
 */
void free_thread_stack(thread * curr_thread_to_free)
{
    // free(curr_thread_to_free->stack);
    delete [] curr_thread_to_free->stack;
    curr_thread_to_free->stack = nullptr;
}

/**
 * Cleans all the program memory
 */
void clean_memory()
{
    // frees all allocated stack for each thread in the map
    for(auto it : *thread_map)
    {
        auto curr_thread = it.second;
        free_thread_stack(curr_thread);
        delete curr_thread;
        curr_thread = nullptr;
    }
    delete ready_queue;
    delete sleep_map;
    delete thread_map;
    delete free_ids;
    delete taken_ids;
}

/**
 * Sets the programs timer
 */
void set_timer()
{
    auto ret = setitimer(ITIMER_VIRTUAL, &timer, NULL);
    if (ret<0)
    {
        std::cerr << TIMER_ERROR << std::endl;
        clean_memory();
        exit(1);
    }
}


/**
 * This function masking or unmasking the signals.
 * @param to_mask True to mask False to unmask.
 */
void mask_signals(bool to_mask){
    int cur_action = to_mask ? SIG_BLOCK : SIG_UNBLOCK;
    int ret = sigprocmask(cur_action,&maskSignals, nullptr);
    if(ret)
    {
        std::cerr << SIGPROCMASK_ERROR << std::endl;
        clean_memory();
        exit(1);
    }

}

/**
 * Removes the given thread id from the ready queue
 * @param id The given id
 */
void remove_tid_from_ready_queue(int id)
{
    for (auto it = ready_queue->begin(); it != ready_queue->end(); ++it)
    {
        if(*it == id)
        {
            ready_queue->erase(it);
            return;
        }
    }
}

/**
 * Address translation to a given address
 * @param addr the given address
 * @return the translated address
 */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

/**
 * This function saves the given thread data
 * @param cur_thread the given thread
 * @return return value of sigsetjmp
 */
int set_thread_data(thread  *cur_thread)
{
    return sigsetjmp(cur_thread->env,1);
}

/**
 * This function jumps to the given thread data
 * @param cur_thread the given thread
 */
void jump_to_thread(thread * cur_thread)
{
    siglongjmp(cur_thread->env,1);
}

/**
 * Sets the given set to empty
 * @param set the given set
 * @return sigemptyset return value
 */
void set_empty_signal_set(sigset_t *set)
{
    if(sigemptyset(set))
     {
         std::cerr << EMPTY_SET_ERROR << std::endl;
         clean_memory();
         exit(1);
     }
}

/**
 * This function resuming all the threads that should resume at this quantum.
 */
void resuming_all_sleeping_threads(){
    int cur_quantum = total_quantum;
    for (auto & itr : *sleep_map)   // running on the current threads who should resume on this quantum
        if (itr.first == cur_quantum)
        {
            uthread_resume(itr.second);
        }
    sleep_map->erase(total_quantum);
}

/**
 * This function takes the next thread from the ready queue and runs it.
 * it also calls the resuming function to wake up the sleeping threads.
 */
void next_running_thread()
{
//    std::cout << "In next thread" << std::endl;
    mask_signals(true);
//    std::cout << "after mask" << std::endl;
//    fflush(stdout);
    static int cur_thread = 0;
    //auto cur_thread_pointer = (*thread_map)[cur_thread];
    if(set_thread_data(running_thread) == 1)
    {
        mask_signals(false);
        return;
    }
    cur_thread = ready_queue->back(); // access
    ready_queue->pop_back(); // erase
    total_quantum++;
    resuming_all_sleeping_threads();
    (*thread_map)[cur_thread]->state = RUNNING;
    (*thread_map)[cur_thread]->num_of_quantum++;
    if(running_thread->state == RUNNING)
    {
        running_thread->state = READY;
        add_thread_to_ready_queue(running_thread->id);
    }
    running_thread = (*thread_map)[cur_thread];
//    std::cout << "before jump to next thread" << std::endl;
//    fflush(stdout);
    auto cur_thread_pointer = (*thread_map)[cur_thread];
    jump_to_thread(cur_thread_pointer);
//    std::cout << "after jump to next thread" << std::endl;
//    fflush(stdout);
    mask_signals(false);
}

/**
 * This function handle the sigvt alarm sent by the timer.
 * Set a new thread in running.
 * @param sig the alarm index from
 */
void sigvtalrm_handler(int sig)
{
    next_running_thread();
}

/**
 * Setups a thread inside the env[] array.
 * @param tid thread tid
 * @param stack thread stack pointer
 * @param entry_point thread function
 */
void setup_thread(thread *thread)
{
    // initializes env[tid] to use the right stack, and to run from the function 'entry_point', when we'll use
    // siglongjmp to jump into the thread.
    address_t sp = (address_t) thread->stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) thread->thread_func;
    set_thread_data(thread);
//    std::cout << "before translate" << std::endl;
//    fflush(stdout);
    (thread->env->__jmpbuf)[JB_SP] = translate_address(sp);
//    std::cout << "after translate 1" << std::endl;
//    fflush(stdout);
    (thread->env->__jmpbuf)[JB_PC] = translate_address(pc);
//    std::cout << "after translate 2" << std::endl;
//    fflush(stdout);
    set_empty_signal_set(&thread->env->__saved_mask);
}

/**
 * Deleting a given id from the taken id vector.
 * @param index the given idex
 */
void delete_id_from_taken_id(int tid)
{
    taken_ids->erase(std::remove(taken_ids->begin(), taken_ids->end(), tid));
}


/**
 * Deletes a given thread id by position index from the free_id vector
 * @param index the given index of the thread id
 */
void delete_id_from_free_id(int index)
{
    free_ids->erase(free_ids->begin()+index);
}

/**
 * Adds a given thread id by to the free_id vector
 * @param tid the given index of the thread id
 */
void add_id_to_free_id(int tid)
{
    free_ids->push_back(tid);
}

/**
 * Adding a given thread to the map<id,thread>
 * @param cur_thread the given thread
 */
void add_thread_to_map(thread* cur_thread){
    (*thread_map)[cur_thread->id] = cur_thread;
}

/**
 * removing a given thread to the map<id,thread>
 * @param tid the given thread index
 */
void remove_thread_from_map(int tid){
    thread_map->erase(tid);}

/**
 * Adding a given thread id to the ready queue
 * @param cur_thread the given thread id
 */
void add_thread_to_ready_queue(int  cur_thread)
{
    ready_queue->push_front(cur_thread);
}


/**
 * Adding a given thread id to the taken ids vector
 * @param id the given thread id
 */
void add_thread_id_to_taken_id(int id)
{
    taken_ids->push_back(id);
}





/**
 * @brief initializes the thread library.
 *
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs){

    if(quantum_usecs<=0)
    {
        std::cerr << INIT_ERROR << std::endl;
        return FAILURE;
    }

    // Global pointers initialization
    ready_queue = new std::deque<int>;
    sleep_map = new std::multimap<int, int>;
    thread_map = new std::map<int,thread*>;
    free_ids = new std::vector<int>;
    taken_ids = new std::vector<int>;

    // Fill all the possible ids
    for (int i = 0; i<MAX_THREAD_NUM; i++)
    {
        add_id_to_free_id(i);
    }

    // Install timer_handler as the signal handler for SIGVTALRM.
    sa.sa_handler = &sigvtalrm_handler;
    if (sigaction(SIGVTALRM, &sa, NULL))
    {
        std::cerr << SIGCATION_ERROR << std::endl;
        clean_memory();
        exit(1);
    }

    // Set empty set to mask signals
    if(sigemptyset(&maskSignals))
    {
        std::cerr << EMPTY_SET_ERROR << std::endl;
        clean_memory();
        exit(1);
    }
    // add SIGVTALARM to mask signals
    if (sigaddset(&maskSignals, SIGVTALRM))
    {
        std::cerr << SIG_ADD_SET_ERROR << std::endl;
        clean_memory();
        exit(1);
    }

    // Configure the timer to expire after quantum_usecs... */
    timer.it_value.tv_sec = ((long)quantum_usecs / 1000000); // first time interval, seconds part
    timer.it_value.tv_usec = ((long)quantum_usecs % 1000000);        // first time interval, microseconds part

    // configure the timer to expire every quantum_usecs after that.
    timer.it_interval.tv_sec = ((long)quantum_usecs / 1000000);   // following time intervals, seconds part
    timer.it_interval.tv_usec = ((long)quantum_usecs % 1000000);    // following time intervals, microseconds part

    // Start a virtual timer. It counts down whenever this process is executing.
    set_timer();

    auto cur_thread = new thread ;
    cur_thread->id =0;
    cur_thread->num_of_quantum = 1;
    cur_thread->state = RUNNING;
    cur_thread->thread_func = nullptr;
    set_thread_data(cur_thread);
    //set_empty_signal_set(&cur_thread->env->__saved_mask);
    total_quantum = 1;
    add_thread_to_map(cur_thread);
    add_thread_id_to_taken_id(cur_thread->id);
    delete_id_from_free_id(cur_thread->id);
    running_thread = cur_thread;
    return SUCCESS;
}

/**
 * returns the minimum free id and delete it from the free id vector
 * If there is no free id then returns -1
 */
int get_min_id()
{
    // There is no free id left
    if (free_ids->empty())
    {
        return FAILURE;
    }
    int curr_min = MAX_THREAD_NUM;
    int index = -1;
    for (int i = 0; i < free_ids->size(); ++i) {
        if(curr_min > (*free_ids)[i])
        {
            curr_min = (*free_ids)[i];
            index = i;
        }
    }
    delete_id_from_free_id(index);
    return curr_min;

}


/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point)
{
    mask_signals(true);
    int thread_id= get_min_id(); // deletes the id from free_id vector and checks if there are ids left
    if (thread_id == FAILURE)
    {
        mask_signals(false);
        return FAILURE;
    }
    char * stack = new char [STACK_SIZE];

    auto cur_thread = new thread;
    cur_thread->id = thread_id;
    cur_thread->state = READY;
    cur_thread->num_of_quantum=0;
    cur_thread->stack = stack;
    cur_thread->thread_func = entry_point;
    setup_thread(cur_thread);
    add_thread_to_map(cur_thread);
    add_thread_to_ready_queue(cur_thread->id);
    add_thread_id_to_taken_id(cur_thread->id);
    mask_signals(false);

    return SUCCESS;
}

/**
 * This function does a self termination of a given thread id and makes the next
 * thread from the ready queue as the running thread.
 * @param tid The id of the thread that will terminate.
 */
void self_termination(int tid)
{
    int next_thread_id = ready_queue->back(); // access
    ready_queue->pop_back(); // erase
    total_quantum++;
    resuming_all_sleeping_threads();
    add_id_to_free_id(tid);
    delete_id_from_taken_id(tid);
    remove_tid_from_ready_queue(tid);
    thread *cur_thread =(*thread_map)[tid];
    free_thread_stack(cur_thread);
    remove_thread_from_map(tid);
    auto next_thread_pointer = (*thread_map)[next_thread_id];
    next_thread_pointer->state = RUNNING;
    next_thread_pointer->num_of_quantum++;
    set_timer();
    running_thread = next_thread_pointer;
    jump_to_thread(next_thread_pointer);
    mask_signals(false);
}


/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid)
{
    mask_signals(true);

    // Case thread 0
    if(tid == 0)
    {
        clean_memory();
        std::cerr << TERMINATION_ERROR_1 << std::endl;
        exit(0);
    }

    // Case the tid does not exist
    if (thread_map->find(tid) == thread_map->end())
    {
        std::cerr << TERMINATION_ERROR_2 << std::endl;
        mask_signals(false);
        return FAILURE;
    }

    int tid_running_thread = running_thread->id;
    //Case self termination
    if(tid_running_thread == tid)
    {
        self_termination(tid);
        return SUCCESS;
    }
    thread *cur_thread =(*thread_map)[tid];
    free_thread_stack(cur_thread);
    if (cur_thread->stack != NULL) //todo delete
        {
        mask_signals(false);
//        std::cout<<"stack not null"<<std::endl;
        }
    delete cur_thread;
    cur_thread = nullptr;
    if (cur_thread!= nullptr) //todo delete
    {
        mask_signals(false);
        std::cout<<"im not null"<<std::endl;
    }
    remove_thread_from_map(tid);
    add_id_to_free_id(tid);
    delete_id_from_taken_id(tid);
    remove_tid_from_ready_queue(tid);
    mask_signals(false);
    return SUCCESS;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid)
{
    mask_signals(true);

    // Case thread 0 - Not allowed
    if(tid == 0)
    {
        std::cerr << BLOCK_ERROR_1 << std::endl;
        mask_signals(false);
        return FAILURE;
    }

    // Case the tid does not exist
    if (thread_map->find(tid) == thread_map->end())
    {
        std::cerr << BLOCK_ERROR_2 << std::endl;
        mask_signals(false);
        return FAILURE;
    }

    thread *curr_tread = (*thread_map)[tid];
    if(curr_tread->state == BLOCKED)
    {
        return SUCCESS;
    }else if(curr_tread->state == RUNNING)
    {
        set_timer(); // restarting the timer in order to get an entire quantum for the next running thread
        curr_tread->state = BLOCKED;
        next_running_thread();
    }else // Was in READY
    {
        curr_tread->state = BLOCKED;
        remove_tid_from_ready_queue(curr_tread->id);
        mask_signals(false);
    }
    mask_signals(false);
    return SUCCESS;
}

//void print_hallo()
//{
//    std::cout << "T Function" << std::endl;
//}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid)
{
    mask_signals(true);

    // Case the tid does not exist
    if (thread_map->find(tid) == thread_map->end())
    {
        std::cerr << RESUME_ERROR << std::endl;
        mask_signals(false);
        return FAILURE;
    }
    thread *curr_tread = (*thread_map)[tid];
    if (curr_tread->state == BLOCKED)
    {
        curr_tread->state = READY;
        add_thread_to_ready_queue(curr_tread->id);
        mask_signals(false);
        return SUCCESS;
    }
    mask_signals(false);
    return SUCCESS;
}


/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY threads list.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid==0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums)
{
    mask_signals(true); // todo check if everyone sleeps
    // Case thread 0
    if(running_thread->id == 0)
    {
        clean_memory();
        std::cerr << SLEEP_ERROR << std::endl;
        mask_signals(false);
        return FAILURE;
    }
    int wake_up_quantum = total_quantum+num_quantums+1;
    sleep_map->insert(make_pair(wake_up_quantum, running_thread->id));
    running_thread->state = BLOCKED;
    set_timer(); // Set timer in order to get an entire quantum, for next running thread
    next_running_thread(); // scheduling decision
    mask_signals(false);
    return SUCCESS;
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid()
{
    return running_thread->id;
}


/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums()
{
    return total_quantum;
}



/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid)
{
    mask_signals(true);

    // Case the tid does not exist
    if (thread_map->find(tid) == thread_map->end())
    {
        std::cerr << GET_QUANTUM_ERROR << std::endl;
        mask_signals(false);
        return FAILURE;
    }

    thread* cur_thread = (*thread_map)[tid];
    if(cur_thread->state == RUNNING)
    {
        mask_signals(false);
        return cur_thread->num_of_quantum + 1;
    }
    mask_signals(false);
    return cur_thread->num_of_quantum;

}

/****************************** Functions for test only **************************************************************/

//void thread7()
//{
//    while(true){
//        std::cout << "In thread 7" << std::endl;
//    }
//}
//
//void thread1()
//{
//    std::cout << "In thread 1" << std::endl;
//    fflush(stdout);
//    int index = 0;
//    while(index < 100000){
//        std::cout << "In thread 1" << std::endl;
//        index+=1;
//    }
////    uthread_terminate(4);
////    std::cout << "T1 terminated T4" << std::endl;
//    uthread_terminate(1);
//}
//
//void thread2()
//{
//    int index = 0;
//    while(index < 100){
//        std::cout << "In thread 2" << std::endl;
//        index+=1;
//    }
//    uthread_block(3);
//    std::cout << "After Block 3" << std::endl;
//    uthread_block(2);
//    std::cout << "Resume T2 2222222222222222222222222" << std::endl;
//    while (true){}
//
//}
//
//void thread3()
//{
//    while(true){
//        std::cout << "In thread 3" << std::endl;
//    }
//}
//
//void thread4()
//{
//    uthread_resume(2);
//    std::cout << "Before Sleep 4" << std::endl;
//    uthread_sleep(2);
//    std::cout << "After Sleep 4" << std::endl;
//    while(true){
//        std::cout << "In thread 4" << std::endl;
//    }
//}
//
//void thread5()
//{
//    uthread_spawn(thread7);
//    while(true){
//        std::cout << "In thread 5" << std::endl;
//    }
//}
//
//void thread6()
//{
//    while(true){
//        std::cout << "In thread 6" << std::endl;
//    }
//}
//
//
//
//void thread8()
//{
//    while(true){
//        std::cout << "In thread 8" << std::endl;
//    }
//}
//
//void thread9()
//{
//    while(true){
//        std::cout << "In thread 9" << std::endl;
//    }
//}
//
//void thread10()
//{
//    while(true){
//        std::cout << "In thread 10" << std::endl;
//    }
//}
//
//void thread11()
//{
//    while(true){
//        std::cout << "In thread 11" << std::endl;
//    }
//}
//
//
//void wait_next_quantum()
//{
//    int quantum = uthread_get_quantums(uthread_get_tid());
//    while (uthread_get_quantums(uthread_get_tid()) == quantum)
//    {}
//    return;
//}



//int main()
//{
//    int one_sec =1000000;
//    int two_sec = 2000000;
//    int three_sec = 3000000;
//
//    //uthread_init(-6); // Check invalid quantum
//
//    uthread_init(one_sec);
//    if(thread_map->size() == 1)
//    {
//        std::cout << "map size correct" << std::endl;
//    }
//
//
////    for (int i = 1; i < MAX_THREAD_NUM; ++i) {
////        uthread_spawn(thread10);
////        std::cout << "created thread:" + to_string(i) << std::endl;
////    }
////    std::cout << "after 100 threads"<< std::endl;
////    if(uthread_spawn(thread2) == -1)
////    {
////        std::cout << "worked"<< std::endl;
////    }
//    uthread_spawn(thread1);
//    std::cout << "after T1"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread2);
//    std::cout << "after T2"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread3);
//    std::cout << "after T3"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread4);
//    std::cout << "after T4"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread5);
//    std::cout << "after T5"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread6);
//    std::cout << "after T6"<< std::endl;
//    fflush(stdout);
//    uthread_resume(3);
//    if(uthread_sleep(3)==-1)
//        std::cout << "Worked"<< std::endl;
//

//    uthread_block(0);

//    std::cout << to_string(running_thread->id) + "NEED TO BE 0" << std::endl;
//    uthread_terminate(5);
//    std::cout << "after T5 termination"<< std::endl;
//    fflush(stdout);
//    uthread_spawn(thread5);


//    uthread_terminate(0); // try to terminate the main thread
//      if(uthread_terminate(7)==-1) // try to terminate a not exists id thread
//          {
//          std::cout << "working"<< std::endl;
//
//          }

//    int num = 0;
//    for(;;)
//    {
////        num += 1;
////        std::cout << num << std::endl;
//    }

//}
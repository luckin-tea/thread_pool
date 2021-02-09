#include <chrono>
#include <iostream>
#include "thread_pool_executor.hpp"

std::mutex cout_lk;
unsigned long int counter = 0;
const int size = 20;
int unsorted[size] = {3, 8, 4, 21, 38, 3, 1, 2, 14, 5, 15, 4, 34, 1, 12, 5, 6, 7, 4, 21};
int sorted[size];
int index = 0;

void sleep(unsigned long long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void sleep_sort(int number)
{
    sleep(number * 10);
    {
        LOCK_GUARD guard(cout_lk);
        sorted[index++] = number;
    }
}

int main(void)
{

    ThreadPoolExecutor p(20, 20, 1, 2);
    p.prestart_all_core();

    std::cout << "[BEFORE]: ";
    for (int i = 0; i < size; i++)
    {
        std::cout << unsorted[i] << ' ';
    }
    std::cout << std::endl;

    for (int i = 0; i < size; i++)
    {
        p.execute(std::bind(sleep_sort, unsorted[i]));
    }
    p.join();

    std::cout << "[SORTED]: ";
    for (int i = 0; i < size; i++)
    {
        std::cout << sorted[i] << ' ';
    }
    std::cout << std::endl;

    p.exit();
    system("pause");
}

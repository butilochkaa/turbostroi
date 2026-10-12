#pragma once
#include <thread>
#include <vector>
#include <string>
#include <atomic>

#if defined(_WIN32)
#include <Windows.h>
#include <array>
#include "version.h"
// массив структур group affinity для поддержки до 32 процессорных групп на винде
using CPU_SET = std::array<GROUP_AFFINITY, 32>;
HANDLE CurrentThread();
DWORD CurrentCPU();
#elif defined(POSIX)
#include <sched.h>
// стандартная маска ядер в linux
using CPU_SET = cpu_set_t;
pid_t CurrentThread();
int CurrentCPU();
#else
using CPU_SET = size_t;
size_t CurrentThread();
size_t CurrentCPU();
#endif

// общее число ядер/потоков процессора
extern size_t g_ProcessorCount;

// маски и списки ядер для главного потока сервера (srcds) и потоков вагонов
extern CPU_SET g_MainThreadGroupAffinity;
extern CPU_SET g_SimThreadGroupAffinity;
extern std::vector<size_t> g_MainCoresList;
extern std::vector<size_t> g_SimCoresList;

// атомарный счетчик для равномерного round-robin распределения вагонов по ядрам
extern std::atomic<size_t> g_TrainRoundRobinIndex;

// парсит человеческую строку с ядрами (all, 0-1, 2-15, 0,2,4, маски)
bool ParseCoresString(const char* value, size_t maxCPUs, CPU_SET& group, std::vector<size_t>& coresList);

// применяет маску группы ядер
bool SetThreadGroup(CPU_SET& group, const char* value);
bool SetThreadGroup(CPU_SET& group, std::vector<size_t>& coresList, const char* value);

// ставит потоку полную маску группы
bool SetAffinityMask(std::thread::native_handle_type handle, const CPU_SET& group);

// точечно сажает поток на конкретное ядро (нужно для round-robin пина вагонов)
bool SetThreadToCore(std::thread::native_handle_type handle, size_t coreId);

// разруливает привязку вагона: если включен spread — пинит на свое ядро, иначе дает общую маску
bool SetTrainThreadAffinity(std::thread::native_handle_type handle, unsigned int wagonIndex, size_t& assignedCore);

// форматирует список ядер в читаемый вид для консоли сервера (например "2-15" или "0,2,4")
std::string CoresListToString(const std::vector<size_t>& cores);

#pragma once
#include "ring_buffer.h"
#include "mutex.h"
#include <string>

struct lua_State;
class CCommand;

// потокобезопасная очередь для вывода print() из потоков симуляции в консоль gmod
class SharedPrint
{
public:
    SharedPrint() = default;

    // закинуть строку в очередь на печать
    void Push(std::string& str);
    void Push(const char* str);
    
    // напечатать все накопившиеся сообщения в главном потоке сервера
    void PrintAvailable();
    // очистить очередь печати
    void ClearPrintQueue();

    // lua-функция print() для виртуальной машины вагона
    static int PrintL(lua_State* L);

private:
    RingBuffer<std::string, 256> m_Queue;
    Mutex m_Mutex{};
};

extern SharedPrint g_SharedPrint;
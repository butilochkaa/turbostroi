#pragma once
#include <string>
#include <atomic>
#include <unordered_map>
#include "version.h"

// имя файла текущей библиотеки dll
extern std::string g_LibraryFileName;

// флаг принудительной остановки всех потоков симуляции (нужен при выгрузке модуля)
extern bool g_ForceThreadsFinished;

// текущее время сервера, синхронизируется через think хук со стороны движка
extern std::atomic<double> g_CurrentTime;

// задержка между тиками симуляции в микросекундах (по умолчанию 10000 мкс = 10 мс = 100 fps)
extern unsigned int g_ThreadTickrate;

// кэш текста загруженных lua скриптов систем
extern std::unordered_map<std::string, std::string> g_LoadedFilesCache;

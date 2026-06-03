#pragma once
#include <string>
#include <thread>
#include <atomic>
#include "Lib_CommandHandler.h"

class ModManager {
    std::thread modThread;
    std::atomic<bool> shouldStop{ false };
    void ModThreadWorker();
    void LoadModsGameThread();
    void UpdateGameThread(int DeltaTimeMs);
    CommandHandler cmdHandler;
public:
	ModManager();
	~ModManager() = default;
    void Message(const std::string&, uint32_t seq = 0);
	void LoadMods();
	void UnloadMods();
    void Update(int DeltaTimeMs);
};

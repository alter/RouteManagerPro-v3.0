// src/common/ShutdownCoordinator.h
#pragma once
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <windows.h>
#include "Logger.h"

class ShutdownCoordinator {
public:
    static ShutdownCoordinator& Instance() {
        static ShutdownCoordinator instance;
        return instance;
    }

    std::atomic<bool> isShuttingDown{ false };
    HANDLE shutdownEvent;

    void RegisterThread(const std::string& name, std::thread* thread) {
        std::lock_guard<std::mutex> lock(threadMutex);
        threads[name] = thread;
        Logger::Instance().Info("Thread registered: " + name);
    }

    void UnregisterThread(const std::string& name) {
        std::lock_guard<std::mutex> lock(threadMutex);
        threads.erase(name);
        Logger::Instance().Info("Thread unregistered: " + name);
    }

    void InitiateShutdown() {
        Logger::Instance().Info("ShutdownCoordinator: Initiating graceful shutdown");
        isShuttingDown = true;
        SetEvent(shutdownEvent);
    }

    bool WaitForThreads(std::chrono::milliseconds timeout) {
        Logger::Instance().Info("ShutdownCoordinator: Waiting for threads to complete");

        auto start = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(threadMutex);
        for (auto it = threads.begin(); it != threads.end(); ++it) {
            std::string name = it->first;
            std::thread* thread = it->second;

            if (thread && thread->joinable()) {
                auto remaining = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);

                if (remaining.count() <= 0) {
                    Logger::Instance().Error("Timeout waiting for thread: " + name);
                    return false;
                }

                Logger::Instance().Info("Waiting for thread: " + name);
                thread->join();
                Logger::Instance().Info("Thread completed: " + name);
            }
        }

        Logger::Instance().Info("All threads completed successfully");
        return true;
    }

private:
    ShutdownCoordinator() {
        shutdownEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    }

    ~ShutdownCoordinator() {
        if (shutdownEvent) CloseHandle(shutdownEvent);
    }

    std::mutex threadMutex;
    std::unordered_map<std::string, std::thread*> threads;
};
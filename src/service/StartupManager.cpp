// src/service/StartupManager.cpp
#include "StartupManager.h"
#include "../common/Logger.h"
#include "../common/Utils.h"
#include <windows.h>
#include <comdef.h>
#include <taskschd.h>
#include <format>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

bool StartupManager::SetStartWithWindows(bool enabled) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Logger::Instance().Error(std::format("StartupManager: CoInitializeEx failed: 0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }
    bool needUninit = SUCCEEDED(hr);

    bool result = false;
    ITaskService* pService = nullptr;
    ITaskFolder* pRootFolder = nullptr;

    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, reinterpret_cast<void**>(&pService));
    if (FAILED(hr)) {
        Logger::Instance().Error(std::format("StartupManager: Failed to create TaskScheduler: 0x{:08X}", static_cast<unsigned>(hr)));
        if (needUninit) CoUninitialize();
        return false;
    }

    hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        Logger::Instance().Error(std::format("StartupManager: Failed to connect to TaskScheduler: 0x{:08X}", static_cast<unsigned>(hr)));
        pService->Release();
        if (needUninit) CoUninitialize();
        return false;
    }

    hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
    if (FAILED(hr)) {
        pService->Release();
        if (needUninit) CoUninitialize();
        return false;
    }

    if (!enabled) {
        hr = pRootFolder->DeleteTask(_bstr_t(TASK_NAME), 0);
        if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            Logger::Instance().Info("StartupManager: Removed startup task");
            result = true;
        } else {
            Logger::Instance().Error(std::format("StartupManager: Failed to delete task: 0x{:08X}", static_cast<unsigned>(hr)));
        }
        pRootFolder->Release();
        pService->Release();
        if (needUninit) CoUninitialize();
        return result;
    }

    // Create new task
    ITaskDefinition* pTask = nullptr;
    hr = pService->NewTask(0, &pTask);
    if (FAILED(hr)) {
        pRootFolder->Release();
        pService->Release();
        if (needUninit) CoUninitialize();
        return false;
    }

    // Settings: run whether user is logged on or not isn't needed,
    // we want it to run at logon with highest privileges (admin)
    ITaskSettings* pSettings = nullptr;
    if (SUCCEEDED(pTask->get_Settings(&pSettings))) {
        pSettings->put_StartWhenAvailable(VARIANT_TRUE);
        pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")); // No time limit
        pSettings->Release();
    }

    // Principal: run with highest privileges (required for route management)
    IPrincipal* pPrincipal = nullptr;
    if (SUCCEEDED(pTask->get_Principal(&pPrincipal))) {
        pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        pPrincipal->Release();
    }

    // Registration info
    IRegistrationInfo* pRegInfo = nullptr;
    if (SUCCEEDED(pTask->get_RegistrationInfo(&pRegInfo))) {
        pRegInfo->put_Description(_bstr_t(L"RouteManagerPro - automatic route management at system startup"));
        pRegInfo->put_Author(_bstr_t(L"RouteManagerPro"));
        pRegInfo->Release();
    }

    // Trigger: at logon (runs after services, before other autostart apps)
    ITriggerCollection* pTriggerCollection = nullptr;
    if (SUCCEEDED(pTask->get_Triggers(&pTriggerCollection))) {
        ITrigger* pTrigger = nullptr;
        if (SUCCEEDED(pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger))) {
            ILogonTrigger* pLogonTrigger = nullptr;
            if (SUCCEEDED(pTrigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&pLogonTrigger)))) {
                pLogonTrigger->put_Id(_bstr_t(L"LogonTrigger"));
                // Delay ensures services are fully initialized
                pLogonTrigger->put_Delay(_bstr_t(L"PT5S"));
                pLogonTrigger->Release();
            }
            pTrigger->Release();
        }
        pTriggerCollection->Release();
    }

    // Action: run the executable with working directory set to binary location
    IActionCollection* pActionCollection = nullptr;
    if (SUCCEEDED(pTask->get_Actions(&pActionCollection))) {
        IAction* pAction = nullptr;
        if (SUCCEEDED(pActionCollection->Create(TASK_ACTION_EXEC, &pAction))) {
            IExecAction* pExecAction = nullptr;
            if (SUCCEEDED(pAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&pExecAction)))) {
                std::wstring exePath = GetExecutablePath();
                std::wstring exeDir = GetExecutableDirectory();

                pExecAction->put_Path(_bstr_t(exePath.c_str()));
                pExecAction->put_WorkingDirectory(_bstr_t(exeDir.c_str()));

                Logger::Instance().Info(std::format("StartupManager: Exe='{}', WorkDir='{}'",
                    Utils::WStringToString(exePath),
                    Utils::WStringToString(exeDir)));

                pExecAction->Release();
            }
            pAction->Release();
        }
        pActionCollection->Release();
    }

    // Register the task (create or update)
    IRegisteredTask* pRegisteredTask = nullptr;
    hr = pRootFolder->RegisterTaskDefinition(
        _bstr_t(TASK_NAME),
        pTask,
        TASK_CREATE_OR_UPDATE,
        _variant_t(),
        _variant_t(),
        TASK_LOGON_INTERACTIVE_TOKEN,
        _variant_t(L""),
        &pRegisteredTask);

    if (SUCCEEDED(hr)) {
        Logger::Instance().Info("StartupManager: Successfully registered startup task");
        result = true;
        pRegisteredTask->Release();
    } else {
        Logger::Instance().Error(std::format("StartupManager: Failed to register task: 0x{:08X}", static_cast<unsigned>(hr)));
    }

    pTask->Release();
    pRootFolder->Release();
    pService->Release();
    if (needUninit) CoUninitialize();

    return result;
}

bool StartupManager::IsStartWithWindowsEnabled() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    bool needUninit = SUCCEEDED(hr);

    bool exists = false;
    ITaskService* pService = nullptr;
    ITaskFolder* pRootFolder = nullptr;

    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, reinterpret_cast<void**>(&pService));
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return false;
    }

    hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
    if (FAILED(hr)) {
        pService->Release();
        if (needUninit) CoUninitialize();
        return false;
    }

    hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
    if (SUCCEEDED(hr)) {
        IRegisteredTask* pTask = nullptr;
        hr = pRootFolder->GetTask(_bstr_t(TASK_NAME), &pTask);
        if (SUCCEEDED(hr)) {
            VARIANT_BOOL taskEnabled = VARIANT_FALSE;
            pTask->get_Enabled(&taskEnabled);
            exists = (taskEnabled == VARIANT_TRUE);
            pTask->Release();
        }
        pRootFolder->Release();
    }

    pService->Release();
    if (needUninit) CoUninitialize();

    return exists;
}

std::wstring StartupManager::GetExecutablePath() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::wstring(buffer);
}

std::wstring StartupManager::GetExecutableDirectory() {
    std::wstring path = GetExecutablePath();
    auto lastSlash = path.find_last_of(L"\\/");
    return (lastSlash != std::wstring::npos) ? path.substr(0, lastSlash) : L".";
}

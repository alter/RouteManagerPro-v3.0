// src/ui/ServiceClient.cpp
#include "ServiceClient.h"
#include "../common/Constants.h"
#include "../common/Logger.h"
#include <thread>
#include <chrono>

ServiceClient::ServiceClient() : pipe(INVALID_HANDLE_VALUE), connected(false) {
    Logger::Instance().Info("ServiceClient: Created, NOT connecting immediately");
}

ServiceClient::~ServiceClient() {
    Disconnect();
}

bool ServiceClient::Connect() {
    Logger::Instance().Info("ServiceClient::Connect - Starting connection attempt");

    pipe = CreateFileA(
        Constants::PIPE_NAME.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (pipe != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            connected = true;
            Logger::Instance().Info("ServiceClient::Connect - Successfully connected to service");
            return true;
        }
        else {
            Logger::Instance().Error("ServiceClient::Connect - Failed to set pipe mode: " + std::to_string(GetLastError()));
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }

    DWORD error = GetLastError();
    Logger::Instance().Debug("ServiceClient::Connect - CreateFile failed with error: " + std::to_string(error));

    if (error == ERROR_PIPE_BUSY) {
        Logger::Instance().Debug("ServiceClient::Connect - Pipe is busy");
        if (WaitNamedPipeA(Constants::PIPE_NAME.c_str(), 1000)) {
            return Connect();
        }
    }

    connected = false;
    Logger::Instance().Info("ServiceClient::Connect - Could not connect to service");
    return false;
}

void ServiceClient::Disconnect() {
    if (pipe != INVALID_HANDLE_VALUE) {
        Logger::Instance().Info("ServiceClient::Disconnect - Closing pipe connection");
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
    connected = false;
}

IPCResponse ServiceClient::SendMessage(const IPCMessage& message) {
    IPCResponse response;
    response.success = false;

    if (!connected) {
        response.error = "Not connected to service";
        return response;
    }

    std::vector<uint8_t> buffer(sizeof(IPCMessageType) + message.data.size());
    memcpy(buffer.data(), &message.type, sizeof(IPCMessageType));
    if (!message.data.empty()) {
        memcpy(buffer.data() + sizeof(IPCMessageType), message.data.data(), message.data.size());
    }

    DWORD bytesWritten;
    if (!WriteFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesWritten, nullptr)) {
        DWORD error = GetLastError();
        Logger::Instance().Error("ServiceClient::SendMessage - WriteFile failed: " + std::to_string(error));
        Disconnect();
        response.error = "Failed to write to pipe";
        return response;
    }

    const size_t INITIAL_BUFFER_SIZE = 65536;
    buffer.resize(INITIAL_BUFFER_SIZE);
    DWORD bytesRead = 0;

    BOOL readResult = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr);

    if (!readResult) {
        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA) {
            Logger::Instance().Debug("ServiceClient::SendMessage - More data available, resizing buffer");
            DWORD bytesAvailable = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr) && bytesAvailable > 0) {
                size_t totalSize = bytesRead + bytesAvailable;
                buffer.resize(totalSize);
                DWORD additionalBytesRead = 0;
                if (ReadFile(pipe, buffer.data() + bytesRead, bytesAvailable, &additionalBytesRead, nullptr)) {
                    bytesRead += additionalBytesRead;
                }
                else {
                    Logger::Instance().Error("ServiceClient::SendMessage - Failed to read additional data: " + std::to_string(GetLastError()));
                    Disconnect();
                    response.error = "Failed to read additional data from pipe";
                    return response;
                }
            }
        }
        else {
            Logger::Instance().Error("ServiceClient::SendMessage - ReadFile failed: " + std::to_string(error));
            Disconnect();
            response.error = "Failed to read from pipe";
            return response;
        }
    }

    if (bytesRead >= sizeof(bool) + sizeof(size_t)) {
        size_t offset = 0;
        response.success = *reinterpret_cast<bool*>(&buffer[offset]);
        offset += sizeof(bool);

        size_t dataSize = *reinterpret_cast<size_t*>(&buffer[offset]);
        offset += sizeof(size_t);

        if (dataSize > 0 && offset + dataSize <= bytesRead) {
            response.data.assign(buffer.begin() + offset, buffer.begin() + offset + dataSize);
            offset += dataSize;
        }

        if (offset + sizeof(size_t) <= bytesRead) {
            size_t errorSize = *reinterpret_cast<size_t*>(&buffer[offset]);
            offset += sizeof(size_t);

            if (errorSize > 0 && offset + errorSize <= bytesRead) {
                response.error.assign(buffer.begin() + offset, buffer.begin() + offset + errorSize);
            }
        }
    }

    return response;
}

ServiceStatus ServiceClient::GetStatus() {
    if (!connected) {
        return ServiceStatus();
    }

    IPCMessage msg;
    msg.type = IPCMessageType::GetStatus;

    auto response = SendMessage(msg);
    if (response.success) {
        return IPCSerializer::DeserializeServiceStatus(response.data);
    }

    Logger::Instance().Debug("ServiceClient::GetStatus - Failed to get status");
    return ServiceStatus();
}

ServiceConfig ServiceClient::GetConfig() {
    if (!connected) {
        ServiceConfig defaultConfig;
        defaultConfig.gatewayIp = "10.200.210.1";
        defaultConfig.metric = 1;
        defaultConfig.startMinimized = false;
        defaultConfig.startWithWindows = false;
        defaultConfig.aiPreloadEnabled = false;
        return defaultConfig;
    }

    IPCMessage msg;
    msg.type = IPCMessageType::GetConfig;

    auto response = SendMessage(msg);
    if (response.success) {
        return IPCSerializer::DeserializeServiceConfig(response.data);
    }

    Logger::Instance().Debug("ServiceClient::GetConfig - Failed to get config");
    return ServiceConfig();
}

void ServiceClient::SetConfig(const ServiceConfig& config) {
    if (!connected) return;

    IPCMessage msg;
    msg.type = IPCMessageType::SetConfig;
    msg.data = IPCSerializer::SerializeServiceConfig(config);

    SendMessage(msg);
}

std::vector<ProcessInfo> ServiceClient::GetProcesses() {
    if (!connected) return std::vector<ProcessInfo>();

    IPCMessage msg;
    msg.type = IPCMessageType::GetProcesses;

    auto response = SendMessage(msg);
    if (response.success) {
        return IPCSerializer::DeserializeProcessList(response.data);
    }

    return std::vector<ProcessInfo>();
}

void ServiceClient::SetSelectedProcesses(const std::vector<std::string>& processes) {
    if (!connected) return;

    IPCMessage msg;
    msg.type = IPCMessageType::SetSelectedProcesses;
    msg.data = IPCSerializer::SerializeStringList(processes);

    SendMessage(msg);
}

std::vector<RouteInfo> ServiceClient::GetRoutes() {
    if (!connected) return std::vector<RouteInfo>();

    IPCMessage msg;
    msg.type = IPCMessageType::GetRoutes;

    auto response = SendMessage(msg);
    if (response.success) {
        return IPCSerializer::DeserializeRouteList(response.data);
    }

    return std::vector<RouteInfo>();
}

void ServiceClient::ClearRoutes() {
    if (!connected) return;

    IPCMessage msg;
    msg.type = IPCMessageType::ClearRoutes;

    SendMessage(msg);
}

void ServiceClient::SetAIPreload(bool enabled) {
    if (!connected) return;

    IPCMessage msg;
    msg.type = IPCMessageType::SetAIPreload;
    msg.data.push_back(enabled ? 1 : 0);

    SendMessage(msg);
}

void ServiceClient::OptimizeRoutes() {
    if (!connected) return;

    IPCMessage msg;
    msg.type = IPCMessageType::OptimizeRoutes;

    SendMessage(msg);
}
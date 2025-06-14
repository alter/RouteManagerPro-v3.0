// src/common/Result.h
#pragma once
#include <string>
#include <variant>
#include <optional>
#include <windows.h>

// Generic error type for route operations
struct RouteError {
    enum class Type {
        None,
        InvalidIP,
        PrivateIP,
        GatewayUnreachable,
        InterfaceNotFound,
        RouteExists,
        RouteNotFound,
        SystemError,
        LimitExceeded,
        AccessDenied
    };

    Type type = Type::None;
    std::string details;
    DWORD winError = 0;

    RouteError() = default;
    RouteError(Type t, const std::string& msg = "", DWORD err = 0)
        : type(t), details(msg), winError(err) {
    }

    bool IsError() const { return type != Type::None; }

    std::string ToString() const {
        std::string result = GetTypeString();
        if (!details.empty()) {
            result += ": " + details;
        }
        if (winError != 0) {
            result += " (Windows error: " + std::to_string(winError) + ")";
        }
        return result;
    }

private:
    std::string GetTypeString() const {
        switch (type) {
        case Type::None: return "Success";
        case Type::InvalidIP: return "Invalid IP address";
        case Type::PrivateIP: return "Private IP address";
        case Type::GatewayUnreachable: return "Gateway unreachable";
        case Type::InterfaceNotFound: return "Network interface not found";
        case Type::RouteExists: return "Route already exists";
        case Type::RouteNotFound: return "Route not found";
        case Type::SystemError: return "System error";
        case Type::LimitExceeded: return "Limit exceeded";
        case Type::AccessDenied: return "Access denied";
        default: return "Unknown error";
        }
    }
};

// Result type that can hold either success value or error
template<typename T>
class Result {
public:
    Result(T value) : data(std::move(value)) {}
    Result(RouteError error) : data(std::move(error)) {}

    bool IsSuccess() const {
        return std::holds_alternative<T>(data);
    }

    bool IsError() const {
        return std::holds_alternative<RouteError>(data);
    }

    T& Value() {
        return std::get<T>(data);
    }

    const T& Value() const {
        return std::get<T>(data);
    }

    RouteError& Error() {
        return std::get<RouteError>(data);
    }

    const RouteError& Error() const {
        return std::get<RouteError>(data);
    }

    // Convenient operators
    explicit operator bool() const {
        return IsSuccess();
    }

    T* operator->() {
        return &Value();
    }

    const T* operator->() const {
        return &Value();
    }

private:
    std::variant<T, RouteError> data;
};

// Specialization for void-like results
template<>
class Result<void> {
public:
    Result() : error(RouteError()) {}
    Result(RouteError err) : error(std::move(err)) {}

    bool IsSuccess() const {
        return !error.IsError();
    }

    bool IsError() const {
        return error.IsError();
    }

    const RouteError& Error() const {
        return error;
    }

    explicit operator bool() const {
        return IsSuccess();
    }

    static Result<void> Success() {
        return Result<void>();
    }

    static Result<void> Failure(RouteError err) {
        return Result<void>(std::move(err));
    }

private:
    RouteError error;
};

// Helper functions
template<typename T>
Result<T> Ok(T value) {
    return Result<T>(std::move(value));
}

inline Result<void> Ok() {
    return Result<void>::Success();
}

template<typename T>
Result<T> Err(RouteError error) {
    return Result<T>(std::move(error));
}

inline Result<void> Err(RouteError error) {
    return Result<void>::Failure(std::move(error));
}
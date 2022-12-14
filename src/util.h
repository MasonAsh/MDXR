#pragma once

#define NOMINMAX


#ifdef NDEBUG
#undef NDEBUG
#include <assert.h>
#define NDEBUG
#endif

#include <comdef.h> 
#include <iostream>
#include <format>
#include <chrono>

#include <locale>
#include <codecvt>
#include <string>
#include <limits>
#include <sstream>
#include <fstream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#define ASSERT_HRESULT(hr) \
    { \
    HRESULT val = (hr); \
    if (!SUCCEEDED(val)) { \
        _com_error err(val); \
        LPCTSTR errMsg = err.ErrorMessage(); \
        DebugLog() << "Failed HRESULT(" << val << "): " << errMsg << "\n"; \
        DebugLog() << #hr << "\n";\
        abort(); \
    }\
    }

inline std::wstring convert_to_wstring(std::string input)
{
    int numWChars = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, NULL, 0);
    wchar_t* wstr = new wchar_t[numWChars];
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, wstr, numWChars);
    std::wstring wide(wstr, numWChars - 1);
    delete[] wstr;
    return wide;
}

template <typename... T>
constexpr auto make_array(T&&... values) ->
std::array<
    typename std::decay<
    typename std::common_type<T...>::type>::type,
    sizeof...(T)> {
    return std::array<
        typename std::decay<
        typename std::common_type<T...>::type>::type,
        sizeof...(T)>{std::forward<T>(values)...};
}

// Allow printing glm data types with cout
inline std::ostream& operator<< (std::ostream& out, const glm::vec3& vec) {
    out << "{"
        << vec.x << " " << vec.y << " " << vec.z
        << "}";

    return out;
}

inline std::ostream& operator<< (std::ostream& out, const glm::vec4& vec) {
    out << "{"
        << vec.x << " " << vec.y << " " << vec.z << " " << vec.w
        << "}";

    return out;
}

inline std::ostream& operator<< (std::ostream& out, const glm::mat4& matrix) {
    out << "\n"
        << glm::row(matrix, 0) << "\n"
        << glm::row(matrix, 1) << "\n"
        << glm::row(matrix, 2) << "\n"
        << glm::row(matrix, 3) << "\n";

    return out;
}

#define DEBUG_VAR(vec) DebugLog() << #vec << ": " << vec << "\n";

// Like assert, but the code still gets executed in release mode
#ifdef MDXR_DEBUG

#ifdef NDEBUG

// RelWithDebInfo. No access to assert, so use this instead
#define CHECK(code) { if (!(code)) { DbgRaiseAssertionFailure(); } }

#else
#define CHECK(code) assert(code)
#endif

#else
#define CHECK(code) code
#endif

#define MDXR_ASSERT(code) if (!(code)) { abort(); }

enum class PerformancePrecision
{
    Seconds,
    Milliseconds,
    Nanoseconds,
};

class DebugLog {
public:
    ~DebugLog()
    {
        OutputDebugStringA(ss.str().c_str());
        std::ofstream outlog;
        outlog.open("debug.txt", std::ios_base::app);
        outlog << ss.str().c_str() << "\n";
    }

    template<class T>
    DebugLog& operator <<(const T& arg)
    {
        ss << arg;
        return *this;
    }
private:
    std::stringstream ss;
};

class ScopedPerformanceTracker
{
public:
    ScopedPerformanceTracker(const char* name, PerformancePrecision precision, const char* numberFormat = "{:.4f}")
        : name(name)
        , precision(precision)
        , numberFormat(numberFormat)
    {
        DebugLog() << "Beginning " << name << "\n";
        startNS = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    ~ScopedPerformanceTracker()
    {
        unsigned long long now = std::chrono::steady_clock::now().time_since_epoch().count();
        unsigned long long delta = now - startNS;

        DebugLog() << name << " finished: ";

        switch (precision) {
        case PerformancePrecision::Seconds:
        {
            float seconds = (float)delta / (float)1e+9;
            DebugLog() << std::format(numberFormat, seconds) << " seconds elapsed\n";
        }
        break;
        case PerformancePrecision::Milliseconds:
        {
            float millis = (float)delta / 1000000.0f;
            DebugLog() << std::format(numberFormat, millis) << " milliseconds elapsed\n";
        }
        break;
        case PerformancePrecision::Nanoseconds:
            DebugLog() << delta << " nanoseconds elapsed\n";
            break;
        }
    }
private:
    unsigned long long startNS;
    const char* name;
    const char* numberFormat;
    PerformancePrecision precision;
};

// Like static_cast, but asserts if there's a loss of data
template<typename Out, typename In>
Out assert_cast(In input)
{
    bool condition = input <= std::numeric_limits<Out>::max();
    CHECK(condition);
    return static_cast<Out>(input);
}

inline glm::mat4 ApplyStandardTransforms(const glm::mat4& base, glm::vec3 translation, glm::vec3 euler, glm::vec3 scale)
{
    glm::mat4 transform = base;
    transform = glm::translate(transform, translation);
    transform = glm::scale(transform, scale);
    transform = transform * glm::eulerAngleXYZ(euler.x, euler.y, euler.z);

    return transform;
}

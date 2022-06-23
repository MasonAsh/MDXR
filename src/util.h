#if !defined(UTIL_H)
#define UTIL_H

#include <assert.h>
#include <comdef.h> 
#include <iostream>

#include <locale>
#include <codecvt>
#include <string>

#include "glm/glm.hpp"

#define ASSERT_HRESULT(hr) \
    { \
    HRESULT val = (hr); \
    if (!SUCCEEDED(val)) { \
        _com_error err(val); \
        LPCTSTR errMsg = err.ErrorMessage(); \
        std::cout << "Failed HRESULT(" << val << "): " << errMsg << "\n"; \
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

// Allow printing glm vectors with cout
std::ostream& operator<< (std::ostream& out, const glm::vec3& vec) {
    out << "{"
        << vec.x << " " << vec.y << " " << vec.z
        << "}";

    return out;
}

#define DEBUG_VAR(vec) std::cout << #vec << ": " << vec << "\n";

// Like assert, but the code still gets executed in release mode
#ifdef _DEBUG
#define CHECK(code) assert(code)
#else
#define CHECK(code) code
#endif

#define MDXR_ASSERT(code) if (!(code)) { abort(); }

#endif // UTIL_H

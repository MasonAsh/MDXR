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
    if (!SUCCEEDED((hr))) { \
        _com_error err(hr); \
        LPCTSTR errMsg = err.ErrorMessage(); \
        std::cout << "Failed HRESULT(" << hr << "): " << errMsg << "\n"; \
        abort(); \
    }

inline std::wstring convert_to_wstring(std::string input)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring wide = converter.from_bytes(input);
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

#endif // UTIL_H

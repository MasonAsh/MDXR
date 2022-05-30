#if !defined(UTIL_H)
#define UTIL_H

#include <assert.h>
#include <comdef.h> 
#include <iostream>

#include <locale>
#include <codecvt>
#include <string>

#define ASSERT_HRESULT(hr) \
    if (!SUCCEEDED((hr))) { \
        _com_error err(hr); \
        LPCTSTR errMsg = err.ErrorMessage(); \
        std::cout << "Failed HRESULT(" << hr << "): " << errMsg << "\n"; \
        abort(); \
    }

std::wstring convert_to_wstring(std::string input)
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

#endif // UTIL_H

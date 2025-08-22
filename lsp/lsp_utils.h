#pragma once
#include <string>
#include <cctype>

inline std::string pathToFileUri(std::string p) {
    // 1) Normalize slashes
    for (auto &c : p) if (c == '\\') c = '/';

    // 2) Windows drive letter? Uppercase it and prefix file:///
    if (p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') {
        p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        p = "file:///" + p;   // yields file:///D:/path...
    } else if (p.rfind("file://", 0) != 0) {
        p = "file://" + p;
    }

    // 3) Encode ONLY spaces (avoid touching ':' or '/')
    std::string out;
    out.reserve(p.size() * 2);
    for (unsigned char ch : p) {
        if (ch == ' ') out += "%20";
        else           out += static_cast<char>(ch);
    }
    return out;
}

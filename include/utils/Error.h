#pragma once

#include <stdexcept>
#include <string>

namespace utils {

class FatalError : public std::runtime_error {
public:
   explicit FatalError(const std::string& what = "")
         : std::runtime_error(get_trace(what)) {}

private:
   std::string get_trace(const std::string& what);
};

class AssertError : public std::runtime_error {
public:
   explicit AssertError(const std::string& what = "")
         : std::runtime_error(get_trace(what)) {}

private:
   std::string get_trace(const std::string& what);
};

} // namespace utils
#pragma once
#include <string>
namespace mvd {
struct DbError {
  std::string code;
  std::string message;
};

[[noreturn]] inline void throw_error(const std::string &code,
                                     const std::string &message) {
  throw DbError{code, message};
}

}

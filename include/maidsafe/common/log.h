/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#ifndef MAIDSAFE_COMMON_LOG_H_
#define MAIDSAFE_COMMON_LOG_H_

#include "boost/current_function.hpp"

#include <string>
#include <sstream>
#include <iostream>
#include <cstdarg>
#include <memory>

#include "maidsafe/common/active.h"

// compile away DLOG and LOG statements
class NullStream {
    public:
    NullStream() { }
    template<typename T> NullStream& operator<<(T const&) { return *this; }
    explicit operator bool() const{ return false; }
};

const int INFO = 0, WARNING = 1, ERROR = 2, FATAL = 3;

#define LOG_INFO  maidsafe::log::LogMessage(__FILE__,__LINE__,BOOST_CURRENT_FUNCTION,0)
#define LOG_WARNING  maidsafe::log::LogMessage(__FILE__,__LINE__,BOOST_CURRENT_FUNCTION,1)
#define LOG_ERROR  maidsafe::log::LogMessage(__FILE__,__LINE__,BOOST_CURRENT_FUNCTION,2)
#define LOG_FATAL  maidsafe::log::LogMessage(__FILE__,__LINE__,BOOST_CURRENT_FUNCTION,3)

#ifdef NDEBUG
#define DLOG(_) false && NullStream()
#else
#define DLOG(level) LOG_##level.messageStream()
#endif

namespace maidsafe {
namespace log {

typedef const std::string& LogEntry;

class LogMessage {
 public:
  LogMessage(const std::string &file,
             const int line,
             const std::string& function,
             const int level);
  ~LogMessage();
  std::ostringstream& messageStream(){return stream_;}
 private:
  const std::string file_;
  const int line_;
  const std::string function_;
  const int level_;
  std::ostringstream stream_;
  std::string log_entry_;
};

class Logging {
 public:
  static Logging& instance() {
    static Logging l;
    return l;
  }
  typedef std::function<void()> functor ;
  void Send(functor function);
  void SetLogLevel(int log_level) { log_levels_ = log_level; }
  int LogLevel() {return log_levels_; }
  void SetFilter(std::string filter) { filter_ = filter; }
  std::string Filter() { return filter_; }
 private:
  Logging();
  std::unique_ptr<maidsafe::Active> background_;
  int log_levels_;
  std::string filter_;
};

}  // log
}  // maidsafe

#endif  // MAIDSAFE_COMMON_LOG_H_
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

struct Field {
  std::string name;
  std::vector<uint8_t> value;
  Field() {}
  Field(const std::string& n, const std::vector<uint8_t>& v)
      : name(n), value(v) {}
  Field(const std::string& n, const std::string& v)
      : name(n), value(v.begin(), v.end()) {}
};

struct Record {
  std::string userkey;
  std::vector<Field> fields;
};

class StreamCoreFile {
 public:
  virtual ~StreamCoreFile() {}

  virtual int save(const Record& r, bool overwrite = true) = 0;
  virtual int load(const std::string& userkey, Record* out) = 0;
  virtual int list(std::vector<std::string>& out_userkeys) = 0;
  virtual int exists(const std::string& userkey) = 0;
  virtual int erase(const std::string& userkey) = 0;
  virtual int set_current(const std::string& userkey) = 0;
  virtual int get_current(Record* out) = 0;
  // Add other virtual functions as needed...
};
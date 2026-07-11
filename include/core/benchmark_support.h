#pragma once

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace agent_aware::bench {

class ArgParser {
 public:
  ArgParser(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string token = argv[i];
      const auto equals = token.find('=');
      if (equals != std::string::npos) {
        entries_.push_back({token.substr(0, equals), token.substr(equals + 1)});
      } else if (token == "--help" || token == "-h") {
        entries_.push_back({std::move(token), {}});
      } else {
        if (++i >= argc) {
          throw std::runtime_error("Missing value for " + token);
        }
        entries_.push_back({std::move(token), argv[i]});
      }
    }
  }

  bool flag(std::initializer_list<const char*> names) {
    bool present = false;
    while (find(names) != nullptr) present = true;
    return present;
  }

  template <typename T>
  bool take(T& target, std::initializer_list<const char*> names) {
    bool present = false;
    while (Entry* entry = find(names)) {
      parse(entry->value, entry->name, target);
      present = true;
    }
    return present;
  }

  void finish() const {
    for (const auto& entry : entries_) {
      if (!entry.used) {
        throw std::runtime_error("Unknown option: " + entry.name);
      }
    }
  }

 private:
  struct Entry {
    std::string name;
    std::string value;
    bool used = false;
  };

  Entry* find(std::initializer_list<const char*> names) {
    for (auto& entry : entries_) {
      for (const char* name : names) {
        if (!entry.used && entry.name == name) {
          entry.used = true;
          return &entry;
        }
      }
    }
    return nullptr;
  }

  static void parse(const std::string& value, const std::string&,
                    std::string& target) {
    target = value;
  }

  static void parse(const std::string& value, const std::string&,
                    std::filesystem::path& target) {
    target = value;
  }

  static void parse(const std::string& value, const std::string& option,
                    bool& target) {
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
      target = true;
    } else if (value == "0" || value == "false" || value == "off" ||
               value == "no") {
      target = false;
    } else {
      throw std::runtime_error("Invalid boolean for " + option + ": " + value);
    }
  }

  template <typename T>
  static std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>
  parse(const std::string& value, const std::string& option, T& target) {
    unsigned long long parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
      throw std::runtime_error("Invalid integer for " + option + ": " + value);
    }
    target = static_cast<T>(parsed);
  }

  static void parse(const std::string& value, const std::string& option,
                    double& target) {
    char* end = nullptr;
    target = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
      throw std::runtime_error("Invalid number for " + option + ": " + value);
    }
  }

  std::vector<Entry> entries_;
};

class JsonWriter {
 public:
  JsonWriter() { output_ << std::boolalpha << "{"; }

  template <typename T>
  void field(const std::string& name, const T& value) {
    prefix(name);
    output_ << value;
  }

  void field(const std::string& name, const std::string& value) {
    prefix(name);
    output_ << '"' << escape(value) << '"';
  }

  void field(const std::string& name, const char* value) {
    field(name, std::string(value));
  }

  template <typename T>
  void optional_field(const std::string& name, const std::optional<T>& value) {
    if (value.has_value()) field(name, *value); else null_field(name);
  }

  void null_field(const std::string& name) {
    prefix(name);
    output_ << "null";
  }

  template <typename T>
  void array_field(const std::string& name, const std::vector<T>& values) {
    prefix(name);
    output_ << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i) output_ << ", ";
      if constexpr (std::is_same_v<T, std::string>) {
        output_ << '"' << escape(values[i]) << '"';
      } else {
        output_ << values[i];
      }
    }
    output_ << ']';
  }

  void begin_object(const std::string& name) {
    prefix(name);
    output_ << '{';
    first_.push_back(true);
  }

  void end_object() {
    output_ << '\n' << std::string((first_.size() - 1) * 2, ' ') << '}';
    first_.pop_back();
  }

  std::string finish() {
    output_ << "\n}\n";
    return output_.str();
  }

  static std::string escape(const std::string& value) {
    std::ostringstream escaped;
    for (const char ch : value) {
      switch (ch) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default: escaped << ch; break;
      }
    }
    return escaped.str();
  }

 private:
  void prefix(const std::string& name) {
    if (first_.empty()) first_.push_back(true);
    output_ << (first_.back() ? "\n" : ",\n")
            << std::string(first_.size() * 2, ' ') << '"' << escape(name)
            << "\": ";
    first_.back() = false;
  }

  std::ostringstream output_;
  std::vector<bool> first_;
};

}  // namespace agent_aware::bench

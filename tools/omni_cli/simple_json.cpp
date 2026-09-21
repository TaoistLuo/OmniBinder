/**************************************************************************************************
 * @file        simple_json.cpp
 * @brief       轻量级 JSON 解析器实现
 *************************************************************************************************/
#include "simple_json.h"
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cerrno>

namespace simple_json {

namespace {

const size_t kMaxJsonDepth = 64;

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

void appendEscapedJsonString(std::ostream& os, const std::string& value) {
    static const char hex_digits[] = "0123456789ABCDEF";
    os << '"';
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            if (c < 0x20) {
                os << "\\u00" << hex_digits[(c >> 4) & 0x0F] << hex_digits[c & 0x0F];
            } else {
                os << static_cast<char>(c);
            }
            break;
        }
    }
    os << '"';
}

} // namespace

class Parser {
public:
    explicit Parser(const std::string& json) : json_(json), pos_(0), depth_(0) {}
    
    Value parse() {
        skipWhitespace();
        Value value = parseValue();
        skipWhitespace();
        if (pos_ != json_.size()) {
            throw std::runtime_error("Trailing characters after JSON value");
        }
        return value;
    }
    
private:
    std::string json_;
    size_t pos_;
    size_t depth_;

    struct DepthGuard {
        explicit DepthGuard(size_t& depth) : depth_(depth) { ++depth_; }
        ~DepthGuard() { --depth_; }
        size_t& depth_;
    };
    
    void skipWhitespace() {
        while (pos_ < json_.size() && isspace(static_cast<unsigned char>(json_[pos_]))) {
            ++pos_;
        }
    }
    
    char peek() const {
        return pos_ < json_.size() ? json_[pos_] : '\0';
    }
    
    char next() {
        return pos_ < json_.size() ? json_[pos_++] : '\0';
    }
    
    void expect(char c) {
        if (next() != c) {
            throw std::runtime_error(std::string("Expected '") + c + "'");
        }
    }

    bool readHex4(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            int v = hexValue(next());
            if (v < 0) return false;
            out = (out << 4) | static_cast<uint32_t>(v);
        }
        return true;
    }
    
    Value parseValue() {
        if (depth_ >= kMaxJsonDepth) {
            throw std::runtime_error("JSON nesting too deep");
        }
        DepthGuard guard(depth_);
        skipWhitespace();
        char c = peek();
        
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || isdigit(static_cast<unsigned char>(c))) return parseNumber();
        
        throw std::runtime_error("Invalid JSON value");
    }
    
    Value parseObject() {
        Value obj;
        obj.setObject();
        expect('{');
        skipWhitespace();
        
        if (peek() == '}') {
            next();
            return obj;
        }
        
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                throw std::runtime_error("Expected string key in object");
            }
            
            std::string key = parseString().asString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            Value value = parseValue();
            obj.set(key, value);
            
            skipWhitespace();
            char c = next();
            if (c == '}') break;
            if (c != ',') {
                throw std::runtime_error("Expected ',' or '}' in object");
            }
        }
        
        return obj;
    }
    
    Value parseArray() {
        Value arr;
        arr.setArray();
        expect('[');
        skipWhitespace();
        
        if (peek() == ']') {
            next();
            return arr;
        }
        
        while (true) {
            arr.push(parseValue());
            skipWhitespace();
            char c = next();
            if (c == ']') break;
            if (c != ',') {
                throw std::runtime_error("Expected ',' or ']' in array");
            }
            skipWhitespace();
        }
        
        return arr;
    }
    
    Value parseString() {
        expect('"');
        std::string str;
        
        while (true) {
            char c = next();
            if (c == '\0') {
                throw std::runtime_error("Unterminated string");
            }
            if (c == '"') break;
            if (c == '\\') {
                c = next();
                switch (c) {
                case '"': str += '"'; break;
                case '\\': str += '\\'; break;
                case '/': str += '/'; break;
                case 'b': str += '\b'; break;
                case 'f': str += '\f'; break;
                case 'n': str += '\n'; break;
                case 'r': str += '\r'; break;
                case 't': str += '\t'; break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!readHex4(codepoint)) {
                        throw std::runtime_error("Invalid \\u escape sequence");
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (next() != '\\' || next() != 'u') {
                            throw std::runtime_error("Unpaired UTF-16 high surrogate");
                        }
                        uint32_t low = 0;
                        if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            throw std::runtime_error("Invalid UTF-16 low surrogate");
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        throw std::runtime_error("Unpaired UTF-16 low surrogate");
                    }
                    appendUtf8(str, codepoint);
                    break;
                }
                default:
                    throw std::runtime_error("Invalid escape sequence");
                }
            } else {
                str += c;
            }
        }
        
        return Value(str);
    }
    
    Value parseNumber() {
        size_t start = pos_;
        if (peek() == '-') next();
        
        if (!isdigit(static_cast<unsigned char>(peek()))) {
            throw std::runtime_error("Invalid number");
        }
        
        while (isdigit(static_cast<unsigned char>(peek()))) next();
        
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            next();
            if (!isdigit(static_cast<unsigned char>(peek()))) {
                throw std::runtime_error("Invalid number");
            }
            while (isdigit(static_cast<unsigned char>(peek()))) next();
        }
        
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            next();
            if (peek() == '+' || peek() == '-') next();
            if (!isdigit(static_cast<unsigned char>(peek()))) {
                throw std::runtime_error("Invalid number");
            }
            while (isdigit(static_cast<unsigned char>(peek()))) next();
        }
        
        std::string numStr = json_.substr(start, pos_ - start);
        if (!is_float) {
            char* end = NULL;
            errno = 0;
            if (numStr[0] == '-') {
                long long signed_value = strtoll(numStr.c_str(), &end, 10);
                if (errno == 0 && end == numStr.c_str() + numStr.size()) {
                    return Value(static_cast<int64_t>(signed_value));
                }
            } else {
                unsigned long long unsigned_value = strtoull(numStr.c_str(), &end, 10);
                if (errno == 0 && end == numStr.c_str() + numStr.size()) {
                    if (unsigned_value <= static_cast<unsigned long long>(INT64_MAX)) {
                        return Value(static_cast<int64_t>(unsigned_value));
                    }
                    return Value(static_cast<uint64_t>(unsigned_value));
                }
            }
        }
        double num = atof(numStr.c_str());
        return Value(num);
    }
    
    Value parseBool() {
        if (json_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return Value(true);
        }
        if (json_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return Value(false);
        }
        throw std::runtime_error("Invalid boolean");
    }
    
    Value parseNull() {
        if (json_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return Value();
        }
        throw std::runtime_error("Invalid null");
    }
};

Value parse(const std::string& json) {
    Parser parser(json);
    return parser.parse();
}

std::string Value::toString(bool pretty, int indent) const {
    std::ostringstream os;
    std::string indentStr(indent * 2, ' ');
    std::string nextIndentStr((indent + 1) * 2, ' ');
    
    switch (type_) {
    case TYPE_NULL:
        os << "null";
        break;
    case TYPE_BOOL:
        os << (bool_val_ ? "true" : "false");
        break;
    case TYPE_NUMBER:
        // 判断是否为整数
        if (num_kind_ == NUM_INT) {
            os << int_val_;
        } else if (num_kind_ == NUM_UINT) {
            os << uint_val_;
        } else if (std::isfinite(num_val_)
                   && num_val_ >= -9223372036854775808.0 && num_val_ <= 9223372036854775807.0
                   && num_val_ == std::floor(num_val_)) {
            os << static_cast<int64_t>(num_val_);
        } else {
            os << std::setprecision(17) << num_val_;
        }
        break;
    case TYPE_STRING:
        appendEscapedJsonString(os, str_val_);
        break;
    case TYPE_ARRAY:
        os << '[';
        if (pretty && !array_val_.empty()) os << '\n';
        for (size_t i = 0; i < array_val_.size(); ++i) {
            if (pretty) os << nextIndentStr;
            os << array_val_[i].toString(pretty, indent + 1);
            if (i + 1 < array_val_.size()) os << ',';
            if (pretty) os << '\n';
        }
        if (pretty && !array_val_.empty()) os << indentStr;
        os << ']';
        break;
    case TYPE_OBJECT:
        os << '{';
        if (pretty && !obj_val_.empty()) os << '\n';
        {
            std::map<std::string, Value>::const_iterator it = obj_val_.begin();
            while (it != obj_val_.end()) {
                if (pretty) os << nextIndentStr;
                appendEscapedJsonString(os, it->first);
                os << ':';
                if (pretty) os << ' ';
                os << it->second.toString(pretty, indent + 1);
                ++it;
                if (it != obj_val_.end()) os << ',';
                if (pretty) os << '\n';
            }
        }
        if (pretty && !obj_val_.empty()) os << indentStr;
        os << '}';
        break;
    }
    
    return os.str();
}

} // namespace simple_json

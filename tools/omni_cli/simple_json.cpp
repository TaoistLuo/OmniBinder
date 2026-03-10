/**************************************************************************************************
 * @file        simple_json.cpp
 * @brief       轻量级 JSON 解析器实现
 *************************************************************************************************/
#include "simple_json.h"
#include <sstream>
#include <cctype>
#include <cstdlib>

namespace simple_json {

class Parser {
public:
    explicit Parser(const std::string& json) : json_(json), pos_(0) {}
    
    Value parse() {
        skipWhitespace();
        return parseValue();
    }
    
private:
    std::string json_;
    size_t pos_;
    
    void skipWhitespace() {
        while (pos_ < json_.size() && isspace(json_[pos_])) {
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
    
    Value parseValue() {
        skipWhitespace();
        char c = peek();
        
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        if (c == '-' || isdigit(c)) return parseNumber();
        
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
        
        if (!isdigit(peek())) {
            throw std::runtime_error("Invalid number");
        }
        
        while (isdigit(peek())) next();
        
        if (peek() == '.') {
            next();
            if (!isdigit(peek())) {
                throw std::runtime_error("Invalid number");
            }
            while (isdigit(peek())) next();
        }
        
        if (peek() == 'e' || peek() == 'E') {
            next();
            if (peek() == '+' || peek() == '-') next();
            if (!isdigit(peek())) {
                throw std::runtime_error("Invalid number");
            }
            while (isdigit(peek())) next();
        }
        
        std::string numStr = json_.substr(start, pos_ - start);
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
        // Check if it's an integer
        if (num_val_ == static_cast<int64_t>(num_val_)) {
            os << static_cast<int64_t>(num_val_);
        } else {
            os << num_val_;
        }
        break;
    case TYPE_STRING:
        os << '"';
        for (size_t i = 0; i < str_val_.size(); ++i) {
            char c = str_val_[i];
            switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default: os << c; break;
            }
        }
        os << '"';
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
                os << '"' << it->first << '"' << ':';
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

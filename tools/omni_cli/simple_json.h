/**************************************************************************************************
 * @file        simple_json.h
 * @brief       轻量级 JSON 解析器
 * @details     用于 omni-cli 解析 JSON 输入和生成 JSON 输出。
 *              支持基本的 JSON 类型：object, array, string, number, bool, null
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-03-05
 *************************************************************************************************/
#ifndef SIMPLE_JSON_H
#define SIMPLE_JSON_H

#include <string>
#include <map>
#include <vector>
#include <stdexcept>

namespace simple_json {

enum ValueType {
    TYPE_NULL,
    TYPE_BOOL,
    TYPE_NUMBER,
    TYPE_STRING,
    TYPE_ARRAY,
    TYPE_OBJECT
};

class Value {
public:
    Value() : type_(TYPE_NULL), bool_val_(false), num_val_(0.0) {}
    explicit Value(bool b) : type_(TYPE_BOOL), bool_val_(b), num_val_(0.0) {}
    explicit Value(double n) : type_(TYPE_NUMBER), bool_val_(false), num_val_(n) {}
    explicit Value(const std::string& s) : type_(TYPE_STRING), bool_val_(false), num_val_(0.0), str_val_(s) {}
    
    ValueType type() const { return type_; }
    
    bool isNull() const { return type_ == TYPE_NULL; }
    bool isBool() const { return type_ == TYPE_BOOL; }
    bool isNumber() const { return type_ == TYPE_NUMBER; }
    bool isString() const { return type_ == TYPE_STRING; }
    bool isArray() const { return type_ == TYPE_ARRAY; }
    bool isObject() const { return type_ == TYPE_OBJECT; }
    
    bool asBool() const { return bool_val_; }
    double asNumber() const { return num_val_; }
    int64_t asInt64() const { return static_cast<int64_t>(num_val_); }
    const std::string& asString() const { return str_val_; }
    
    // Array operations
    void setArray() { type_ = TYPE_ARRAY; }
    void push(const Value& v) { array_val_.push_back(v); }
    size_t size() const { return array_val_.size(); }
    const Value& operator[](size_t idx) const { return array_val_[idx]; }
    Value& operator[](size_t idx) { return array_val_[idx]; }
    
    // Object operations
    void setObject() { type_ = TYPE_OBJECT; }
    void set(const std::string& key, const Value& v) { obj_val_[key] = v; }
    bool has(const std::string& key) const { return obj_val_.find(key) != obj_val_.end(); }
    const Value& get(const std::string& key) const {
        std::map<std::string, Value>::const_iterator it = obj_val_.find(key);
        if (it == obj_val_.end()) throw std::runtime_error("Key not found: " + key);
        return it->second;
    }
    Value& get(const std::string& key) {
        return obj_val_[key];
    }
    const std::map<std::string, Value>& getObject() const { return obj_val_; }
    
    // Serialization
    std::string toString(bool pretty = false, int indent = 0) const;
    
private:
    ValueType type_;
    bool bool_val_;
    double num_val_;
    std::string str_val_;
    std::vector<Value> array_val_;
    std::map<std::string, Value> obj_val_;
};

// Parse JSON string to Value
Value parse(const std::string& json);

} // namespace simple_json

#endif // SIMPLE_JSON_H

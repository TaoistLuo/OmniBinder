#include "lexer.h"
#include <cctype>
#include <cstdio>

namespace omnic {

Lexer::Lexer(const std::string& source)
    : source_(source), pos_(0), line_(1), column_(1), has_error_(false) {}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[pos_];
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') { line_++; column_ = 1; } else { column_++; }
    return c;
}

bool Lexer::isAtEnd() const { return pos_ >= source_.size(); }

void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (isspace(c)) { advance(); continue; }
        if (c == '/' && pos_ + 1 < source_.size()) {
            if (source_[pos_ + 1] == '/') {
                while (!isAtEnd() && peek() != '\n') advance();
                continue;
            }
            if (source_[pos_ + 1] == '*') {
                advance(); advance();
                while (!isAtEnd()) {
                    if (peek() == '*' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
                        advance(); advance(); break;
                    }
                    advance();
                }
                continue;
            }
        }
        break;
    }
}

Token Lexer::readIdentifierOrKeyword() {
    int l = line_, c = column_;
    std::string val;
    while (!isAtEnd() && (isalnum(peek()) || peek() == '_')) {
        val += advance();
    }
    
    if (val == "package")   return Token(TOK_PACKAGE, val, l, c);
    if (val == "import")    return Token(TOK_IMPORT, val, l, c);
    if (val == "struct")    return Token(TOK_STRUCT, val, l, c);
    if (val == "service")   return Token(TOK_SERVICE, val, l, c);
    if (val == "topic")     return Token(TOK_TOPIC, val, l, c);
    if (val == "publishes") return Token(TOK_PUBLISHES, val, l, c);
    if (val == "array")     return Token(TOK_ARRAY, val, l, c);
    if (val == "bool")      return Token(TOK_BOOL, val, l, c);
    if (val == "int8")      return Token(TOK_INT8, val, l, c);
    if (val == "uint8")     return Token(TOK_UINT8, val, l, c);
    if (val == "int16")     return Token(TOK_INT16, val, l, c);
    if (val == "uint16")    return Token(TOK_UINT16, val, l, c);
    if (val == "int32")     return Token(TOK_INT32, val, l, c);
    if (val == "uint32")    return Token(TOK_UINT32, val, l, c);
    if (val == "int64")     return Token(TOK_INT64, val, l, c);
    if (val == "uint64")    return Token(TOK_UINT64, val, l, c);
    if (val == "float32")   return Token(TOK_FLOAT32, val, l, c);
    if (val == "float64")   return Token(TOK_FLOAT64, val, l, c);
    if (val == "string")    return Token(TOK_STRING_TYPE, val, l, c);
    if (val == "bytes")     return Token(TOK_BYTES, val, l, c);
    if (val == "void")      return Token(TOK_VOID, val, l, c);
    
    return Token(TOK_IDENTIFIER, val, l, c);
}

Token Lexer::readNumber() {
    int l = line_, c = column_;
    std::string val;
    while (!isAtEnd() && isdigit(peek())) val += advance();
    return Token(TOK_NUMBER, val, l, c);
}

Token Lexer::readString() {
    int l = line_, c = column_;
    advance(); // skip opening quote
    std::string val;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') { advance(); if (!isAtEnd()) val += advance(); }
        else val += advance();
    }
    if (!isAtEnd()) advance(); // skip closing quote
    return Token(TOK_STRING, val, l, c);
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();
    if (isAtEnd()) return Token(TOK_EOF, "", line_, column_);
    
    int l = line_, c = column_;
    char ch = peek();
    
    if (isalpha(ch) || ch == '_') return readIdentifierOrKeyword();
    if (isdigit(ch)) return readNumber();
    if (ch == '"') return readString();
    
    advance();
    switch (ch) {
    case '{': return Token(TOK_LBRACE, "{", l, c);
    case '}': return Token(TOK_RBRACE, "}", l, c);
    case '(': return Token(TOK_LPAREN, "(", l, c);
    case ')': return Token(TOK_RPAREN, ")", l, c);
    case '<': return Token(TOK_LANGLE, "<", l, c);
    case '>': return Token(TOK_RANGLE, ">", l, c);
    case ';': return Token(TOK_SEMICOLON, ";", l, c);
    case ',': return Token(TOK_COMMA, ",", l, c);
    case '.': return Token(TOK_DOT, ".", l, c);
    default:
        has_error_ = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "Unexpected character '%c' at line %d:%d", ch, l, c);
        error_msg_ = buf;
        return Token(TOK_ERROR, std::string(1, ch), l, c);
    }
}

const char* tokenTypeToString(TokenType type) {
    switch (type) {
    case TOK_EOF: return "EOF"; case TOK_IDENTIFIER: return "IDENTIFIER";
    case TOK_NUMBER: return "NUMBER"; case TOK_STRING: return "STRING";
    case TOK_PACKAGE: return "package"; case TOK_IMPORT: return "import";
    case TOK_STRUCT: return "struct";
    case TOK_SERVICE: return "service"; case TOK_TOPIC: return "topic";
    case TOK_PUBLISHES: return "publishes"; case TOK_ARRAY: return "array";
    case TOK_SEMICOLON: return ";"; case TOK_LBRACE: return "{";
    case TOK_RBRACE: return "}"; case TOK_LPAREN: return "(";
    case TOK_RPAREN: return ")"; case TOK_LANGLE: return "<";
    case TOK_RANGLE: return ">"; case TOK_COMMA: return ",";
    case TOK_DOT: return ".";
    default: return "?";
    }
}

} // namespace omnic

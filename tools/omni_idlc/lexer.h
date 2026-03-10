/**************************************************************************************************
 * @file        lexer.h
 * @brief       IDL 词法分析器
 * @details     将 .bidl 源文件文本分解为 Token 流。支持的 Token 类型包括关键字
 *              （package/struct/service/topic/publishes 及所有基础类型名）、标识符、
 *              数字字面量、字符串字面量和各种标点符号。自动跳过空白和注释。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2025-05-20
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef BINDERC_LEXER_H
#define BINDERC_LEXER_H

#include <string>
#include <vector>

namespace omnic {

enum TokenType {
    TOK_EOF, TOK_IDENTIFIER, TOK_NUMBER, TOK_STRING,
    TOK_PACKAGE, TOK_IMPORT, TOK_STRUCT, TOK_SERVICE, TOK_TOPIC, TOK_PUBLISHES, TOK_ARRAY,
    TOK_BOOL, TOK_INT8, TOK_UINT8, TOK_INT16, TOK_UINT16,
    TOK_INT32, TOK_UINT32, TOK_INT64, TOK_UINT64,
    TOK_FLOAT32, TOK_FLOAT64, TOK_STRING_TYPE, TOK_BYTES, TOK_VOID,
    TOK_LBRACE, TOK_RBRACE, TOK_LPAREN, TOK_RPAREN, TOK_LANGLE, TOK_RANGLE,
    TOK_SEMICOLON, TOK_COMMA, TOK_DOT, TOK_ERROR
};

struct Token {
    TokenType   type;
    std::string value;
    int         line;
    int         column;
    
    Token() : type(TOK_EOF), line(0), column(0) {}
    Token(TokenType t, const std::string& v, int l, int c)
        : type(t), value(v), line(l), column(c) {}
};

class Lexer {
public:
    Lexer(const std::string& source);
    Token nextToken();
    bool hasError() const { return has_error_; }
    const std::string& errorMessage() const { return error_msg_; }
    
private:
    void skipWhitespaceAndComments();
    Token readIdentifierOrKeyword();
    Token readNumber();
    Token readString();
    char peek() const;
    char advance();
    bool isAtEnd() const;
    
    std::string source_;
    size_t pos_;
    int line_;
    int column_;
    bool has_error_;
    std::string error_msg_;
};

const char* tokenTypeToString(TokenType type);

} // namespace omnic

#endif // BINDERC_LEXER_H

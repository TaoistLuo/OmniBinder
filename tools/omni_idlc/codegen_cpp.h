/**************************************************************************************************
 * @file        codegen_cpp.h
 * @brief       C++ 代码生成器
 * @details     从 AST 生成 C++ 头文件和源文件，包括结构体定义及序列化/反序列化、
 *              话题类（TopicPublisher/TopicSubscriber）、服务 Stub（服务端骨架，
 *              自动方法分发）和 Proxy（客户端代理，类型安全的 RPC 调用封装）。
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
#ifndef BINDERC_CODEGEN_CPP_H
#define BINDERC_CODEGEN_CPP_H

#include "codegen.h"

namespace omnic {

class CppCodeGen {
public:
    bool generate(const AstFile& ast, const std::string& output_dir,
                  const std::string& filename);
private:
    void generateHeader(const AstFile& ast, std::ostream& os, const std::string& filename);
    void generateSource(const AstFile& ast, std::ostream& os, const std::string& filename);
    
    void genStruct(const StructDef& s, std::ostream& os);
    void genTopic(const TopicDef& t, std::ostream& os);
    void genStub(const ServiceDef& svc, const AstFile& ast, std::ostream& os);
    void genProxy(const ServiceDef& svc, const AstFile& ast, std::ostream& os);
    
    void genSerialize(const std::vector<FieldDef>& fields, const std::string& obj, std::ostream& os);
    void genDeserialize(const std::vector<FieldDef>& fields, const std::string& obj, std::ostream& os);
    
    std::string pkg_;
};

} // namespace omnic

#endif

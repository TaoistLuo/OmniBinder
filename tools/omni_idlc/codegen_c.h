/**************************************************************************************************
 * @file        codegen_c.h
 * @brief       C 语言代码生成器
 * @details     从 AST 生成纯 C 头文件和源文件，包括结构体定义及序列化/反序列化函数、
 *              话题序列化/反序列化函数、服务 Stub（回调表 + 分发函数）和
 *              Proxy（类型安全的 RPC 调用封装函数）。生成的代码依赖 omnibinder_c.h。
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
#ifndef BINDERC_CODEGEN_C_H
#define BINDERC_CODEGEN_C_H

#include "codegen.h"

namespace omnic {

class CCodeGen {
public:
    bool generate(const AstFile& ast, const std::string& output_dir,
                  const std::string& filename);
private:
    void generateHeader(const AstFile& ast, std::ostream& os, const std::string& filename);
    void generateSource(const AstFile& ast, std::ostream& os, const std::string& filename);

    void genStruct(const StructDef& s, std::ostream& os);
    void genStructSerialize(const StructDef& s, std::ostream& os);
    void genStructDeserialize(const StructDef& s, std::ostream& os);

    void genTopic(const TopicDef& t, std::ostream& os);
    void genTopicSerialize(const TopicDef& t, std::ostream& os);
    void genTopicDeserialize(const TopicDef& t, std::ostream& os);

    void genServiceStubHeader(const ServiceDef& svc, const AstFile& ast, std::ostream& os);
    void genServiceStubSource(const ServiceDef& svc, const AstFile& ast, std::ostream& os);

    void genServiceProxyHeader(const ServiceDef& svc, const AstFile& ast, std::ostream& os);
    void genServiceProxySource(const ServiceDef& svc, const AstFile& ast, std::ostream& os);

    void genFieldSerialize(const FieldDef& f, const std::string& obj, std::ostream& os);
    void genFieldDeserialize(const FieldDef& f, const std::string& obj, std::ostream& os);

    std::string toSnakeCase(const std::string& name);

    std::string pkg_;
};

} // namespace omnic

#endif

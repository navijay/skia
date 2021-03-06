/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkSLHCodeGenerator.h"

#include "SkSLUtil.h"
#include "ir/SkSLFunctionDeclaration.h"
#include "ir/SkSLFunctionDefinition.h"
#include "ir/SkSLSection.h"
#include "ir/SkSLVarDeclarations.h"

namespace SkSL {

HCodeGenerator::HCodeGenerator(const Program* program, ErrorReporter* errors, String name,
                               OutputStream* out)
: INHERITED(program, errors, out)
, fName(std::move(name))
, fFullName(String::printf("Gr%s", fName.c_str()))
, fSectionAndParameterHelper(*program, *errors) {}

String HCodeGenerator::ParameterType(const Type& type) {
    if (type.fName == "vec2") {
        return "SkPoint";
    } else if (type.fName == "ivec4") {
        return "SkIRect";
    } else if (type.fName == "vec4") {
        return "SkRect";
    } else if (type.fName == "mat4") {
        return "SkMatrix44";
    } else if (type.kind() == Type::kSampler_Kind) {
        return "sk_sp<GrTextureProxy>";
    } else if (type.fName == "colorSpaceXform") {
        return "sk_sp<GrColorSpaceXform>";
    }
    return type.name();
}

String HCodeGenerator::FieldType(const Type& type) {
    if (type.kind() == Type::kSampler_Kind) {
        return "TextureSampler";
    }
    return ParameterType(type);
}

void HCodeGenerator::writef(const char* s, va_list va) {
    static constexpr int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    int length = vsnprintf(buffer, BUFFER_SIZE, s, va);
    if (length < BUFFER_SIZE) {
        fOut->write(buffer, length);
    } else {
        std::unique_ptr<char[]> heap(new char[length + 1]);
        vsprintf(heap.get(), s, va);
        fOut->write(heap.get(), length);
    }
}

void HCodeGenerator::writef(const char* s, ...) {
    va_list va;
    va_start(va, s);
    this->writef(s, va);
    va_end(va);
}

bool HCodeGenerator::writeSection(const char* name, const char* prefix) {
    const auto found = fSectionAndParameterHelper.fSections.find(String(name));
    if (found != fSectionAndParameterHelper.fSections.end()) {
        this->writef("%s%s", prefix, found->second->fText.c_str());
        return true;
    }
    return false;
}

void HCodeGenerator::writeExtraConstructorParams(const char* separator) {
    // super-simple parse, just assume the last token before a comma is the name of a parameter
    // (which is true as long as there are no multi-parameter template types involved). Will replace
    // this with something more robust if the need arises.
    const auto found = fSectionAndParameterHelper.fSections.find(
                                                                String(CONSTRUCTOR_PARAMS_SECTION));
    if (found != fSectionAndParameterHelper.fSections.end()) {
        const char* s = found->second->fText.c_str();
        #define BUFFER_SIZE 64
        char lastIdentifier[BUFFER_SIZE];
        int lastIdentifierLength = 0;
        bool foundBreak = false;
        while (*s) {
            char c = *s;
            ++s;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_') {
                if (foundBreak) {
                    lastIdentifierLength = 0;
                    foundBreak = false;
                }
                ASSERT(lastIdentifierLength < BUFFER_SIZE);
                lastIdentifier[lastIdentifierLength] = c;
                ++lastIdentifierLength;
            } else {
                foundBreak = true;
                if (c == ',') {
                    ASSERT(lastIdentifierLength < BUFFER_SIZE);
                    lastIdentifier[lastIdentifierLength] = 0;
                    this->writef("%s%s", separator, lastIdentifier);
                    separator = ", ";
                } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    lastIdentifierLength = 0;
                }
            }
        }
        if (lastIdentifierLength) {
            ASSERT(lastIdentifierLength < BUFFER_SIZE);
            lastIdentifier[lastIdentifierLength] = 0;
            this->writef("%s%s", separator, lastIdentifier);
        }
    }
}

void HCodeGenerator::writeMake() {
    const char* separator;
    if (!this->writeSection(MAKE_SECTION)) {
        this->writef("    static sk_sp<GrFragmentProcessor> Make(");
        separator = "";
        for (const auto& param : fSectionAndParameterHelper.fParameters) {
            this->writef("%s%s %s", separator, ParameterType(param->fType).c_str(),
                         param->fName.c_str());
            separator = ", ";
        }
        this->writeSection(CONSTRUCTOR_PARAMS_SECTION, separator);
        this->writef(") {\n"
                     "        return sk_sp<GrFragmentProcessor>(new %s(",
                     fFullName.c_str());
        separator = "";
        for (const auto& param : fSectionAndParameterHelper.fParameters) {
            this->writef("%s%s", separator, param->fName.c_str());
            separator = ", ";
        }
        this->writeExtraConstructorParams(separator);
        this->writef("));\n"
                     "    }\n");
    }
}

void HCodeGenerator::writeConstructor() {
    if (this->writeSection(CONSTRUCTOR_SECTION)) {
        return;
    }
    this->writef("    %s(", fFullName.c_str());
    const char* separator = "";
    for (const auto& param : fSectionAndParameterHelper.fParameters) {
        this->writef("%s%s %s", separator, ParameterType(param->fType).c_str(),
                     param->fName.c_str());
        separator = ", ";
    }
    this->writeSection(CONSTRUCTOR_PARAMS_SECTION, separator);
    this->writef(")\n"
                 "    : INHERITED(");
    if (!this->writeSection(OPTIMIZATION_FLAGS_SECTION, "(OptimizationFlags) ")) {
        this->writef("kNone_OptimizationFlags");
    }
    this->writef(")");
    this->writeSection(INITIALIZERS_SECTION, "\n    , ");
    for (const auto& param : fSectionAndParameterHelper.fParameters) {
        const char* name = param->fName.c_str();
        if (param->fType.kind() == Type::kSampler_Kind) {
            this->writef("\n    , %s(resourceProvider, std::move(%s))", FieldName(name).c_str(),
                         name);
        } else {
            this->writef("\n    , %s(%s)", FieldName(name).c_str(), name);
        }
    }
    this->writef(" {\n");
    this->writeSection(CONSTRUCTOR_CODE_SECTION);
    for (const auto& param : fSectionAndParameterHelper.fParameters) {
        if (param->fType.kind() == Type::kSampler_Kind) {
            this->writef("        this->addTextureSampler(&%s);\n",
                         FieldName(param->fName.c_str()).c_str());
        }
    }
    this->writef("        this->initClassID<%s>();\n"
                 "    }\n",
                 fFullName.c_str());
}

void HCodeGenerator::writeFields() {
    this->writeSection(FIELDS_SECTION);
    for (const auto& param : fSectionAndParameterHelper.fParameters) {
        const char* name = param->fName.c_str();
        this->writef("    %s %s;\n", FieldType(param->fType).c_str(), FieldName(name).c_str());
    }
}

bool HCodeGenerator::generateCode() {
    this->writef(kFragmentProcessorHeader, fFullName.c_str());
    this->writef("#ifndef %s_DEFINED\n"
                 "#define %s_DEFINED\n"
                 "#include \"GrFragmentProcessor.h\"\n"
                 "#include \"GrCoordTransform.h\"\n"
                 "#include \"effects/GrProxyMove.h\"\n",
                 fFullName.c_str(), fFullName.c_str());
    this->writeSection(HEADER_SECTION);
    this->writef("class %s : public GrFragmentProcessor {\n"
                 "public:\n",
                 fFullName.c_str());
    this->writeSection(CLASS_SECTION);
    for (const auto& param : fSectionAndParameterHelper.fParameters) {
        if (param->fType.kind() == Type::kSampler_Kind) {
            continue;
        }
        const char* name = param->fName.c_str();
        this->writef("%s %s() const { return %s; }\n",
                     FieldType(param->fType).c_str(), name, FieldName(name).c_str());
    }
    this->writeMake();
    this->writef("    const char* name() const override { return \"%s\"; }\n"
                 "private:\n",
                 fName.c_str());
    this->writeConstructor();
    this->writef("    GrGLSLFragmentProcessor* onCreateGLSLInstance() const override;\n"
                 "    void onGetGLSLProcessorKey(const GrShaderCaps&,"
                                                "GrProcessorKeyBuilder*) const override;\n"
                 "    bool onIsEqual(const GrFragmentProcessor&) const override;\n"
                 "    GR_DECLARE_FRAGMENT_PROCESSOR_TEST;\n");
    this->writeFields();
    this->writef("    typedef GrFragmentProcessor INHERITED;\n"
                 "};\n"
                 "#endif\n");
    return 0 == fErrors.errorCount();
}

} // namespace

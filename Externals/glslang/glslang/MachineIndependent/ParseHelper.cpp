//
// Copyright (C) 2002-2005  3Dlabs Inc. Ltd.
// Copyright (C) 2012-2015 LunarG, Inc.
// Copyright (C) 2015-2016 Google, Inc.
// Copyright (C) 2017 ARM Limited.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include "ParseHelper.h"
#include "Scan.h"

#include "../OSDependent/osinclude.h"
#include <algorithm>

#include "preprocessor/PpContext.h"

extern int yyparse(glslang::TParseContext*);

namespace glslang {

TParseContext::TParseContext(TSymbolTable& symbolTable, TIntermediate& interm, bool parsingBuiltins,
                             int version, EProfile profile, const SpvVersion& spvVersion, EShLanguage language,
                             TInfoSink& infoSink, bool forwardCompatible, EShMessages messages,
                             const TString* entryPoint) :
            TParseContextBase(symbolTable, interm, parsingBuiltins, version, profile, spvVersion, language,
                              infoSink, forwardCompatible, messages, entryPoint),
            inMain(false),
            blockName(nullptr),
            limits(resources.limits),
            atomicUintOffsets(nullptr), anyIndexLimits(false)
{
    // decide whether precision qualifiers should be ignored or respected
    if (profile == EEsProfile || spvVersion.vulkan > 0) {
        precisionManager.respectPrecisionQualifiers();
        if (! parsingBuiltins && language == EShLangFragment && profile != EEsProfile && spvVersion.vulkan > 0)
            precisionManager.warnAboutDefaults();
    }

    setPrecisionDefaults();

    globalUniformDefaults.clear();
    globalUniformDefaults.layoutMatrix = ElmColumnMajor;
    globalUniformDefaults.layoutPacking = spvVersion.spv != 0 ? ElpStd140 : ElpShared;

    globalBufferDefaults.clear();
    globalBufferDefaults.layoutMatrix = ElmColumnMajor;
    globalBufferDefaults.layoutPacking = spvVersion.spv != 0 ? ElpStd430 : ElpShared;

    globalInputDefaults.clear();
    globalOutputDefaults.clear();

    // "Shaders in the transform
    // feedback capturing mode have an initial global default of
    //     layout(xfb_buffer = 0) out;"
    if (language == EShLangVertex ||
        language == EShLangTessControl ||
        language == EShLangTessEvaluation ||
        language == EShLangGeometry)
        globalOutputDefaults.layoutXfbBuffer = 0;

    if (language == EShLangGeometry)
        globalOutputDefaults.layoutStream = 0;

    if (entryPoint != nullptr && entryPoint->size() > 0 && *entryPoint != "main")
        infoSink.info.message(EPrefixError, "Source entry point must be \"main\"");
}

TParseContext::~TParseContext()
{
    delete [] atomicUintOffsets;
}

// Set up all default precisions as needed by the current environment.
// Intended just as a TParseContext constructor helper.
void TParseContext::setPrecisionDefaults()
{
    // Set all precision defaults to EpqNone, which is correct for all types
    // when not obeying precision qualifiers, and correct for types that don't
    // have defaults (thus getting an error on use) when obeying precision
    // qualifiers.

    for (int type = 0; type < EbtNumTypes; ++type)
        defaultPrecision[type] = EpqNone;

    for (int type = 0; type < maxSamplerIndex; ++type)
        defaultSamplerPrecision[type] = EpqNone;

    // replace with real precision defaults for those that have them
    if (obeyPrecisionQualifiers()) {
        if (profile == EEsProfile) {
            // Most don't have defaults, a few default to lowp.
            TSampler sampler;
            sampler.set(EbtFloat, Esd2D);
            defaultSamplerPrecision[computeSamplerTypeIndex(sampler)] = EpqLow;
            sampler.set(EbtFloat, EsdCube);
            defaultSamplerPrecision[computeSamplerTypeIndex(sampler)] = EpqLow;
            sampler.set(EbtFloat, Esd2D);
            sampler.external = true;
            defaultSamplerPrecision[computeSamplerTypeIndex(sampler)] = EpqLow;
        }

        // If we are parsing built-in computational variables/functions, it is meaningful to record
        // whether the built-in has no precision qualifier, as that ambiguity
        // is used to resolve the precision from the supplied arguments/operands instead.
        // So, we don't actually want to replace EpqNone with a default precision for built-ins.
        if (! parsingBuiltins) {
            if (profile == EEsProfile && language == EShLangFragment) {
                defaultPrecision[EbtInt] = EpqMedium;
                defaultPrecision[EbtUint] = EpqMedium;
            } else {
                defaultPrecision[EbtInt] = EpqHigh;
                defaultPrecision[EbtUint] = EpqHigh;
                defaultPrecision[EbtFloat] = EpqHigh;
            }

            if (profile != EEsProfile) {
                // Non-ES profile
                // All sampler precisions default to highp.
                for (int type = 0; type < maxSamplerIndex; ++type)
                    defaultSamplerPrecision[type] = EpqHigh;
            }
        }

        defaultPrecision[EbtSampler] = EpqLow;
        defaultPrecision[EbtAtomicUint] = EpqHigh;
    }
}

void TParseContext::setLimits(const TBuiltInResource& r)
{
    resources = r;

    anyIndexLimits = ! limits.generalAttributeMatrixVectorIndexing ||
                     ! limits.generalConstantMatrixVectorIndexing ||
                     ! limits.generalSamplerIndexing ||
                     ! limits.generalUniformIndexing ||
                     ! limits.generalVariableIndexing ||
                     ! limits.generalVaryingIndexing;

    intermediate.setLimits(resources);

    // "Each binding point tracks its own current default offset for
    // inheritance of subsequent variables using the same binding. The initial state of compilation is that all
    // binding points have an offset of 0."
    atomicUintOffsets = new int[resources.maxAtomicCounterBindings];
    for (int b = 0; b < resources.maxAtomicCounterBindings; ++b)
        atomicUintOffsets[b] = 0;
}

//
// Parse an array of strings using yyparse, going through the
// preprocessor to tokenize the shader strings, then through
// the GLSL scanner.
//
// Returns true for successful acceptance of the shader, false if any errors.
//
bool TParseContext::parseShaderStrings(TPpContext& ppContext, TInputScanner& input, bool versionWillBeError)
{
    currentScanner = &input;
    ppContext.setInput(input, versionWillBeError);
    yyparse(this);

    finish();

    return numErrors == 0;
}

// This is called from bison when it has a parse (syntax) error
// Note though that to stop cascading errors, we set EOF, which
// will usually cause a syntax error, so be more accurate that
// compilation is terminating.
void TParseContext::parserError(const char* s)
{
    if (! getScanner()->atEndOfInput() || numErrors == 0)
        error(getCurrentLoc(), "", "", s, "");
    else
        error(getCurrentLoc(), "compilation terminated", "", "");
}

void TParseContext::handlePragma(const TSourceLoc& loc, const TVector<TString>& tokens)
{
    if (pragmaCallback)
        pragmaCallback(loc.line, tokens);

    if (tokens.size() == 0)
        return;

    if (tokens[0].compare("optimize") == 0) {
        if (tokens.size() != 4) {
            error(loc, "optimize pragma syntax is incorrect", "#pragma", "");
            return;
        }

        if (tokens[1].compare("(") != 0) {
            error(loc, "\"(\" expected after 'optimize' keyword", "#pragma", "");
            return;
        }

        if (tokens[2].compare("on") == 0)
            contextPragma.optimize = true;
        else if (tokens[2].compare("off") == 0)
            contextPragma.optimize = false;
        else {
            error(loc, "\"on\" or \"off\" expected after '(' for 'optimize' pragma", "#pragma", "");
            return;
        }

        if (tokens[3].compare(")") != 0) {
            error(loc, "\")\" expected to end 'optimize' pragma", "#pragma", "");
            return;
        }
    } else if (tokens[0].compare("debug") == 0) {
        if (tokens.size() != 4) {
            error(loc, "debug pragma syntax is incorrect", "#pragma", "");
            return;
        }

        if (tokens[1].compare("(") != 0) {
            error(loc, "\"(\" expected after 'debug' keyword", "#pragma", "");
            return;
        }

        if (tokens[2].compare("on") == 0)
            contextPragma.debug = true;
        else if (tokens[2].compare("off") == 0)
            contextPragma.debug = false;
        else {
            error(loc, "\"on\" or \"off\" expected after '(' for 'debug' pragma", "#pragma", "");
            return;
        }

        if (tokens[3].compare(")") != 0) {
            error(loc, "\")\" expected to end 'debug' pragma", "#pragma", "");
            return;
        }
    } else if (spvVersion.spv > 0 && tokens[0].compare("use_storage_buffer") == 0) {
        if (tokens.size() != 1)
            error(loc, "extra tokens", "#pragma", "");
        intermediate.setUseStorageBuffer();
    } else if (spvVersion.spv > 0 && tokens[0].compare("use_vulkan_memory_model") == 0) {
        if (tokens.size() != 1)
            error(loc, "extra tokens", "#pragma", "");
        intermediate.setUseVulkanMemoryModel();
    } else if (tokens[0].compare("once") == 0) {
        warn(loc, "not implemented", "#pragma once", "");
    } else if (tokens[0].compare("glslang_binary_double_output") == 0)
        intermediate.setBinaryDoubleOutput();
}

//
// Handle seeing a variable identifier in the grammar.
//
TIntermTyped* TParseContext::handleVariable(const TSourceLoc& loc, TSymbol* symbol, const TString* string)
{
    TIntermTyped* node = nullptr;

    // Error check for requiring specific extensions present.
    if (symbol && symbol->getNumExtensions())
        requireExtensions(loc, symbol->getNumExtensions(), symbol->getExtensions(), symbol->getName().c_str());

    if (symbol && symbol->isReadOnly()) {
        // All shared things containing an unsized array must be copied up
        // on first use, so that all future references will share its array structure,
        // so that editing the implicit size will effect all nodes consuming it,
        // and so that editing the implicit size won't change the shared one.
        //
        // If this is a variable or a block, check it and all it contains, but if this
        // is a member of an anonymous block, check the whole block, as the whole block
        // will need to be copied up if it contains an unsized array.
        if (symbol->getType().containsUnsizedArray() ||
            (symbol->getAsAnonMember() &&
             symbol->getAsAnonMember()->getAnonContainer().getType().containsUnsizedArray()))
            makeEditable(symbol);
    }

    const TVariable* variable;
    const TAnonMember* anon = symbol ? symbol->getAsAnonMember() : nullptr;
    if (anon) {
        // It was a member of an anonymous container.

        // The "getNumExtensions()" mechanism above doesn't yet work for block members
        blockMemberExtensionCheck(loc, nullptr, *string);

        // Create a subtree for its dereference.
        variable = anon->getAnonContainer().getAsVariable();
        TIntermTyped* container = intermediate.addSymbol(*variable, loc);
        TIntermTyped* constNode = intermediate.addConstantUnion(anon->getMemberNumber(), loc);
        node = intermediate.addIndex(EOpIndexDirectStruct, container, constNode, loc);

        node->setType(*(*variable->getType().getStruct())[anon->getMemberNumber()].type);
        if (node->getType().hiddenMember())
            error(loc, "member of nameless block was not redeclared", string->c_str(), "");
    } else {
        // Not a member of an anonymous container.

        // The symbol table search was done in the lexical phase.
        // See if it was a variable.
        variable = symbol ? symbol->getAsVariable() : nullptr;
        if (variable) {
            if ((variable->getType().getBasicType() == EbtBlock ||
                 variable->getType().getBasicType() == EbtStruct) && variable->getType().getStruct() == nullptr) {
                error(loc, "cannot be used (maybe an instance name is needed)", string->c_str(), "");
                variable = nullptr;
            }
        } else {
            if (symbol)
                error(loc, "variable name expected", string->c_str(), "");
        }

        // Recovery, if it wasn't found or was not a variable.
        if (! variable)
            variable = new TVariable(string, TType(EbtVoid));

        if (variable->getType().getQualifier().isFrontEndConstant())
            node = intermediate.addConstantUnion(variable->getConstArray(), variable->getType(), loc);
        else
            node = intermediate.addSymbol(*variable, loc);
    }

    if (variable->getType().getQualifier().isIo())
        intermediate.addIoAccessed(*string);

    return node;
}

//
// Handle seeing a base[index] dereference in the grammar.
//
TIntermTyped* TParseContext::handleBracketDereference(const TSourceLoc& loc, TIntermTyped* base, TIntermTyped* index)
{
    int indexValue = 0;
    if (index->getQualifier().isFrontEndConstant())
        indexValue = index->getAsConstantUnion()->getConstArray()[0].getIConst();

    // basic type checks...
    variableCheck(base);

    if (! base->isArray() && ! base->isMatrix() && ! base->isVector()) {
        if (base->getAsSymbolNode())
            error(loc, " left of '[' is not of type array, matrix, or vector ", base->getAsSymbolNode()->getName().c_str(), "");
        else
            error(loc, " left of '[' is not of type array, matrix, or vector ", "expression", "");

        // Insert dummy error-recovery result
        return intermediate.addConstantUnion(0.0, EbtFloat, loc);
    }

    if (!base->isArray() && base->isVector()) {
        if (base->getType().containsBasicType(EbtFloat16))
            requireFloat16Arithmetic(loc, "[", "does not operate on types containing float16");
        if (base->getType().contains16BitInt())
            requireInt16Arithmetic(loc, "[", "does not operate on types containing (u)int16");
        if (base->getType().contains8BitInt())
            requireInt8Arithmetic(loc, "[", "does not operate on types containing (u)int8");
    }

    // check for constant folding
    if (base->getType().getQualifier().isFrontEndConstant() && index->getQualifier().isFrontEndConstant()) {
        // both base and index are front-end constants
        checkIndex(loc, base->getType(), indexValue);
        return intermediate.foldDereference(base, indexValue, loc);
    }

    // at least one of base and index is not a front-end constant variable...
    TIntermTyped* result = nullptr;
    if (index->getQualifier().isFrontEndConstant())
        checkIndex(loc, base->getType(), indexValue);

    if (base->getAsSymbolNode() && isIoResizeArray(base->getType()))
        handleIoResizeArrayAccess(loc, base);

    if (index->getQualifier().isFrontEndConstant()) {
        if (base->getType().isUnsizedArray()) {
            base->getWritableType().updateImplicitArraySize(indexValue + 1);
#ifdef NV_EXTENSIONS
            // For 2D per-view builtin arrays, update the inner dimension size in parent type
            if (base->getQualifier().isPerView() && base->getQualifier().builtIn != EbvNone) {
                TIntermBinary* binaryNode = base->getAsBinaryNode();
                if (binaryNode) {
                    TType& leftType = binaryNode->getLeft()->getWritableType();
                    TArraySizes& arraySizes = *leftType.getArraySizes();
                    assert(arraySizes.getNumDims() == 2);
                    arraySizes.setDimSize(1, std::max(arraySizes.getDimSize(1), indexValue + 1));
                }
            }
#endif
        } else
            checkIndex(loc, base->getType(), indexValue);
        result = intermediate.addIndex(EOpIndexDirect, base, index, loc);
    } else {
        if (base->getType().isUnsizedArray()) {
            // we have a variable index into an unsized array, which is okay,
            // depending on the situation
            if (base->getAsSymbolNode() && isIoResizeArray(base->getType()))
                error(loc, "", "[", "array must be sized by a redeclaration or layout qualifier before being indexed with a variable");
            else {
                // it is okay for a run-time sized array
                checkRuntimeSizable(loc, *base);
            }
            base->getWritableType().setArrayVariablyIndexed();
        }
        if (base->getBasicType() == EbtBlock) {
            if (base->getQualifier().storage == EvqBuffer)
                requireProfile(base->getLoc(), ~EEsProfile, "variable indexing buffer block array");
            else if (base->getQualifier().storage == EvqUniform)
                profileRequires(base->getLoc(), EEsProfile, 320, Num_AEP_gpu_shader5, AEP_gpu_shader5,
                                "variable indexing uniform block array");
            else {
                // input/output blocks either don't exist or can be variable indexed
            }
        } else if (language == EShLangFragment && base->getQualifier().isPipeOutput())
            requireProfile(base->getLoc(), ~EEsProfile, "variable indexing fragment shader output array");
        else if (base->getBasicType() == EbtSampler && version >= 130) {
            const char* explanation = "variable indexing sampler array";
            requireProfile(base->getLoc(), EEsProfile | ECoreProfile | ECompatibilityProfile, explanation);
            profileRequires(base->getLoc(), EEsProfile, 320, Num_AEP_gpu_shader5, AEP_gpu_shader5, explanation);
            profileRequires(base->getLoc(), ECoreProfile | ECompatibilityProfile, 400, nullptr, explanation);
        }

        result = intermediate.addIndex(EOpIndexIndirect, base, index, loc);
    }

    // Insert valid dereferenced result
    TType newType(base->getType(), 0);  // dereferenced type
    if (base->getType().getQualifier().isConstant() && index->getQualifier().isConstant()) {
        newType.getQualifier().storage = EvqConst;
        // If base or index is a specialization constant, the result should also be a specialization constant.
        if (base->getType().getQualifier().isSpecConstant() || index->getQualifier().isSpecConstant()) {
            newType.getQualifier().makeSpecConstant();
        }
    } else {
        newType.getQualifier().makePartialTemporary();
    }
    result->setType(newType);

    // Propagate nonuniform
    if (base->getQualifier().isNonUniform() || index->getQualifier().isNonUniform())
        result->getWritableType().getQualifier().nonUniform = true;

    if (anyIndexLimits)
        handleIndexLimits(loc, base, index);

    return result;
}

// for ES 2.0 (version 100) limitations for almost all index operations except vertex-shader uniforms
void TParseContext::handleIndexLimits(const TSourceLoc& /*loc*/, TIntermTyped* base, TIntermTyped* index)
{
    if ((! limits.generalSamplerIndexing && base->getBasicType() == EbtSampler) ||
        (! limits.generalUniformIndexing && base->getQualifier().isUniformOrBuffer() && language != EShLangVertex) ||
        (! limits.generalAttributeMatrixVectorIndexing && base->getQualifier().isPipeInput() && language == EShLangVertex && (base->getType().isMatrix() || base->getType().isVector())) ||
        (! limits.generalConstantMatrixVectorIndexing && base->getAsConstantUnion()) ||
        (! limits.generalVariableIndexing && ! base->getType().getQualifier().isUniformOrBuffer() &&
                                             ! base->getType().getQualifier().isPipeInput() &&
                                             ! base->getType().getQualifier().isPipeOutput() &&
                                             ! base->getType().getQualifier().isConstant()) ||
        (! limits.generalVaryingIndexing && (base->getType().getQualifier().isPipeInput() ||
                                                base->getType().getQualifier().isPipeOutput()))) {
        // it's too early to know what the inductive variables are, save it for post processing
        needsIndexLimitationChecking.push_back(index);
    }
}

// Make a shared symbol have a non-shared version that can be edited by the current
// compile, such that editing its type will not change the shared version and will
// effect all nodes sharing it.
void TParseContext::makeEditable(TSymbol*& symbol)
{
    TParseContextBase::makeEditable(symbol);

    // See if it's tied to IO resizing
    if (isIoResizeArray(symbol->getType()))
        ioArraySymbolResizeList.push_back(symbol);
}

// Return true if this is a geometry shader input array or tessellation control output array
// or mesh shader output array.
bool TParseContext::isIoResizeArray(const TType& type) const
{
    return type.isArray() &&
           ((language == EShLangGeometry    && type.getQualifier().storage == EvqVaryingIn) ||
            (language == EShLangTessControl && type.getQualifier().storage == EvqVaryingOut && ! type.getQualifier().patch)
#ifdef NV_EXTENSIONS
            ||
            (language == EShLangFragment && type.getQualifier().storage == EvqVaryingIn &&  type.getQualifier().pervertexNV) ||
            (language == EShLangMeshNV && type.getQualifier().storage == EvqVaryingOut && !type.getQualifier().perTaskNV)

#endif
            );
}

// If an array is not isIoResizeArray() but is an io array, make sure it has the right size
void TParseContext::fixIoArraySize(const TSourceLoc& loc, TType& type)
{
    if (! type.isArray() || type.getQualifier().patch || symbolTable.atBuiltInLevel())
        return;

    assert(! isIoResizeArray(type));

    if (type.getQualifier().storage != EvqVaryingIn || type.getQualifier().patch)
        return;

    if (language == EShLangTessControl || language == EShLangTessEvaluation) {
        if (type.getOuterArraySize() != resources.maxPatchVertices) {
            if (type.isSizedArray())
                error(loc, "tessellation input array size must be gl_MaxPatchVertices or implicitly sized", "[]", "");
            type.changeOuterArraySize(resources.maxPatchVertices);
        }
    }
}

// Issue any errors if the non-array object is missing arrayness WRT
// shader I/O that has array requirements.
// All arrayness checking is handled in array paths, this is for
void TParseContext::ioArrayCheck(const TSourceLoc& loc, const TType& type, const TString& identifier)
{
    if (! type.isArray() && ! symbolTable.atBuiltInLevel()) {
        if (type.getQualifier().isArrayedIo(language)
#ifdef NV_EXTENSIONS
            && !type.getQualifier().layoutPassthrough
#endif
           )
            error(loc, "type must be an array:", type.getStorageQualifierString(), identifier.c_str());
    }
}

// Handle a dereference of a geometry shader input array or tessellation control output array.
// See ioArraySymbolResizeList comment in ParseHelper.h.
//
void TParseContext::handleIoResizeArrayAccess(const TSourceLoc& /*loc*/, TIntermTyped* base)
{
    TIntermSymbol* symbolNode = base->getAsSymbolNode();
    assert(symbolNode);
    if (! symbolNode)
        return;

    // fix array size, if it can be fixed and needs to be fixed (will allow variable indexing)
    if (symbolNode->getType().isUnsizedArray()) {
        int newSize = getIoArrayImplicitSize(symbolNode->getType().getQualifier().isPerPrimitive());
        if (newSize > 0)
            symbolNode->getWritableType().changeOuterArraySize(newSize);
    }
}

// If there has been an input primitive declaration (geometry shader) or an output
// number of vertices declaration(tessellation shader), make sure all input array types
// match it in size.  Types come either from nodes in the AST or symbols in the
// symbol table.
//
// Types without an array size will be given one.
// Types already having a size that is wrong will get an error.
//
void TParseContext::checkIoArraysConsistency(const TSourceLoc& loc, bool tailOnly, bool isPerPrimitive)
{
    int requiredSize = getIoArrayImplicitSize(isPerPrimitive);
    if (requiredSize == 0)
        return;

    const char* feature;
    if (language == EShLangGeometry)
        feature = TQualifier::getGeometryString(intermediate.getInputPrimitive());
    else if (language == EShLangTessControl
#ifdef NV_EXTENSIONS
          || language == EShLangFragment
#endif
        )

        feature = "vertices";
#ifdef NV_EXTENSIONS
     else if (language == EShLangMeshNV) {
        feature = isPerPrimitive ? "max_primitives" : "max_vertices";
     }
#endif
    else
        feature = "unknown";

    if (tailOnly) {
        checkIoArrayConsistency(loc, requiredSize, feature, ioArraySymbolResizeList.back()->getWritableType(), ioArraySymbolResizeList.back()->getName());
        return;
    }

    for (size_t i = 0; i < ioArraySymbolResizeList.size(); ++i)
        checkIoArrayConsistency(loc, requiredSize, feature, ioArraySymbolResizeList[i]->getWritableType(), ioArraySymbolResizeList[i]->getName());
}

int TParseContext::getIoArrayImplicitSize(bool isPerPrimitive) const
{
    if (language == EShLangGeometry)
        return TQualifier::mapGeometryToSize(intermediate.getInputPrimitive());
    else if (language == EShLangTessControl)
        return intermediate.getVertices() != TQualifier::layoutNotSet ? intermediate.getVertices() : 0;
#ifdef NV_EXTENSIONS
    else if (language == EShLangFragment)
        return 3; //Number of vertices for Fragment shader is always three.
    else if (language == EShLangMeshNV) {
        if (isPerPrimitive) {
            return intermediate.getPrimitives() != TQualifier::layoutNotSet ? intermediate.getPrimitives() : 0;
        } else {
            return intermediate.getVertices() != TQualifier::layoutNotSet ? intermediate.getVertices() : 0;
        }
    }
#endif

    else
        return 0;
}

void TParseContext::checkIoArrayConsistency(const TSourceLoc& loc, int requiredSize, const char* feature, TType& type, const TString& name)
{
    if (type.isUnsizedArray())
        type.changeOuterArraySize(requiredSize);
    else if (type.getOuterArraySize() != requiredSize) {
        if (language == EShLangGeometry)
            error(loc, "inconsistent input primitive for array size of", feature, name.c_str());
        else if (language == EShLangTessControl)
            error(loc, "inconsistent output number of vertices for array size of", feature, name.c_str());
#ifdef NV_EXTENSIONS
        else if (language == EShLangFragment) {
            if (type.getOuterArraySize() > requiredSize)
                error(loc, " cannot be greater than 3 for pervertexNV", feature, name.c_str());
        }
        else if (language == EShLangMeshNV)
            error(loc, "inconsistent output array size of", feature, name.c_str());
#endif
        else
            assert(0);
    }
}

// Handle seeing a binary node with a math operation.
// Returns nullptr if not semantically allowed.
TIntermTyped* TParseContext::handleBinaryMath(const TSourceLoc& loc, const char* str, TOperator op, TIntermTyped* left, TIntermTyped* right)
{
    rValueErrorCheck(loc, str, left->getAsTyped());
    rValueErrorCheck(loc, str, right->getAsTyped());

    bool allowed = true;
    switch (op) {
    // TODO: Bring more source language-specific checks up from intermediate.cpp
    // to the specific parse helpers for that source language.
    case EOpLessThan:
    case EOpGreaterThan:
    case EOpLessThanEqual:
    case EOpGreaterThanEqual:
        if (! left->isScalar() || ! right->isScalar())
            allowed = false;
        break;
    default:
        break;
    }

    if (((left->getType().containsBasicType(EbtFloat16) || right->getType().containsBasicType(EbtFloat16)) && !float16Arithmetic()) ||
        ((left->getType().contains16BitInt() || right->getType().contains16BitInt()) && !int16Arithmetic()) ||
        ((left->getType().contains8BitInt() || right->getType().contains8BitInt()) && !int8Arithmetic())) {
        allowed = false;
    }

    TIntermTyped* result = nullptr;
    if (allowed)
        result = intermediate.addBinaryMath(op, left, right, loc);

    if (result == nullptr)
        binaryOpError(loc, str, left->getCompleteString(), right->getCompleteString());

    return result;
}

// Handle seeing a unary node with a math operation.
TIntermTyped* TParseContext::handleUnaryMath(const TSourceLoc& loc, const char* str, TOperator op, TIntermTyped* childNode)
{
    rValueErrorCheck(loc, str, childNode);

    bool allowed = true;
    if ((childNode->getType().containsBasicType(EbtFloat16) && !float16Arithmetic()) ||
        (childNode->getType().contains16BitInt() && !int16Arithmetic()) ||
        (childNode->getType().contains8BitInt() && !int8Arithmetic())) {
        allowed = false;
    }

    TIntermTyped* result = nullptr;
    
    if (allowed)
        result = intermediate.addUnaryMath(op, childNode, loc);

    if (result)
        return result;
    else
        unaryOpError(loc, str, childNode->getCompleteString());

    return childNode;
}

//
// Handle seeing a base.field dereference in the grammar.
//
TIntermTyped* TParseContext::handleDotDereference(const TSourceLoc& loc, TIntermTyped* base, const TString& field)
{
    variableCheck(base);

    //
    // .length() can't be resolved until we later see the function-calling syntax.
    // Save away the name in the AST for now.  Processing is completed in
    // handleLengthMethod().
    //
    if (field == "length") {
        if (base->isArray()) {
            profileRequires(loc, ENoProfile, 120, E_GL_3DL_array_objects, ".length");
            profileRequires(loc, EEsProfile, 300, nullptr, ".length");
        } else if (base->isVector() || base->isMatrix()) {
            const char* feature = ".length() on vectors and matrices";
            requireProfile(loc, ~EEsProfile, feature);
            profileRequires(loc, ~EEsProfile, 420, E_GL_ARB_shading_language_420pack, feature);
        } else {
            error(loc, "does not operate on this type:", field.c_str(), base->getType().getCompleteString().c_str());

            return base;
        }

        return intermediate.addMethod(base, TType(EbtInt), &field, loc);
    }

    // It's not .length() if we get to here.

    if (base->isArray()) {
        error(loc, "cannot apply to an array:", ".", field.c_str());

        return base;
    }

    // It's neither an array nor .length() if we get here,
    // leaving swizzles and struct/block dereferences.

    TIntermTyped* result = base;
    if ((base->isVector() || base->isScalar()) &&
        (base->isFloatingDomain() || base->isIntegerDomain() || base->getBasicType() == EbtBool)) {
        if (base->isScalar()) {
            const char* dotFeature = "scalar swizzle";
            requireProfile(loc, ~EEsProfile, dotFeature);
            profileRequires(loc, ~EEsProfile, 420, E_GL_ARB_shading_language_420pack, dotFeature);
        }

        TSwizzleSelectors<TVectorSelector> selectors;
        parseSwizzleSelector(loc, field, base->getVectorSize(), selectors);

        if (base->isVector() && selectors.size() != 1 && base->getType().containsBasicType(EbtFloat16))
            requireFloat16Arithmetic(loc, ".", "can't swizzle types containing float16");
        if (base->isVector() && selectors.size() != 1 && base->getType().contains16BitInt())
            requireInt16Arithmetic(loc, ".", "can't swizzle types containing (u)int16");
        if (base->isVector() && selectors.size() != 1 && base->getType().contains8BitInt())
            requireInt8Arithmetic(loc, ".", "can't swizzle types containing (u)int8");

        if (base->isScalar()) {
            if (selectors.size() == 1)
                return result;
            else {
                TType type(base->getBasicType(), EvqTemporary, selectors.size());
                // Swizzle operations propagate specialization-constantness
                if (base->getQualifier().isSpecConstant())
                    type.getQualifier().makeSpecConstant();
                return addConstructor(loc, base, type);
            }
        }

        if (base->getType().getQualifier().isFrontEndConstant())
            result = intermediate.foldSwizzle(base, selectors, loc);
        else {
            if (selectors.size() == 1) {
                TIntermTyped* index = intermediate.addConstantUnion(selectors[0], loc);
                result = intermediate.addIndex(EOpIndexDirect, base, index, loc);
                result->setType(TType(base->getBasicType(), EvqTemporary, base->getType().getQualifier().precision));
            } else {
                TIntermTyped* index = intermediate.addSwizzle(selectors, loc);
                result = intermediate.addIndex(EOpVectorSwizzle, base, index, loc);
                result->setType(TType(base->getBasicType(), EvqTemporary, base->getType().getQualifier().precision, selectors.size()));
            }
            // Swizzle operations propagate specialization-constantness
            if (base->getType().getQualifier().isSpecConstant())
                result->getWritableType().getQualifier().makeSpecConstant();
        }
    } else if (base->getBasicType() == EbtStruct || base->getBasicType() == EbtBlock) {
        const TTypeList* fields = base->getType().getStruct();
        bool fieldFound = false;
        int member;
        for (member = 0; member < (int)fields->size(); ++member) {
            if ((*fields)[member].type->getFieldName() == field) {
                fieldFound = true;
                break;
            }
        }
        if (fieldFound) {
            if (base->getType().getQualifier().isFrontEndConstant())
                result = intermediate.foldDereference(base, member, loc);
            else {
                blockMemberExtensionCheck(loc, base, field);
                TIntermTyped* index = intermediate.addConstantUnion(member, loc);
                result = intermediate.addIndex(EOpIndexDirectStruct, base, index, loc);
                result->setType(*(*fields)[member].type);
                if ((*fields)[member].type->getQualifier().isIo())
                    intermediate.addIoAccessed(field);
            }
        } else
            error(loc, "no such field in structure", field.c_str(), "");
    } else
        error(loc, "does not apply to this type:", field.c_str(), base->getType().getCompleteString().c_str());

    // Propagate noContraction up the dereference chain
    if (base->getQualifier().noContraction)
        result->getWritableType().getQualifier().noContraction = true;

    // Propagate nonuniform
    if (base->getQualifier().isNonUniform())
        result->getWritableType().getQualifier().nonUniform = true;

    return result;
}

void TParseContext::blockMemberExtensionCheck(const TSourceLoc& loc, const TIntermTyped* /*base*/, const TString& field)
{
    if (profile == EEsProfile && field == "gl_PointSize") {
        if (language == EShLangGeometry)
            requireExtensions(loc, Num_AEP_geometry_point_size, AEP_geometry_point_size, "gl_PointSize");
        else if (language == EShLangTessControl || language == EShLangTessEvaluation)
            requireExtensions(loc, Num_AEP_tessellation_point_size, AEP_tessellation_point_size, "gl_PointSize");
    }
}

//
// Handle seeing a function declarator in the grammar.  This is the precursor
// to recognizing a function prototype or function definition.
//
TFunction* TParseContext::handleFunctionDeclarator(const TSourceLoc& loc, TFunction& function, bool prototype)
{
    // ES can't declare prototypes inside functions
    if (! symbolTable.atGlobalLevel())
        requireProfile(loc, ~EEsProfile, "local function declaration");

    //
    // Multiple declarations of the same function name are allowed.
    //
    // If this is a definition, the definition production code will check for redefinitions
    // (we don't know at this point if it's a definition or not).
    //
    // Redeclarations (full signature match) are allowed.  But, return types and parameter qualifiers must also match.
    //  - except ES 100, which only allows a single prototype
    //
    // ES 100 does not allow redefining, but does allow overloading of built-in functions.
    // ES 300 does not allow redefining or overloading of built-in functions.
    //
    bool builtIn;
    TSymbol* symbol = symbolTable.find(function.getMangledName(), &builtIn);
    if (symbol && symbol->getAsFunction() && builtIn)
        requireProfile(loc, ~EEsProfile, "redefinition of built-in function");
    const TFunction* prevDec = symbol ? symbol->getAsFunction() : 0;
    if (prevDec) {
        if (prevDec->isPrototyped() && prototype)
            profileRequires(loc, EEsProfile, 300, nullptr, "multiple prototypes for same function");
        if (prevDec->getType() != function.getType())
            error(loc, "overloaded functions must have the same return type", function.getName().c_str(), "");
        for (int i = 0; i < prevDec->getParamCount(); ++i) {
            if ((*prevDec)[i].type->getQualifier().storage != function[i].type->getQualifier().storage)
                error(loc, "overloaded functions must have the same parameter storage qualifiers for argument", function[i].type->getStorageQualifierString(), "%d", i+1);

            if ((*prevDec)[i].type->getQualifier().precision != function[i].type->getQualifier().precision)
                error(loc, "overloaded functions must have the same parameter precision qualifiers for argument", function[i].type->getPrecisionQualifierString(), "%d", i+1);
        }
    }

    arrayObjectCheck(loc, function.getType(), "array in function return type");

    if (prototype) {
        // All built-in functions are defined, even though they don't have a body.
        // Count their prototype as a definition instead.
        if (symbolTable.atBuiltInLevel())
            function.setDefined();
        else {
            if (prevDec && ! builtIn)
                symbol->getAsFunction()->setPrototyped();  // need a writable one, but like having prevDec as a const
            function.setPrototyped();
        }
    }

    // This insert won't actually insert it if it's a duplicate signature, but it will still check for
    // other forms of name collisions.
    if (! symbolTable.insert(function))
        error(loc, "function name is redeclaration of existing name", function.getName().c_str(), "");

    //
    // If this is a redeclaration, it could also be a definition,
    // in which case, we need to use the parameter names from this one, and not the one that's
    // being redeclared.  So, pass back this declaration, not the one in the symbol table.
    //
    return &function;
}

//
// Handle seeing the function prototype in front of a function definition in the grammar.
// The body is handled after this function returns.
//
TIntermAggregate* TParseContext::handleFunctionDefinition(const TSourceLoc& loc, TFunction& function)
{
    currentCaller = function.getMangledName();
    TSymbol* symbol = symbolTable.find(function.getMangledName());
    TFunction* prevDec = symbol ? symbol->getAsFunction() : nullptr;

    if (! prevDec)
        error(loc, "can't find function", function.getName().c_str(), "");
    // Note:  'prevDec' could be 'function' if this is the first time we've seen function
    // as it would have just been put in the symbol table.  Otherwise, we're looking up
    // an earlier occurrence.

    if (prevDec && prevDec->isDefined()) {
        // Then this function already has a body.
        error(loc, "function already has a body", function.getName().c_str(), "");
    }
    if (prevDec && ! prevDec->isDefined()) {
        prevDec->setDefined();

        // Remember the return type for later checking for RETURN statements.
        currentFunctionType = &(prevDec->getType());
    } else
        currentFunctionType = new TType(EbtVoid);
    functionReturnsValue = false;

    // Check for entry point
    if (function.getName().compare(intermediate.getEntryPointName().c_str()) == 0) {
        intermediate.setEntryPointMangledName(function.getMangledName().c_str());
        intermediate.incrementEntryPointCount();
        inMain = true;
    } else
        inMain = false;

    //
    // Raise error message if main function takes any parameters or returns anything other than void
    //
    if (inMain) {
        if (function.getParamCount() > 0)
            error(loc, "function cannot take any parameter(s)", function.getName().c_str(), "");
        if (function.getType().getBasicType() != EbtVoid)
            error(loc, "", function.getType().getBasicTypeString().c_str(), "entry point cannot return a value");
    }

    //
    // New symbol table scope for body of function plus its arguments
    //
    symbolTable.push();

    //
    // Insert parameters into the symbol table.
    // If the parameter has no name, it's not an error, just don't insert it
    // (could be used for unused args).
    //
    // Also, accumulate the list of parameters into the HIL, so lower level code
    // knows where to find parameters.
    //
    TIntermAggregate* paramNodes = new TIntermAggregate;
    for (int i = 0; i < function.getParamCount(); i++) {
        TParameter& param = function[i];
        if (param.name != nullptr) {
            TVariable *variable = new TVariable(param.name, *param.type);

            // Insert the parameters with name in the symbol table.
            if (! symbolTable.insert(*variable))
                error(loc, "redefinition", variable->getName().c_str(), "");
            else {
                // Transfer ownership of name pointer to symbol table.
                param.name = nullptr;

                // Add the parameter to the HIL
                paramNodes = intermediate.growAggregate(paramNodes,
                                                        intermediate.addSymbol(*variable, loc),
                                                        loc);
            }
        } else
            paramNodes = intermediate.growAggregate(paramNodes, intermediate.addSymbol(*param.type, loc), loc);
    }
    intermediate.setAggregateOperator(paramNodes, EOpParameters, TType(EbtVoid), loc);
    loopNestingLevel = 0;
    statementNestingLevel = 0;
    controlFlowNestingLevel = 0;
    postEntryPointReturn = false;

    return paramNodes;
}

//
// Handle seeing function call syntax in the grammar, which could be any of
//  - .length() method
//  - constructor
//  - a call to a built-in function mapped to an operator
//  - a call to a built-in function that will remain a function call (e.g., texturing)
//  - user function
//  - subroutine call (not implemented yet)
//
TIntermTyped* TParseContext::handleFunctionCall(const TSourceLoc& loc, TFunction* function, TIntermNode* arguments)
{
    TIntermTyped* result = nullptr;

    if (function->getBuiltInOp() == EOpArrayLength)
        result = handleLengthMethod(loc, function, arguments);
    else if (function->getBuiltInOp() != EOpNull) {
        //
        // Then this should be a constructor.
        // Don't go through the symbol table for constructors.
        // Their parameters will be verified algorithmically.
        //
        TType type(EbtVoid);  // use this to get the type back
        if (! constructorError(loc, arguments, *function, function->getBuiltInOp(), type)) {
            //
            // It's a constructor, of type 'type'.
            //
            result = addConstructor(loc, arguments, type);
            if (result == nullptr)
                error(loc, "cannot construct with these arguments", type.getCompleteString().c_str(), "");
        }
    } else {
        //
        // Find it in the symbol table.
        //
        const TFunction* fnCandidate;
        bool builtIn;
        fnCandidate = findFunction(loc, *function, builtIn);
        if (fnCandidate) {
            // This is a declared function that might map to
            //  - a built-in operator,
            //  - a built-in function not mapped to an operator, or
            //  - a user function.

            // Error check for a function requiring specific extensions present.
            if (builtIn && fnCandidate->getNumExtensions())
                requireExtensions(loc, fnCandidate->getNumExtensions(), fnCandidate->getExtensions(), fnCandidate->getName().c_str());

            if (builtIn && fnCandidate->getType().containsBasicType(EbtFloat16))
                requireFloat16Arithmetic(loc, "built-in function", "float16 types can only be in uniform block or buffer storage");
            if (builtIn && fnCandidate->getType().contains16BitInt())
                requireInt16Arithmetic(loc, "built-in function", "(u)int16 types can only be in uniform block or buffer storage");
            if (builtIn && fnCandidate->getType().contains8BitInt())
                requireInt8Arithmetic(loc, "built-in function", "(u)int8 types can only be in uniform block or buffer storage");

            if (arguments != nullptr) {
                // Make sure qualifications work for these arguments.
                TIntermAggregate* aggregate = arguments->getAsAggregate();
                for (int i = 0; i < fnCandidate->getParamCount(); ++i) {
                    // At this early point there is a slight ambiguity between whether an aggregate 'arguments'
                    // is the single argument itself or its children are the arguments.  Only one argument
                    // means take 'arguments' itself as the one argument.
                    TIntermNode* arg = fnCandidate->getParamCount() == 1 ? arguments : (aggregate ? aggregate->getSequence()[i] : arguments);
                    TQualifier& formalQualifier = (*fnCandidate)[i].type->getQualifier();
                    if (formalQualifier.isParamOutput()) {
                        if (lValueErrorCheck(arguments->getLoc(), "assign", arg->getAsTyped()))
                            error(arguments->getLoc(), "Non-L-value cannot be passed for 'out' or 'inout' parameters.", "out", "");
                    }
                    TQualifier& argQualifier = arg->getAsTyped()->getQualifier();
                    if (argQualifier.isMemory()) {
                        const char* message = "argument cannot drop memory qualifier when passed to formal parameter";
                        if (argQualifier.volatil && ! formalQualifier.volatil)
                            error(arguments->getLoc(), message, "volatile", "");
                        if (argQualifier.coherent && ! (formalQualifier.devicecoherent || formalQualifier.coherent))
                            error(arguments->getLoc(), message, "coherent", "");
                        if (argQualifier.devicecoherent && ! (formalQualifier.devicecoherent || formalQualifier.coherent))
                            error(arguments->getLoc(), message, "devicecoherent", "");
                        if (argQualifier.queuefamilycoherent && ! (formalQualifier.queuefamilycoherent || formalQualifier.devicecoherent || formalQualifier.coherent))
                            error(arguments->getLoc(), message, "queuefamilycoherent", "");
                        if (argQualifier.workgroupcoherent && ! (formalQualifier.workgroupcoherent || formalQualifier.queuefamilycoherent || formalQualifier.devicecoherent || formalQualifier.coherent))
                            error(arguments->getLoc(), message, "workgroupcoherent", "");
                        if (argQualifier.subgroupcoherent && ! (formalQualifier.subgroupcoherent || formalQualifier.workgroupcoherent || formalQualifier.queuefamilycoherent || formalQualifier.devicecoherent || formalQualifier.coherent))
                            error(arguments->getLoc(), message, "subgroupcoherent", "");
                        if (argQualifier.readonly && ! formalQualifier.readonly)
                            error(arguments->getLoc(), message, "readonly", "");
                        if (argQualifier.writeonly && ! formalQualifier.writeonly)
                            error(arguments->getLoc(), message, "writeonly", "");
                    }

                    if (builtIn && arg->getAsTyped()->getType().containsBasicType(EbtFloat16))
                        requireFloat16Arithmetic(arguments->getLoc(), "built-in function", "float16 types can only be in uniform block or buffer storage");
                    if (builtIn && arg->getAsTyped()->getType().contains16BitInt())
                        requireInt16Arithmetic(arguments->getLoc(), "built-in function", "(u)int16 types can only be in uniform block or buffer storage");
                    if (builtIn && arg->getAsTyped()->getType().contains8BitInt())
                        requireInt8Arithmetic(arguments->getLoc(), "built-in function", "(u)int8 types can only be in uniform block or buffer storage");

                    // TODO 4.5 functionality:  A shader will fail to compile
                    // if the value passed to the memargument of an atomic memory function does not correspond to a buffer or
                    // shared variable. It is acceptable to pass an element of an array or a single component of a vector to the
                    // memargument of an atomic memory function, as long as the underlying array or vector is a buffer or
                    // shared variable.
                }

                // Convert 'in' arguments
                addInputArgumentConversions(*fnCandidate, arguments);  // arguments may be modified if it's just a single argument node
            }

            if (builtIn && fnCandidate->getBuiltInOp() != EOpNull) {
                // A function call mapped to a built-in operation.
                result = handleBuiltInFunctionCall(loc, arguments, *fnCandidate);
            } else {
                // This is a function call not mapped to built-in operator.
                // It could still be a built-in function, but only if PureOperatorBuiltins == false.
                result = intermediate.setAggregateOperator(arguments, EOpFunctionCall, fnCandidate->getType(), loc);
                TIntermAggregate* call = result->getAsAggregate();
                call->setName(fnCandidate->getMangledName());

                // this is how we know whether the given function is a built-in function or a user-defined function
                // if builtIn == false, it's a userDefined -> could be an overloaded built-in function also
                // if builtIn == true, it's definitely a built-in function with EOpNull
                if (! builtIn) {
                    call->setUserDefined();
                    if (symbolTable.atGlobalLevel()) {
                        requireProfile(loc, ~EEsProfile, "calling user function from global scope");
                        intermediate.addToCallGraph(infoSink, "main(", fnCandidate->getMangledName());
                    } else
                        intermediate.addToCallGraph(infoSink, currentCaller, fnCandidate->getMangledName());
                }

                if (builtIn)
                    nonOpBuiltInCheck(loc, *fnCandidate, *call);
                else
                    userFunctionCallCheck(loc, *call);
            }

            // Convert 'out' arguments.  If it was a constant folded built-in, it won't be an aggregate anymore.
            // Built-ins with a single argument aren't called with an aggregate, but they also don't have an output.
            // Also, build the qualifier list for user function calls, which are always called with an aggregate.
            if (result->getAsAggregate()) {
                TQualifierList& qualifierList = result->getAsAggregate()->getQualifierList();
                for (int i = 0; i < fnCandidate->getParamCount(); ++i) {
                    TStorageQualifier qual = (*fnCandidate)[i].type->getQualifier().storage;
                    qualifierList.push_back(qual);
                }
                result = addOutputArgumentConversions(*fnCandidate, *result->getAsAggregate());
            }
        }
    }

    // generic error recovery
    // TODO: simplification: localize all the error recoveries that look like this, and taking type into account to reduce cascades
    if (result == nullptr)
        result = intermediate.addConstantUnion(0.0, EbtFloat, loc);

    return result;
}

TIntermTyped* TParseContext::handleBuiltInFunctionCall(TSourceLoc loc, TIntermNode* arguments,
                                                       const TFunction& function)
{
    checkLocation(loc, function.getBuiltInOp());
    TIntermTyped *result = intermediate.addBuiltInFunctionCall(loc, function.getBuiltInOp(),
                                                               function.getParamCount() == 1,
                                                               arguments, function.getType());
    if (obeyPrecisionQualifiers())
        computeBuiltinPrecisions(*result, function);

    if (result == nullptr) {
        if (arguments == nullptr)
            error(loc, " wrong operand type", "Internal Error",
                                      "built in unary operator function.  Type: %s", "");
        else
            error(arguments->getLoc(), " wrong operand type", "Internal Error",
                                      "built in unary operator function.  Type: %s",
                                      static_cast<TIntermTyped*>(arguments)->getCompleteString().c_str());
    } else if (result->getAsOperator())
        builtInOpCheck(loc, function, *result->getAsOperator());

    return result;
}

// "The operation of a built-in function can have a different precision
// qualification than the precision qualification of the resulting value.
// These two precision qualifications are established as follows.
//
// The precision qualification of the operation of a built-in function is
// based on the precision qualification of its input arguments and formal
// parameters:  When a formal parameter specifies a precision qualifier,
// that is used, otherwise, the precision qualification of the calling
// argument is used.  The highest precision of these will be the precision
// qualification of the operation of the built-in function. Generally,
// this is applied across all arguments to a built-in function, with the
// exceptions being:
//   - bitfieldExtract and bitfieldInsert ignore the 'offset' and 'bits'
//     arguments.
//   - interpolateAt* functions only look at the 'interpolant' argument.
//
// The precision qualification of the result of a built-in function is
// determined in one of the following ways:
//
//   - For the texture sampling, image load, and image store functions,
//     the precision of the return type matches the precision of the
//     sampler type
//
//   Otherwise:
//
//   - For prototypes that do not specify a resulting precision qualifier,
//     the precision will be the same as the precision of the operation.
//
//   - For prototypes that do specify a resulting precision qualifier,
//     the specified precision qualifier is the precision qualification of
//     the result."
//
void TParseContext::computeBuiltinPrecisions(TIntermTyped& node, const TFunction& function)
{
    TPrecisionQualifier operationPrecision = EpqNone;
    TPrecisionQualifier resultPrecision = EpqNone;

    TIntermOperator* opNode = node.getAsOperator();
    if (opNode == nullptr)
        return;

    if (TIntermUnary* unaryNode = node.getAsUnaryNode()) {
        operationPrecision = std::max(function[0].type->getQualifier().precision,
                                      unaryNode->getOperand()->getType().getQualifier().precision);
        if (function.getType().getBasicType() != EbtBool)
            resultPrecision = function.getType().getQualifier().precision == EpqNone ?
                                        operationPrecision :
                                        function.getType().getQualifier().precision;
    } else if (TIntermAggregate* agg = node.getAsAggregate()) {
        TIntermSequence& sequence = agg->getSequence();
        unsigned int numArgs = (unsigned int)sequence.size();
        switch (agg->getOp()) {
        case EOpBitfieldExtract:
            numArgs = 1;
            break;
        case EOpBitfieldInsert:
            numArgs = 2;
            break;
        case EOpInterpolateAtCentroid:
        case EOpInterpolateAtOffset:
        case EOpInterpolateAtSample:
            numArgs = 1;
            break;
        default:
            break;
        }
        // find the maximum precision from the arguments and parameters
        for (unsigned int arg = 0; arg < numArgs; ++arg) {
            operationPrecision = std::max(operationPrecision, sequence[arg]->getAsTyped()->getQualifier().precision);
            operationPrecision = std::max(operationPrecision, function[arg].type->getQualifier().precision);
        }
        // compute the result precision
#ifdef AMD_EXTENSIONS
        if (agg->isSampling() ||
            agg->getOp() == EOpImageLoad || agg->getOp() == EOpImageStore ||
            agg->getOp() == EOpImageLoadLod || agg->getOp() == EOpImageStoreLod)
#else
        if (agg->isSampling() || agg->getOp() == EOpImageLoad || agg->getOp() == EOpImageStore)
#endif
            resultPrecision = sequence[0]->getAsTyped()->getQualifier().precision;
        else if (function.getType().getBasicType() != EbtBool)
            resultPrecision = function.getType().getQualifier().precision == EpqNone ?
                                        operationPrecision :
                                        function.getType().getQualifier().precision;
    }

    // Propagate precision through this node and its children. That algorithm stops
    // when a precision is found, so start by clearing this subroot precision
    opNode->getQualifier().precision = EpqNone;
    if (operationPrecision != EpqNone) {
        opNode->propagatePrecision(operationPrecision);
        opNode->setOperationPrecision(operationPrecision);
    }
    // Now, set the result precision, which might not match
    opNode->getQualifier().precision = resultPrecision;
}

TIntermNode* TParseContext::handleReturnValue(const TSourceLoc& loc, TIntermTyped* value)
{
    storage16BitAssignmentCheck(loc, value->getType(), "return");

    functionReturnsValue = true;
    if (currentFunctionType->getBasicType() == EbtVoid) {
        error(loc, "void function cannot return a value", "return", "");
        return intermediate.addBranch(EOpReturn, loc);
    } else if (*currentFunctionType != value->getType()) {
        TIntermTyped* converted = intermediate.addConversion(EOpReturn, *currentFunctionType, value);
        if (converted) {
            if (*currentFunctionType != converted->getType())
                error(loc, "cannot convert return value to function return type", "return", "");
            if (version < 420)
                warn(loc, "type conversion on return values was not explicitly allowed until version 420", "return", "");
            return intermediate.addBranch(EOpReturn, converted, loc);
        } else {
            error(loc, "type does not match, or is not convertible to, the function's return type", "return", "");
            return intermediate.addBranch(EOpReturn, value, loc);
        }
    } else
        return intermediate.addBranch(EOpReturn, value, loc);
}

// See if the operation is being done in an illegal location.
void TParseContext::checkLocation(const TSourceLoc& loc, TOperator op)
{
    switch (op) {
    case EOpBarrier:
        if (language == EShLangTessControl) {
            if (controlFlowNestingLevel > 0)
                error(loc, "tessellation control barrier() cannot be placed within flow control", "", "");
            if (! inMain)
                error(loc, "tessellation control barrier() must be in main()", "", "");
            else if (postEntryPointReturn)
                error(loc, "tessellation control barrier() cannot be placed after a return from main()", "", "");
        }
        break;
    default:
        break;
    }
}

// Finish processing object.length(). This started earlier in handleDotDereference(), where
// the ".length" part was recognized and semantically checked, and finished here where the
// function syntax "()" is recognized.
//
// Return resulting tree node.
TIntermTyped* TParseContext::handleLengthMethod(const TSourceLoc& loc, TFunction* function, TIntermNode* intermNode)
{
    int length = 0;

    if (function->getParamCount() > 0)
        error(loc, "method does not accept any arguments", function->getName().c_str(), "");
    else {
        const TType& type = intermNode->getAsTyped()->getType();
        if (type.isArray()) {
            if (type.isUnsizedArray()) {
                if (intermNode->getAsSymbolNode() && isIoResizeArray(type)) {
                    // We could be between a layout declaration that gives a built-in io array implicit size and
                    // a user redeclaration of that array, meaning we have to substitute its implicit size here
                    // without actually redeclaring the array.  (It is an error to use a member before the
                    // redeclaration, but not an error to use the array name itself.)
                    const TString& name = intermNode->getAsSymbolNode()->getName();
                    if (name == "gl_in" || name == "gl_out"
#ifdef NV_EXTENSIONS
                        || name == "gl_MeshVerticesNV"
                        || name == "gl_MeshPrimitivesNV"
#endif
                        )
                    {
                        length = getIoArrayImplicitSize(type.getQualifier().isPerPrimitive());
                    }
                }
                if (length == 0) {
                    if (intermNode->getAsSymbolNode() && isIoResizeArray(type))
                        error(loc, "", function->getName().c_str(), "array must first be sized by a redeclaration or layout qualifier");
                    else if (isRuntimeLength(*intermNode->getAsTyped())) {
                        // Create a unary op and let the back end handle it
                        return intermediate.addBuiltInFunctionCall(loc, EOpArrayLength, true, intermNode, TType(EbtInt));
                    } else
                        error(loc, "", function->getName().c_str(), "array must be declared with a size before using this method");
                }
            } else if (type.getOuterArrayNode()) {
                // If the array's outer size is specified by an intermediate node, it means the array's length
                // was specified by a specialization constant. In such a case, we should return the node of the
                // specialization constants to represent the length.
                return type.getOuterArrayNode();
            } else
                length = type.getOuterArraySize();
        } else if (type.isMatrix())
            length = type.getMatrixCols();
        else if (type.isVector())
            length = type.getVectorSize();
        else {
            // we should not get here, because earlier semantic checking should have prevented this path
            error(loc, ".length()", "unexpected use of .length()", "");
        }
    }

    if (length == 0)
        length = 1;

    return intermediate.addConstantUnion(length, loc);
}

//
// Add any needed implicit conversions for function-call arguments to input parameters.
//
void TParseContext::addInputArgumentConversions(const TFunction& function, TIntermNode*& arguments) const
{
    TIntermAggregate* aggregate = arguments->getAsAggregate();

    // Process each argument's conversion
    for (int i = 0; i < function.getParamCount(); ++i) {
        // At this early point there is a slight ambiguity between whether an aggregate 'arguments'
        // is the single argument itself or its children are the arguments.  Only one argument
        // means take 'arguments' itself as the one argument.
        TIntermTyped* arg = function.getParamCount() == 1 ? arguments->getAsTyped() : (aggregate ? aggregate->getSequence()[i]->getAsTyped() : arguments->getAsTyped());
        if (*function[i].type != arg->getType()) {
            if (function[i].type->getQualifier().isParamInput()) {
                // In-qualified arguments just need an extra node added above the argument to
                // convert to the correct type.
                arg = intermediate.addConversion(EOpFunctionCall, *function[i].type, arg);
                if (arg) {
                    if (function.getParamCount() == 1)
                        arguments = arg;
                    else {
                        if (aggregate)
                            aggregate->getSequence()[i] = arg;
                        else
                            arguments = arg;
                    }
                }
            }
        }
    }
}

//
// Add any needed implicit output conversions for function-call arguments.  This
// can require a new tree topology, complicated further by whether the function
// has a return value.
//
// Returns a node of a subtree that evaluates to the return value of the function.
//
TIntermTyped* TParseContext::addOutputArgumentConversions(const TFunction& function, TIntermAggregate& intermNode) const
{
    TIntermSequence& arguments = intermNode.getSequence();

    // Will there be any output conversions?
    bool outputConversions = false;
    for (int i = 0; i < function.getParamCount(); ++i) {
        if (*function[i].type != arguments[i]->getAsTyped()->getType() && function[i].type->getQualifier().isParamOutput()) {
            outputConversions = true;
            break;
        }
    }

    if (! outputConversions)
        return &intermNode;

    // Setup for the new tree, if needed:
    //
    // Output conversions need a different tree topology.
    // Out-qualified arguments need a temporary of the correct type, with the call
    // followed by an assignment of the temporary to the original argument:
    //     void: function(arg, ...)  ->        (          function(tempArg, ...), arg = tempArg, ...)
    //     ret = function(arg, ...)  ->  ret = (tempRet = function(tempArg, ...), arg = tempArg, ..., tempRet)
    // Where the "tempArg" type needs no conversion as an argument, but will convert on assignment.
    TIntermTyped* conversionTree = nullptr;
    TVariable* tempRet = nullptr;
    if (intermNode.getBasicType() != EbtVoid) {
        // do the "tempRet = function(...), " bit from above
        tempRet = makeInternalVariable("tempReturn", intermNode.getType());
        TIntermSymbol* tempRetNode = intermediate.addSymbol(*tempRet, intermNode.getLoc());
        conversionTree = intermediate.addAssign(EOpAssign, tempRetNode, &intermNode, intermNode.getLoc());
    } else
        conversionTree = &intermNode;

    conversionTree = intermediate.makeAggregate(conversionTree);

    // Process each argument's conversion
    for (int i = 0; i < function.getParamCount(); ++i) {
        if (*function[i].type != arguments[i]->getAsTyped()->getType()) {
            if (function[i].type->getQualifier().isParamOutput()) {
                // Out-qualified arguments need to use the topology set up above.
                // do the " ...(tempArg, ...), arg = tempArg" bit from above
                TVariable* tempArg = makeInternalVariable("tempArg", *function[i].type);
                tempArg->getWritableType().getQualifier().makeTemporary();
                TIntermSymbol* tempArgNode = intermediate.addSymbol(*tempArg, intermNode.getLoc());
                TIntermTyped* tempAssign = intermediate.addAssign(EOpAssign, arguments[i]->getAsTyped(), tempArgNode, arguments[i]->getLoc());
                conversionTree = intermediate.growAggregate(conversionTree, tempAssign, arguments[i]->getLoc());
                // replace the argument with another node for the same tempArg variable
                arguments[i] = intermediate.addSymbol(*tempArg, intermNode.getLoc());
            }
        }
    }

    // Finalize the tree topology (see bigger comment above).
    if (tempRet) {
        // do the "..., tempRet" bit from above
        TIntermSymbol* tempRetNode = intermediate.addSymbol(*tempRet, intermNode.getLoc());
        conversionTree = intermediate.growAggregate(conversionTree, tempRetNode, intermNode.getLoc());
    }
    conversionTree = intermediate.setAggregateOperator(conversionTree, EOpComma, intermNode.getType(), intermNode.getLoc());

    return conversionTree;
}

void TParseContext::memorySemanticsCheck(const TSourceLoc& loc, const TFunction& fnCandidate, const TIntermOperator& callNode)
{
    const TIntermSequence* argp = &callNode.getAsAggregate()->getSequence();

    const int gl_SemanticsRelaxed         = 0x0;
    const int gl_SemanticsAcquire         = 0x2;
    const int gl_SemanticsRelease         = 0x4;
    const int gl_SemanticsAcquireRelease  = 0x8;
    const int gl_SemanticsMakeAvailable   = 0x2000;
    const int gl_SemanticsMakeVisible     = 0x4000;

    const int gl_StorageSemanticsNone     = 0x0;
    const int gl_StorageSemanticsBuffer   = 0x40;
    const int gl_StorageSemanticsShared   = 0x100;
    const int gl_StorageSemanticsImage    = 0x800;
    const int gl_StorageSemanticsOutput   = 0x1000;


    unsigned int semantics = 0, storageClassSemantics = 0;
    unsigned int semantics2 = 0, storageClassSemantics2 = 0;

    // Grab the semantics and storage class semantics from the operands, based on opcode
    switch (callNode.getOp()) {
    case EOpAtomicAdd:
    case EOpAtomicMin:
    case EOpAtomicMax:
    case EOpAtomicAnd:
    case EOpAtomicOr:
    case EOpAtomicXor:
    case EOpAtomicExchange:
    case EOpAtomicStore:
        storageClassSemantics = (*argp)[3]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[4]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    case EOpAtomicLoad:
        storageClassSemantics = (*argp)[2]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[3]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    case EOpAtomicCompSwap:
        storageClassSemantics = (*argp)[4]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[5]->getAsConstantUnion()->getConstArray()[0].getIConst();
        storageClassSemantics2 = (*argp)[6]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics2 = (*argp)[7]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;

    case EOpImageAtomicAdd:
    case EOpImageAtomicMin:
    case EOpImageAtomicMax:
    case EOpImageAtomicAnd:
    case EOpImageAtomicOr:
    case EOpImageAtomicXor:
    case EOpImageAtomicExchange:
    case EOpImageAtomicStore:
        storageClassSemantics = (*argp)[4]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[5]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    case EOpImageAtomicLoad:
        storageClassSemantics = (*argp)[3]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[4]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    case EOpImageAtomicCompSwap:
        storageClassSemantics = (*argp)[5]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[6]->getAsConstantUnion()->getConstArray()[0].getIConst();
        storageClassSemantics2 = (*argp)[7]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics2 = (*argp)[8]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;

    case EOpBarrier:
        storageClassSemantics = (*argp)[2]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[3]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    case EOpMemoryBarrier:
        storageClassSemantics = (*argp)[1]->getAsConstantUnion()->getConstArray()[0].getIConst();
        semantics = (*argp)[2]->getAsConstantUnion()->getConstArray()[0].getIConst();
        break;
    default:
        break;
    }

    if ((semantics & gl_SemanticsAcquire) && 
        (callNode.getOp() == EOpAtomicStore || callNode.getOp() == EOpImageAtomicStore)) {
        error(loc, "gl_SemanticsAcquire must not be used with (image) atomic store",
              fnCandidate.getName().c_str(), "");
    }
    if ((semantics & gl_SemanticsRelease) && 
        (callNode.getOp() == EOpAtomicLoad || callNode.getOp() == EOpImageAtomicLoad)) {
        error(loc, "gl_SemanticsRelease must not be used with (image) atomic load",
              fnCandidate.getName().c_str(), "");
    }
    if ((semantics & gl_SemanticsAcquireRelease) && 
        (callNode.getOp() == EOpAtomicStore || callNode.getOp() == EOpImageAtomicStore || 
         callNode.getOp() == EOpAtomicLoad  || callNode.getOp() == EOpImageAtomicLoad)) {
        error(loc, "gl_SemanticsAcquireRelease must not be used with (image) atomic load/store",
              fnCandidate.getName().c_str(), "");
    }
    if (((semantics | semantics2) & ~(gl_SemanticsAcquire |
                                      gl_SemanticsRelease |
                                      gl_SemanticsAcquireRelease |
                                      gl_SemanticsMakeAvailable |
                                      gl_SemanticsMakeVisible))) {
        error(loc, "Invalid semantics value", fnCandidate.getName().c_str(), "");
    }
    if (((storageClassSemantics | storageClassSemantics2) & ~(gl_StorageSemanticsBuffer |
                                                              gl_StorageSemanticsShared |
                                                              gl_StorageSemanticsImage |
                                                              gl_StorageSemanticsOutput))) {
        error(loc, "Invalid storage class semantics value", fnCandidate.getName().c_str(), "");
    }

    if (callNode.getOp() == EOpMemoryBarrier) {
        if (!IsPow2(semantics & (gl_SemanticsAcquire | gl_SemanticsRelease | gl_SemanticsAcquireRelease))) {
            error(loc, "Semantics must include exactly one of gl_SemanticsRelease, gl_SemanticsAcquire, or "
                       "gl_SemanticsAcquireRelease", fnCandidate.getName().c_str(), "");
        }
    } else {
        if (semantics & (gl_SemanticsAcquire | gl_SemanticsRelease | gl_SemanticsAcquireRelease)) {
            if (!IsPow2(semantics & (gl_SemanticsAcquire | gl_SemanticsRelease | gl_SemanticsAcquireRelease))) {
                error(loc, "Semantics must not include multiple of gl_SemanticsRelease, gl_SemanticsAcquire, or "
                           "gl_SemanticsAcquireRelease", fnCandidate.getName().c_str(), "");
            }
        }
        if (semantics2 & (gl_SemanticsAcquire | gl_SemanticsRelease | gl_SemanticsAcquireRelease)) {
            if (!IsPow2(semantics2 & (gl_SemanticsAcquire | gl_SemanticsRelease | gl_SemanticsAcquireRelease))) {
                error(loc, "semUnequal must not include multiple of gl_SemanticsRelease, gl_SemanticsAcquire, or "
                           "gl_SemanticsAcquireRelease", fnCandidate.getName().c_str(), "");
            }
        }
    }
    if (callNode.getOp() == EOpMemoryBarrier) {
        if (storageClassSemantics == 0) {
            error(loc, "Storage class semantics must not be zero", fnCandidate.getName().c_str(), "");
        }
    }
    if (callNode.getOp() == EOpBarrier && semantics != 0 && storageClassSemantics == 0) {
        error(loc, "Storage class semantics must not be zero", fnCandidate.getName().c_str(), "");
    }
    if ((callNode.getOp() == EOpAtomicCompSwap || callNode.getOp() == EOpImageAtomicCompSwap) &&
        (semantics2 & (gl_SemanticsRelease | gl_SemanticsAcquireRelease))) {
        error(loc, "semUnequal must not be gl_SemanticsRelease or gl_SemanticsAcquireRelease",
              fnCandidate.getName().c_str(), "");
    }
    if ((semantics & gl_SemanticsMakeAvailable) &&
        !(semantics & (gl_SemanticsRelease | gl_SemanticsAcquireRelease))) {
        error(loc, "gl_SemanticsMakeAvailable requires gl_SemanticsRelease or gl_SemanticsAcquireRelease",
              fnCandidate.getName().c_str(), "");
    }
    if ((semantics & gl_SemanticsMakeVisible) &&
        !(semantics & (gl_SemanticsAcquire | gl_SemanticsAcquireRelease))) {
        error(loc, "gl_SemanticsMakeVisible requires gl_SemanticsAcquire or gl_SemanticsAcquireRelease",
              fnCandidate.getName().c_str(), "");
    }

}


//
// Do additional checking of built-in function calls that is not caught
// by normal semantic checks on argument type, extension tagging, etc.
//
// Assumes there has been a semantically correct match to a built-in function prototype.
//
void TParseContext::builtInOpCheck(const TSourceLoc& loc, const TFunction& fnCandidate, TIntermOperator& callNode)
{
    // Set up convenience accessors to the argument(s).  There is almost always
    // multiple arguments for the cases below, but when there might be one,
    // check the unaryArg first.
    const TIntermSequence* argp = nullptr;   // confusing to use [] syntax on a pointer, so this is to help get a reference
    const TIntermTyped* unaryArg = nullptr;
    const TIntermTyped* arg0 = nullptr;
    if (callNode.getAsAggregate()) {
        argp = &callNode.getAsAggregate()->getSequence();
        if (argp->size() > 0)
            arg0 = (*argp)[0]->getAsTyped();
    } else {
        assert(callNode.getAsUnaryNode());
        unaryArg = callNode.getAsUnaryNode()->getOperand();
        arg0 = unaryArg;
    }

    TString featureString;
    const char* feature = nullptr;
    switch (callNode.getOp()) {
    case EOpTextureGather:
    case EOpTextureGatherOffset:
    case EOpTextureGatherOffsets:
    {
        // Figure out which variants are allowed by what extensions,
        // and what arguments must be constant for which situations.

        featureString = fnCandidate.getName();
        featureString += "(...)";
        feature = featureString.c_str();
        profileRequires(loc, EEsProfile, 310, nullptr, feature);
        int compArg = -1;  // track which argument, if any, is the constant component argument
        switch (callNode.getOp()) {
        case EOpTextureGather:
            // More than two arguments needs gpu_shader5, and rectangular or shadow needs gpu_shader5,
            // otherwise, need GL_ARB_texture_gather.
            if (fnCandidate.getParamCount() > 2 || fnCandidate[0].type->getSampler().dim == EsdRect || fnCandidate[0].type->getSampler().shadow) {
                profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
                if (! fnCandidate[0].type->getSampler().shadow)
                    compArg = 2;
            } else
                profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_texture_gather, feature);
            break;
        case EOpTextureGatherOffset:
            // GL_ARB_texture_gather is good enough for 2D non-shadow textures with no component argument
            if (fnCandidate[0].type->getSampler().dim == Esd2D && ! fnCandidate[0].type->getSampler().shadow && fnCandidate.getParamCount() == 3)
                profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_texture_gather, feature);
            else
                profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
            if (! (*argp)[fnCandidate[0].type->getSampler().shadow ? 3 : 2]->getAsConstantUnion())
                profileRequires(loc, EEsProfile, 320, Num_AEP_gpu_shader5, AEP_gpu_shader5,
                                "non-constant offset argument");
            if (! fnCandidate[0].type->getSampler().shadow)
                compArg = 3;
            break;
        case EOpTextureGatherOffsets:
            profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
            if (! fnCandidate[0].type->getSampler().shadow)
                compArg = 3;
            // check for constant offsets
            if (! (*argp)[fnCandidate[0].type->getSampler().shadow ? 3 : 2]->getAsConstantUnion())
                error(loc, "must be a compile-time constant:", feature, "offsets argument");
            break;
        default:
            break;
        }

        if (compArg > 0 && compArg < fnCandidate.getParamCount()) {
            if ((*argp)[compArg]->getAsConstantUnion()) {
                int value = (*argp)[compArg]->getAsConstantUnion()->getConstArray()[0].getIConst();
                if (value < 0 || value > 3)
                    error(loc, "must be 0, 1, 2, or 3:", feature, "component argument");
            } else
                error(loc, "must be a compile-time constant:", feature, "component argument");
        }

#ifdef AMD_EXTENSIONS
        bool bias = false;
        if (callNode.getOp() == EOpTextureGather)
            bias = fnCandidate.getParamCount() > 3;
        else if (callNode.getOp() == EOpTextureGatherOffset ||
                 callNode.getOp() == EOpTextureGatherOffsets)
            bias = fnCandidate.getParamCount() > 4;

        if (bias) {
            featureString = fnCandidate.getName();
            featureString += "with bias argument";
            feature = featureString.c_str();
            profileRequires(loc, ~EEsProfile, 450, nullptr, feature);
            requireExtensions(loc, 1, &E_GL_AMD_texture_gather_bias_lod, feature);
        }
#endif

        break;
    }

#ifdef AMD_EXTENSIONS
    case EOpSparseTextureGather:
    case EOpSparseTextureGatherOffset:
    case EOpSparseTextureGatherOffsets:
    {
        bool bias = false;
        if (callNode.getOp() == EOpSparseTextureGather)
            bias = fnCandidate.getParamCount() > 4;
        else if (callNode.getOp() == EOpSparseTextureGatherOffset ||
                 callNode.getOp() == EOpSparseTextureGatherOffsets)
            bias = fnCandidate.getParamCount() > 5;

        if (bias) {
            featureString = fnCandidate.getName();
            featureString += "with bias argument";
            feature = featureString.c_str();
            profileRequires(loc, ~EEsProfile, 450, nullptr, feature);
            requireExtensions(loc, 1, &E_GL_AMD_texture_gather_bias_lod, feature);
        }

        break;
    }

    case EOpSparseTextureGatherLod:
    case EOpSparseTextureGatherLodOffset:
    case EOpSparseTextureGatherLodOffsets:
    {
        requireExtensions(loc, 1, &E_GL_ARB_sparse_texture2, fnCandidate.getName().c_str());
        break;
    }

    case EOpSwizzleInvocations:
    {
        if (! (*argp)[1]->getAsConstantUnion())
            error(loc, "argument must be compile-time constant", "offset", "");
        else {
            unsigned offset[4] = {};
            offset[0] = (*argp)[1]->getAsConstantUnion()->getConstArray()[0].getUConst();
            offset[1] = (*argp)[1]->getAsConstantUnion()->getConstArray()[1].getUConst();
            offset[2] = (*argp)[1]->getAsConstantUnion()->getConstArray()[2].getUConst();
            offset[3] = (*argp)[1]->getAsConstantUnion()->getConstArray()[3].getUConst();
            if (offset[0] > 3 || offset[1] > 3 || offset[2] > 3 || offset[3] > 3)
                error(loc, "components must be in the range [0, 3]", "offset", "");
        }

        break;
    }

    case EOpSwizzleInvocationsMasked:
    {
        if (! (*argp)[1]->getAsConstantUnion())
            error(loc, "argument must be compile-time constant", "mask", "");
        else {
            unsigned mask[3] = {};
            mask[0] = (*argp)[1]->getAsConstantUnion()->getConstArray()[0].getUConst();
            mask[1] = (*argp)[1]->getAsConstantUnion()->getConstArray()[1].getUConst();
            mask[2] = (*argp)[1]->getAsConstantUnion()->getConstArray()[2].getUConst();
            if (mask[0] > 31 || mask[1] > 31 || mask[2] > 31)
                error(loc, "components must be in the range [0, 31]", "mask", "");
        }

        break;
    }
#endif

    case EOpTextureOffset:
    case EOpTextureFetchOffset:
    case EOpTextureProjOffset:
    case EOpTextureLodOffset:
    case EOpTextureProjLodOffset:
    case EOpTextureGradOffset:
    case EOpTextureProjGradOffset:
    {
        // Handle texture-offset limits checking
        // Pick which argument has to hold constant offsets
        int arg = -1;
        switch (callNode.getOp()) {
        case EOpTextureOffset:          arg = 2;  break;
        case EOpTextureFetchOffset:     arg = (arg0->getType().getSampler().dim != EsdRect) ? 3 : 2; break;
        case EOpTextureProjOffset:      arg = 2;  break;
        case EOpTextureLodOffset:       arg = 3;  break;
        case EOpTextureProjLodOffset:   arg = 3;  break;
        case EOpTextureGradOffset:      arg = 4;  break;
        case EOpTextureProjGradOffset:  arg = 4;  break;
        default:
            assert(0);
            break;
        }

        if (arg > 0) {

#ifdef AMD_EXTENSIONS
            bool f16ShadowCompare = (*argp)[1]->getAsTyped()->getBasicType() == EbtFloat16 && arg0->getType().getSampler().shadow;
            if (f16ShadowCompare)
                ++arg;
#endif
            if (! (*argp)[arg]->getAsConstantUnion())
                error(loc, "argument must be compile-time constant", "texel offset", "");
            else {
                const TType& type = (*argp)[arg]->getAsTyped()->getType();
                for (int c = 0; c < type.getVectorSize(); ++c) {
                    int offset = (*argp)[arg]->getAsConstantUnion()->getConstArray()[c].getIConst();
                    if (offset > resources.maxProgramTexelOffset || offset < resources.minProgramTexelOffset)
                        error(loc, "value is out of range:", "texel offset", "[gl_MinProgramTexelOffset, gl_MaxProgramTexelOffset]");
                }
            }
        }

        break;
    }

    case EOpTextureQuerySamples:
    case EOpImageQuerySamples:
        // GL_ARB_shader_texture_image_samples
        profileRequires(loc, ~EEsProfile, 450, E_GL_ARB_shader_texture_image_samples, "textureSamples and imageSamples");
        break;

    case EOpImageAtomicAdd:
    case EOpImageAtomicMin:
    case EOpImageAtomicMax:
    case EOpImageAtomicAnd:
    case EOpImageAtomicOr:
    case EOpImageAtomicXor:
    case EOpImageAtomicExchange:
    case EOpImageAtomicCompSwap:
    case EOpImageAtomicLoad:
    case EOpImageAtomicStore:
    {
        // Make sure the image types have the correct layout() format and correct argument types
        const TType& imageType = arg0->getType();
        if (imageType.getSampler().type == EbtInt || imageType.getSampler().type == EbtUint) {
            if (imageType.getQualifier().layoutFormat != ElfR32i && imageType.getQualifier().layoutFormat != ElfR32ui)
                error(loc, "only supported on image with format r32i or r32ui", fnCandidate.getName().c_str(), "");
        } else {
            if (fnCandidate.getName().compare(0, 19, "imageAtomicExchange") != 0)
                error(loc, "only supported on integer images", fnCandidate.getName().c_str(), "");
            else if (imageType.getQualifier().layoutFormat != ElfR32f && profile == EEsProfile)
                error(loc, "only supported on image with format r32f", fnCandidate.getName().c_str(), "");
        }

        const size_t maxArgs = imageType.getSampler().isMultiSample() ? 5 : 4;
        if (argp->size() > maxArgs) {
            requireExtensions(loc, 1, &E_GL_KHR_memory_scope_semantics, fnCandidate.getName().c_str());
            memorySemanticsCheck(loc, fnCandidate, callNode);
        }

        break;
    }

    case EOpAtomicAdd:
    case EOpAtomicMin:
    case EOpAtomicMax:
    case EOpAtomicAnd:
    case EOpAtomicOr:
    case EOpAtomicXor:
    case EOpAtomicExchange:
    case EOpAtomicCompSwap:
    case EOpAtomicLoad:
    case EOpAtomicStore:
    {
        if (argp->size() > 3) {
            requireExtensions(loc, 1, &E_GL_KHR_memory_scope_semantics, fnCandidate.getName().c_str());
            memorySemanticsCheck(loc, fnCandidate, callNode);
        } else if (arg0->getType().getBasicType() == EbtInt64 || arg0->getType().getBasicType() == EbtUint64) {
#ifdef NV_EXTENSIONS
            const char* const extensions[2] = { E_GL_NV_shader_atomic_int64,
                                                E_GL_EXT_shader_atomic_int64 };
            requireExtensions(loc, 2, extensions, fnCandidate.getName().c_str());
#else
            requireExtensions(loc, 1, &E_GL_EXT_shader_atomic_int64, fnCandidate.getName().c_str());
#endif
        }
        break;
    }

    case EOpInterpolateAtCentroid:
    case EOpInterpolateAtSample:
    case EOpInterpolateAtOffset:
#ifdef AMD_EXTENSIONS
    case EOpInterpolateAtVertex:
#endif
        // Make sure the first argument is an interpolant, or an array element of an interpolant
        if (arg0->getType().getQualifier().storage != EvqVaryingIn) {
            // It might still be an array element.
            //
            // We could check more, but the semantics of the first argument are already met; the
            // only way to turn an array into a float/vec* is array dereference and swizzle.
            //
            // ES and desktop 4.3 and earlier:  swizzles may not be used
            // desktop 4.4 and later: swizzles may be used
            bool swizzleOkay = (profile != EEsProfile) && (version >= 440);
            const TIntermTyped* base = TIntermediate::findLValueBase(arg0, swizzleOkay);
            if (base == nullptr || base->getType().getQualifier().storage != EvqVaryingIn)
                error(loc, "first argument must be an interpolant, or interpolant-array element", fnCandidate.getName().c_str(), "");
        }

#ifdef AMD_EXTENSIONS
        if (callNode.getOp() == EOpInterpolateAtVertex) {
            if (!arg0->getType().getQualifier().isExplicitInterpolation())
                error(loc, "argument must be qualified as __explicitInterpAMD in", "interpolant", "");
            else {
                if (! (*argp)[1]->getAsConstantUnion())
                    error(loc, "argument must be compile-time constant", "vertex index", "");
                else {
                    unsigned vertexIdx = (*argp)[1]->getAsConstantUnion()->getConstArray()[0].getUConst();
                    if (vertexIdx > 2)
                        error(loc, "must be in the range [0, 2]", "vertex index", "");
                }
            }
        }
#endif

        break;

    case EOpEmitStreamVertex:
    case EOpEndStreamPrimitive:
        intermediate.setMultiStream();
        break;

    case EOpSubgroupClusteredAdd:
    case EOpSubgroupClusteredMul:
    case EOpSubgroupClusteredMin:
    case EOpSubgroupClusteredMax:
    case EOpSubgroupClusteredAnd:
    case EOpSubgroupClusteredOr:
    case EOpSubgroupClusteredXor:
        if ((*argp)[1]->getAsConstantUnion() == nullptr)
            error(loc, "argument must be compile-time constant", "cluster size", "");
        else {
            int size = (*argp)[1]->getAsConstantUnion()->getConstArray()[0].getIConst();
            if (size < 1)
                error(loc, "argument must be at least 1", "cluster size", "");
            else if (!IsPow2(size))
                error(loc, "argument must be a power of 2", "cluster size", "");
        }
        break;

    case EOpBarrier:
    case EOpMemoryBarrier:
        if (argp->size() > 0) {
            requireExtensions(loc, 1, &E_GL_KHR_memory_scope_semantics, fnCandidate.getName().c_str());
            memorySemanticsCheck(loc, fnCandidate, callNode);
        }
        break;

    default:
        break;
    }

    // Texture operations on texture objects (aside from texelFetch on a
    // textureBuffer) require EXT_samplerless_texture_functions.
    switch (callNode.getOp()) {
    case EOpTextureQuerySize:
    case EOpTextureQueryLevels:
    case EOpTextureQuerySamples:
    case EOpTextureFetch:
    case EOpTextureFetchOffset:
    {
        const TSampler& sampler = fnCandidate[0].type->getSampler();

        const bool isTexture = sampler.isTexture() && !sampler.isCombined();
        const bool isBuffer = sampler.dim == EsdBuffer;
        const bool isFetch = callNode.getOp() == EOpTextureFetch || callNode.getOp() == EOpTextureFetchOffset;

        if (isTexture && (!isBuffer || !isFetch))
            requireExtensions(loc, 1, &E_GL_EXT_samplerless_texture_functions, fnCandidate.getName().c_str());

        break;
    }

    default:
        break;
    }

    if (callNode.getOp() > EOpSubgroupGuardStart && callNode.getOp() < EOpSubgroupGuardStop) {
        // these require SPIR-V 1.3
        if (spvVersion.spv > 0 && spvVersion.spv < EShTargetSpv_1_3)
            error(loc, "requires SPIR-V 1.3", "subgroup op", "");
    }
}

extern bool PureOperatorBuiltins;

// Deprecated!  Use PureOperatorBuiltins == true instead, in which case this
// functionality is handled in builtInOpCheck() instead of here.
//
// Do additional checking of built-in function calls that were not mapped
// to built-in operations (e.g., texturing functions).
//
// Assumes there has been a semantically correct match to a built-in function.
//
void TParseContext::nonOpBuiltInCheck(const TSourceLoc& loc, const TFunction& fnCandidate, TIntermAggregate& callNode)
{
    // Further maintenance of this function is deprecated, because the "correct"
    // future-oriented design is to not have to do string compares on function names.

    // If PureOperatorBuiltins == true, then all built-ins should be mapped
    // to a TOperator, and this function would then never get called.

    assert(PureOperatorBuiltins == false);

    // built-in texturing functions get their return value precision from the precision of the sampler
    if (fnCandidate.getType().getQualifier().precision == EpqNone &&
        fnCandidate.getParamCount() > 0 && fnCandidate[0].type->getBasicType() == EbtSampler)
        callNode.getQualifier().precision = callNode.getSequence()[0]->getAsTyped()->getQualifier().precision;

    if (fnCandidate.getName().compare(0, 7, "texture") == 0) {
        if (fnCandidate.getName().compare(0, 13, "textureGather") == 0) {
            TString featureString = fnCandidate.getName() + "(...)";
            const char* feature = featureString.c_str();
            profileRequires(loc, EEsProfile, 310, nullptr, feature);

            int compArg = -1;  // track which argument, if any, is the constant component argument
            if (fnCandidate.getName().compare("textureGatherOffset") == 0) {
                // GL_ARB_texture_gather is good enough for 2D non-shadow textures with no component argument
                if (fnCandidate[0].type->getSampler().dim == Esd2D && ! fnCandidate[0].type->getSampler().shadow && fnCandidate.getParamCount() == 3)
                    profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_texture_gather, feature);
                else
                    profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
                int offsetArg = fnCandidate[0].type->getSampler().shadow ? 3 : 2;
                if (! callNode.getSequence()[offsetArg]->getAsConstantUnion())
                    profileRequires(loc, EEsProfile, 320, Num_AEP_gpu_shader5, AEP_gpu_shader5,
                                    "non-constant offset argument");
                if (! fnCandidate[0].type->getSampler().shadow)
                    compArg = 3;
            } else if (fnCandidate.getName().compare("textureGatherOffsets") == 0) {
                profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
                if (! fnCandidate[0].type->getSampler().shadow)
                    compArg = 3;
                // check for constant offsets
                int offsetArg = fnCandidate[0].type->getSampler().shadow ? 3 : 2;
                if (! callNode.getSequence()[offsetArg]->getAsConstantUnion())
                    error(loc, "must be a compile-time constant:", feature, "offsets argument");
            } else if (fnCandidate.getName().compare("textureGather") == 0) {
                // More than two arguments needs gpu_shader5, and rectangular or shadow needs gpu_shader5,
                // otherwise, need GL_ARB_texture_gather.
                if (fnCandidate.getParamCount() > 2 || fnCandidate[0].type->getSampler().dim == EsdRect || fnCandidate[0].type->getSampler().shadow) {
                    profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_gpu_shader5, feature);
                    if (! fnCandidate[0].type->getSampler().shadow)
                        compArg = 2;
                } else
                    profileRequires(loc, ~EEsProfile, 400, E_GL_ARB_texture_gather, feature);
            }

            if (compArg > 0 && compArg < fnCandidate.getParamCount()) {
                if (callNode.getSequence()[compArg]->getAsConstantUnion()) {
                    int value = callNode.getSequence()[compArg]->getAsConstantUnion()->getConstArray()[0].getIConst();
                    if (value < 0 || value > 3)
                        error(loc, "must be 0, 1, 2, or 3:", feature, "component argument");
                } else
                    error(loc, "must be a compile-time constant:", feature, "component argument");
            }
        } else {
            // this is only for functions not starting "textureGather"...
            if (fnCandidate.getName().find("Offset") != TString::npos) {

                // Handle texture-offset limits checking
                int arg = -1;
                if (fnCandidate.getName().compare("textureOffset") == 0)
                    arg = 2;
                else if (fnCandidate.getName().compare("texelFetchOffset") == 0)
                    arg = 3;
                else if (fnCandidate.getName().compare("textureProjOffset") == 0)
                    arg = 2;
                else if (fnCandidate.getName().compare("textureLodOffset") == 0)
                    arg = 3;
                else if (fnCandidate.getName().compare("textureProjLodOffset") == 0)
                    arg = 3;
                else if (fnCandidate.getName().compare("textureGradOffset") == 0)
                    arg = 4;
                else if (fnCandidate.getName().compare("textureProjGradOffset") == 0)
                    arg = 4;

                if (arg > 0) {
                    if (! callNode.getSequence()[arg]->getAsConstantUnion())
                        error(loc, "argument must be compile-time constant", "texel offset", "");
                    else {
                        const TType& type = callNode.getSequence()[arg]->getAsTyped()->getType();
                        for (int c = 0; c < type.getVectorSize(); ++c) {
                            int offset = callNode.getSequence()[arg]->getAsConstantUnion()->getConstArray()[c].getIConst();
                            if (offset > resources.maxProgramTexelOffset || offset < resources.minProgramTexelOffset)
                                error(loc, "value is out of range:", "texel offset", "[gl_MinProgramTexelOffset, gl_MaxProgramTexelOffset]");
                        }
                    }
                }
            }
        }
    }

    // GL_ARB_shader_texture_image_samples
    if (fnCandidate.getName().compare(0, 14, "textureSamples") == 0 || fnCandidate.getName().compare(0, 12, "imageSamples") == 0)
        profileRequires(loc, ~EEsProfile, 450, E_GL_ARB_shader_texture_image_samples, "textureSamples and imageSamples");

    if (fnCandidate.getName().compare(0, 11, "imageAtomic") == 0) {
        const TType& imageType = callNode.getSequence()[0]->getAsTyped()->getType();
        if (imageType.getSampler().type == EbtInt || imageType.getSampler().type == EbtUint) {
            if (imageType.getQualifier().layoutFormat != ElfR32i && imageType.getQualifier().layoutFormat != ElfR32ui)
                error(loc, "only supported on image with format r32i or r32ui", fnCandidate.getName().c_str(), "");
        } else {
            if (fnCandidate.getName().compare(0, 19, "imageAtomicExchange") != 0)
                error(loc, "only supported on integer images", fnCandidate.getName().c_str(), "");
            else if (imageType.getQualifier().layoutFormat != ElfR32f && profile == EEsProfile)
                error(loc, "only supported on image with format r32f", fnCandidate.getName().c_str(), "");
        }
    }
}

//
// Do any extra checking for a user function call.
//
void TParseContext::userFunctionCallCheck(const TSourceLoc& loc, TIntermAggregate& callNode)
{
    TIntermSequence& arguments = callNode.getSequence();

    for (int i = 0; i < (int)arguments.size(); ++i)
        samplerConstructorLocationCheck(loc, "call argument", arguments[i]);
}

//
// Emit an error if this is a sampler constructor
//
void TParseContext::samplerConstructorLocationCheck(const TSourceLoc& loc, const char* token, TIntermNode* node)
{
    if (node->getAsOperator() && node->getAsOperator()->getOp() == EOpConstructTextureSampler)
        error(loc, "sampler constructor must appear at point of use", token, "");
}

//
// Handle seeing a built-in constructor in a grammar production.
//
TFunction* TParseContext::handleConstructorCall(const TSourceLoc& loc, const TPublicType& publicType)
{
    TType type(publicType);
    type.getQualifier().precision = EpqNone;

    if (type.isArray()) {
        profileRequires(loc, ENoProfile, 120, E_GL_3DL_array_objects, "arrayed constructor");
        profileRequires(loc, EEsProfile, 300, nullptr, "arrayed constructor");
    }

    TOperator op = intermediate.mapTypeToConstructorOp(type);

    if (op == EOpNull) {
        error(loc, "cannot construct this type", type.getBasicString(), "");
        op = EOpConstructFloat;
        TType errorType(EbtFloat);
        type.shallowCopy(errorType);
    }

    TString empty("");

    return new TFunction(&empty, type, op);
}

// Handle seeing a precision qualifier in the grammar.
void TParseContext::handlePrecisionQualifier(const TSourceLoc& /*loc*/, TQualifier& qualifier, TPrecisionQualifier precision)
{
    if (obeyPrecisionQualifiers())
        qualifier.precision = precision;
}

// Check for messages to give on seeing a precision qualifier used in a
// declaration in the grammar.
void TParseContext::checkPrecisionQualifier(const TSourceLoc& loc, TPrecisionQualifier)
{
    if (precisionManager.shouldWarnAboutDefaults()) {
        warn(loc, "all default precisions are highp; use precision statements to quiet warning, e.g.:\n"
                  "         \"precision mediump int; precision highp float;\"", "", "");
        precisionManager.defaultWarningGiven();
    }
}

//
// Same error message for all places assignments don't work.
//
void TParseContext::assignError(const TSourceLoc& loc, const char* op, TString left, TString right)
{
    error(loc, "", op, "cannot convert from '%s' to '%s'",
          right.c_str(), left.c_str());
}

//
// Same error message for all places unary operations don't work.
//
void TParseContext::unaryOpError(const TSourceLoc& loc, const char* op, TString operand)
{
   error(loc, " wrong operand type", op,
          "no operation '%s' exists that takes an operand of type %s (or there is no acceptable conversion)",
          op, operand.c_str());
}

//
// Same error message for all binary operations don't work.
//
void TParseContext::binaryOpError(const TSourceLoc& loc, const char* op, TString left, TString right)
{
    error(loc, " wrong operand types:", op,
            "no operation '%s' exists that takes a left-hand operand of type '%s' and "
            "a right operand of type '%s' (or there is no acceptable conversion)",
            op, left.c_str(), right.c_str());
}

//
// A basic type of EbtVoid is a key that the name string was seen in the source, but
// it was not found as a variable in the symbol table.  If so, give the error
// message and insert a dummy variable in the symbol table to prevent future errors.
//
void TParseContext::variableCheck(TIntermTyped*& nodePtr)
{
    TIntermSymbol* symbol = nodePtr->getAsSymbolNode();
    if (! symbol)
        return;

    if (symbol->getType().getBasicType() == EbtVoid) {
        const char *extraInfoFormat = "";
        if (spvVersion.vulkan != 0 && symbol->getName() == "gl_VertexID") {
          extraInfoFormat = "(Did you mean gl_VertexIndex?)";
        } else if (spvVersion.vulkan != 0 && symbol->getName() == "gl_InstanceID") {
          extraInfoFormat = "(Did you mean gl_InstanceIndex?)";
        }
        error(symbol->getLoc(), "undeclared identifier", symbol->getName().c_str(), extraInfoFormat);

        // Add to symbol table to prevent future error messages on the same name
        if (symbol->getName().size() > 0) {
            TVariable* fakeVariable = new TVariable(&symbol->getName(), TType(EbtFloat));
            symbolTable.insert(*fakeVariable);

            // substitute a symbol node for this new variable
            nodePtr = intermediate.addSymbol(*fakeVariable, symbol->getLoc());
        }
    } else {
        switch (symbol->getQualifier().storage) {
        case EvqPointCoord:
            profileRequires(symbol->getLoc(), ENoProfile, 120, nullptr, "gl_PointCoord");
            break;
        default: break; // some compilers want this
        }
    }
}

//
// Both test and if necessary, spit out an error, to see if the node is really
// an l-value that can be operated on this way.
//
// Returns true if there was an error.
//
bool TParseContext::lValueErrorCheck(const TSourceLoc& loc, const char* op, TIntermTyped* node)
{
    TIntermBinary* binaryNode = node->getAsBinaryNode();

    if (binaryNode) {
        bool errorReturn = false;

        switch(binaryNode->getOp()) {
        case EOpIndexDirect:
        case EOpIndexIndirect:
            // ...  tessellation control shader ...
            // If a per-vertex output variable is used as an l-value, it is a
            // compile-time or link-time error if the expression indicating the
            // vertex index is not the identifier gl_InvocationID.
            if (language == EShLangTessControl) {
                const TType& leftType = binaryNode->getLeft()->getType();
                if (leftType.getQualifier().storage == EvqVaryingOut && ! leftType.getQualifier().patch && binaryNode->getLeft()->getAsSymbolNode()) {
                    // we have a per-vertex output
                    const TIntermSymbol* rightSymbol = binaryNode->getRight()->getAsSymbolNode();
                    if (! rightSymbol || rightSymbol->getQualifier().builtIn != EbvInvocationId)
                        error(loc, "tessellation-control per-vertex output l-value must be indexed with gl_InvocationID", "[]", "");
                }
            }

            break; // left node is checked by base class
        case EOpIndexDirectStruct:
            break; // left node is checked by base class
        case EOpVectorSwizzle:
            errorReturn = lValueErrorCheck(loc, op, binaryNode->getLeft());
            if (!errorReturn) {
                int offset[4] = {0,0,0,0};

                TIntermTyped* rightNode = binaryNode->getRight();
                TIntermAggregate *aggrNode = rightNode->getAsAggregate();

                for (TIntermSequence::iterator p = aggrNode->getSequence().begin();
                                               p != aggrNode->getSequence().end(); p++) {
                    int value = (*p)->getAsTyped()->getAsConstantUnion()->getConstArray()[0].getIConst();
                    offset[value]++;
                    if (offset[value] > 1) {
                        error(loc, " l-value of swizzle cannot have duplicate components", op, "", "");

                        return true;
                    }
                }
            }

            return errorReturn;
        default:
            break;
        }

        if (errorReturn) {
            error(loc, " l-value required", op, "", "");
            return true;
        }
    }

    // Let the base class check errors
    if (TParseContextBase::lValueErrorCheck(loc, op, node))
        return true;

    const char* symbol = nullptr;
    TIntermSymbol* symNode = node->getAsSymbolNode();
    if (symNode != nullptr)
        symbol = symNode->getName().c_str();

    const char* message = nullptr;
    switch (node->getQualifier().storage) {
    case EvqVaryingIn:      message = "can't modify shader input";   break;
    case EvqInstanceId:     message = "can't modify gl_InstanceID";  break;
    case EvqVertexId:       message = "can't modify gl_VertexID";    break;
    case EvqFace:           message = "can't modify gl_FrontFace";   break;
    case EvqFragCoord:      message = "can't modify gl_FragCoord";   break;
    case EvqPointCoord:     message = "can't modify gl_PointCoord";  break;
    case EvqFragDepth:
        intermediate.setDepthReplacing();
        // "In addition, it is an error to statically write to gl_FragDepth in the fragment shader."
        if (profile == EEsProfile && intermediate.getEarlyFragmentTests())
            message = "can't modify gl_FragDepth if using early_fragment_tests";
        break;

    default:
        break;
    }

    if (message == nullptr && binaryNode == nullptr && symNode == nullptr) {
        error(loc, " l-value required", op, "", "");

        return true;
    }

    //
    // Everything else is okay, no error.
    //
    if (message == nullptr)
        return false;

    //
    // If we get here, we have an error and a message.
    //
    if (symNode)
        error(loc, " l-value required", op, "\"%s\" (%s)", symbol, message);
    else
        error(loc, " l-value required", op, "(%s)", message);

    return true;
}

// Test for and give an error if the node can't be read from.
void TParseContext::rValueErrorCheck(const TSourceLoc& loc, const char* op, TIntermTyped* node)
{
    // Let the base class check errors
    TParseContextBase::rValueErrorCheck(loc, op, node);

#ifdef AMD_EXTENSIONS
    TIntermSymbol* symNode = node->getAsSymbolNode();
    if (!(symNode && symNode->getQualifier().writeonly)) // base class checks
        if (symNode && symNode->getQualifier().explicitInterp)
            error(loc, "can't read from explicitly-interpolated object: ", op, symNode->getName().c_str());
#endif
}

//
// Both test, and if necessary spit out an error, to see if the node is really
// a constant.
//
void TParseContext::constantValueCheck(TIntermTyped* node, const char* token)
{
    if (! node->getQualifier().isConstant())
        error(node->getLoc(), "constant expression required", token, "");
}

//
// Both test, and if necessary spit out an error, to see if the node is really
// an integer.
//
void TParseContext::integerCheck(const TIntermTyped* node, const char* token)
{
    if ((node->getBasicType() == EbtInt || node->getBasicType() == EbtUint) && node->isScalar())
        return;

    error(node->getLoc(), "scalar integer expression required", token, "");
}

//
// Both test, and if necessary spit out an error, to see if we are currently
// globally scoped.
//
void TParseContext::globalCheck(const TSourceLoc& loc, const char* token)
{
    if (! symbolTable.atGlobalLevel())
        error(loc, "not allowed in nested scope", token, "");
}

//
// Reserved errors for GLSL.
//
void TParseContext::reservedErrorCheck(const TSourceLoc& loc, const TString& identifier)
{
    // "Identifiers starting with "gl_" are reserved for use by OpenGL, and may not be
    // declared in a shader; this results in a compile-time error."
    if (! symbolTable.atBuiltInLevel()) {
        if (builtInName(identifier))
            error(loc, "identifiers starting with \"gl_\" are reserved", identifier.c_str(), "");

        // "__" are not supposed to be an error.  ES 310 (and desktop) added the clarification:
        // "In addition, all identifiers containing two consecutive underscores (__) are
        // reserved; using such a name does not itself result in an error, but may result
        // in undefined behavior."
        // however, before that, ES tests required an error.
        if (identifier.find("__") != TString::npos) {
            if (profile == EEsProfile && version <= 300)
                error(loc, "identifiers containing consecutive underscores (\"__\") are reserved, and an error if version <= 300", identifier.c_str(), "");
            else
                warn(loc, "identifiers containing consecutive underscores (\"__\") are reserved", identifier.c_str(), "");
        }
    }
}

//
// Reserved errors for the preprocessor.
//
void TParseContext::reservedPpErrorCheck(const TSourceLoc& loc, const char* identifier, const char* op)
{
    // "__" are not supposed to be an error.  ES 310 (and desktop) added the clarification:
    // "All macro names containing two consecutive underscores ( __ ) are reserved;
    // defining such a name does not itself result in an error, but may result in
    // undefined behavior.  All macro names prefixed with "GL_" ("GL" followed by a
    // single underscore) are also reserved, and defining such a name results in a
    // compile-time error."
    // however, before that, ES tests required an error.
    if (strncmp(identifier, "GL_", 3) == 0)
        ppError(loc, "names beginning with \"GL_\" can't be (un)defined:", op,  identifier);
    else if (strncmp(identifier, "defined", 8) == 0)
        ppError(loc, "\"defined\" can't be (un)defined:", op,  identifier);
    else if (strstr(identifier, "__") != 0) {
        if (profile == EEsProfile && version >= 300 &&
            (strcmp(identifier, "__LINE__") == 0 ||
             strcmp(identifier, "__FILE__") == 0 ||
             strcmp(identifier, "__VERSION__") == 0))
            ppError(loc, "predefined names can't be (un)defined:", op,  identifier);
        else {
            if (profile == EEsProfile && version <= 300)
                ppError(loc, "names containing consecutive underscores are reserved, and an error if version <= 300:", op, identifier);
            else
                ppWarn(loc, "names containing consecutive underscores are reserved:", op, identifier);
        }
    }
}

//
// See if this version/profile allows use of the line-continuation character '\'.
//
// Returns true if a line continuation should be done.
//
bool TParseContext::lineContinuationCheck(const TSourceLoc& loc, bool endOfComment)
{
    const char* message = "line continuation";

    bool lineContinuationAllowed = (profile == EEsProfile && version >= 300) ||
                                   (profile != EEsProfile && (version >= 420 || extensionTurnedOn(E_GL_ARB_shading_language_420pack)));

    if (endOfComment) {
        if (lineContinuationAllowed)
            warn(loc, "used at end of comment; the following line is still part of the comment", message, "");
        else
            warn(loc, "used at end of comment, but this version does not provide line continuation", message, "");

        return lineContinuationAllowed;
    }

    if (relaxedErrors()) {
        if (! lineContinuationAllowed)
            warn(loc, "not allowed in this version", message, "");
        return true;
    } else {
        profileRequires(loc, EEsProfile, 300, nullptr, message);
        profileRequires(loc, ~EEsProfile, 420, E_GL_ARB_shading_language_420pack, message);
    }

    return lineContinuationAllowed;
}

bool TParseContext::builtInName(const TString& identifier)
{
    return identifier.compare(0, 3, "gl_") == 0;
}

//
// Make sure there is enough data and not too many arguments provided to the
// constructor to build something of the type of the constructor.  Also returns
// the type of the constructor.
//
// Part of establishing type is establishing specialization-constness.
// We don't yet know "top down" whether type is a specialization constant,
// but a const constructor can becomes a specialization constant if any of
// its children are, subject to KHR_vulkan_glsl rules:
//
//     - int(), uint(), and bool() constructors for type conversions
//       from any of the following types to any of the following types:
//         * int
//         * uint
//         * bool
//     - vector versions of the above conversion constructors
//
// Returns true if there was an error in construction.
//
bool TParseContext::constructorError(const TSourceLoc& loc, TIntermNode* node, TFunction& function, TOperator op, TType& type)
{
    type.shallowCopy(function.getType());

    bool constructingMatrix = false;
    switch(op) {
    case EOpConstructTextureSampler:
        return constructorTextureSamplerError(loc, function);
    case EOpConstructMat2x2:
    case EOpConstructMat2x3:
    case EOpConstructMat2x4:
    case EOpConstructMat3x2:
    case EOpConstructMat3x3:
    case EOpConstructMat3x4:
    case EOpConstructMat4x2:
    case EOpConstructMat4x3:
    case EOpConstructMat4x4:
    case EOpConstructDMat2x2:
    case EOpConstructDMat2x3:
    case EOpConstructDMat2x4:
    case EOpConstructDMat3x2:
    case EOpConstructDMat3x3:
    case EOpConstructDMat3x4:
    case EOpConstructDMat4x2:
    case EOpConstructDMat4x3:
    case EOpConstructDMat4x4:
    case EOpConstructF16Mat2x2:
    case EOpConstructF16Mat2x3:
    case EOpConstructF16Mat2x4:
    case EOpConstructF16Mat3x2:
    case EOpConstructF16Mat3x3:
    case EOpConstructF16Mat3x4:
    case EOpConstructF16Mat4x2:
    case EOpConstructF16Mat4x3:
    case EOpConstructF16Mat4x4:
        constructingMatrix = true;
        break;
    default:
        break;
    }

    //
    // Walk the arguments for first-pass checks and collection of information.
    //

    int size = 0;
    bool constType = true;
    bool specConstType = false;   // value is only valid if constType is true
    bool full = false;
    bool overFull = false;
    bool matrixInMatrix = false;
    bool arrayArg = false;
    bool floatArgument = false;
    for (int arg = 0; arg < function.getParamCount(); ++arg) {
        if (function[arg].type->isArray()) {
            if (function[arg].type->isUnsizedArray()) {
                // Can't construct from an unsized array.
                error(loc, "array argument must be sized", "constructor", "");
                return true;
            }
            arrayArg = true;
        }
        if (constructingMatrix && function[arg].type->isMatrix())
            matrixInMatrix = true;

        // 'full' will go to true when enough args have been seen.  If we loop
        // again, there is an extra argument.
        if (full) {
            // For vectors and matrices, it's okay to have too many components
            // available, but not okay to have unused arguments.
            overFull = true;
        }

        size += function[arg].type->computeNumComponents();
        if (op != EOpConstructStruct && ! type.isArray() && size >= type.computeNumComponents())
            full = true;

        if (! function[arg].type->getQualifier().isConstant())
            constType = false;
        if (function[arg].type->getQualifier().isSpecConstant())
            specConstType = true;
        if (function[arg].type->isFloatingDomain())
            floatArgument = true;
        if (type.isStruct()) {
            if (function[arg].type->containsBasicType(EbtFloat16)) {
                requireFloat16Arithmetic(loc, "constructor", "can't construct structure containing 16-bit type");
            }
            if (function[arg].type->containsBasicType(EbtUint16) ||
                function[arg].type->containsBasicType(EbtInt16)) {
                requireInt16Arithmetic(loc, "constructor", "can't construct structure containing 16-bit type");
            }
            if (function[arg].type->containsBasicType(EbtUint8) ||
                function[arg].type->containsBasicType(EbtInt8)) {
                requireInt8Arithmetic(loc, "constructor", "can't construct structure containing 8-bit type");
            }
        }
    }

    switch (op) {
    case EOpConstructFloat16:
    case EOpConstructF16Vec2:
    case EOpConstructF16Vec3:
    case EOpConstructF16Vec4:
        if (type.isArray())
            requireFloat16Arithmetic(loc, "constructor", "16-bit arrays not supported");
        if (type.isVector() && function.getParamCount() != 1)
            requireFloat16Arithmetic(loc, "constructor", "16-bit vectors only take vector types");
        break;
    case EOpConstructUint16:
    case EOpConstructU16Vec2:
    case EOpConstructU16Vec3:
    case EOpConstructU16Vec4:
    case EOpConstructInt16:
    case EOpConstructI16Vec2:
    case EOpConstructI16Vec3:
    case EOpConstructI16Vec4:
        if (type.isArray())
            requireInt16Arithmetic(loc, "constructor", "16-bit arrays not supported");
        if (type.isVector() && function.getParamCount() != 1)
            requireInt16Arithmetic(loc, "constructor", "16-bit vectors only take vector types");
        break;
    case EOpConstructUint8:
    case EOpConstructU8Vec2:
    case EOpConstructU8Vec3:
    case EOpConstructU8Vec4:
    case EOpConstructInt8:
    case EOpConstructI8Vec2:
    case EOpConstructI8Vec3:
    case EOpConstructI8Vec4:
        if (type.isArray())
            requireInt8Arithmetic(loc, "constructor", "8-bit arrays not supported");
        if (type.isVector() && function.getParamCount() != 1)
            requireInt8Arithmetic(loc, "constructor", "8-bit vectors only take vector types");
        break;
    default:
        break;
    }

    // inherit constness from children
    if (constType) {
        bool makeSpecConst;
        // Finish pinning down spec-const semantics
        if (specConstType) {
            switch (op) {
            case EOpConstructInt8:
            case EOpConstructUint8:
            case EOpConstructInt16:
            case EOpConstructUint16:
            case EOpConstructInt:
            case EOpConstructUint:
            case EOpConstructInt64:
            case EOpConstructUint64:
            case EOpConstructBool:
            case EOpConstructBVec2:
            case EOpConstructBVec3:
            case EOpConstructBVec4:
            case EOpConstructI8Vec2:
            case EOpConstructI8Vec3:
            case EOpConstructI8Vec4:
            case EOpConstructU8Vec2:
            case EOpConstructU8Vec3:
            case EOpConstructU8Vec4:
            case EOpConstructI16Vec2:
            case EOpConstructI16Vec3:
            case EOpConstructI16Vec4:
            case EOpConstructU16Vec2:
            case EOpConstructU16Vec3:
            case EOpConstructU16Vec4:
            case EOpConstructIVec2:
            case EOpConstructIVec3:
            case EOpConstructIVec4:
            case EOpConstructUVec2:
            case EOpConstructUVec3:
            case EOpConstructUVec4:
            case EOpConstructI64Vec2:
            case EOpConstructI64Vec3:
            case EOpConstructI64Vec4:
            case EOpConstructU64Vec2:
            case EOpConstructU64Vec3:
            case EOpConstructU64Vec4:
                // This was the list of valid ones, if they aren't converting from float
                // and aren't making an array.
                makeSpecConst = ! floatArgument && ! type.isArray();
                break;
            default:
                // anything else wasn't white-listed in the spec as a conversion
                makeSpecConst = false;
                break;
            }
        } else
            makeSpecConst = false;

        if (makeSpecConst)
            type.getQualifier().makeSpecConstant();
        else if (specConstType)
            type.getQualifier().makeTemporary();
        else
            type.getQualifier().storage = EvqConst;
    }

    if (type.isArray()) {
        if (function.getParamCount() == 0) {
            error(loc, "array constructor must have at least one argument", "constructor", "");
            return true;
        }

        if (type.isUnsizedArray()) {
            // auto adapt the constructor type to the number of arguments
            type.changeOuterArraySize(function.getParamCount());
        } else if (type.getOuterArraySize() != function.getParamCount()) {
            error(loc, "array constructor needs one argument per array element", "constructor", "");
            return true;
        }

        if (type.isArrayOfArrays()) {
            // Types have to match, but we're still making the type.
            // Finish making the type, and the comparison is done later
            // when checking for conversion.
            TArraySizes& arraySizes = *type.getArraySizes();

            // At least the dimensionalities have to match.
            if (! function[0].type->isArray() ||
                    arraySizes.getNumDims() != function[0].type->getArraySizes()->getNumDims() + 1) {
                error(loc, "array constructor argument not correct type to construct array element", "constructor", "");
                return true;
            }

            if (arraySizes.isInnerUnsized()) {
                // "Arrays of arrays ..., and the size for any dimension is optional"
                // That means we need to adopt (from the first argument) the other array sizes into the type.
                for (int d = 1; d < arraySizes.getNumDims(); ++d) {
                    if (arraySizes.getDimSize(d) == UnsizedArraySize) {
                        arraySizes.setDimSize(d, function[0].type->getArraySizes()->getDimSize(d - 1));
                    }
                }
            }
        }
    }

    if (arrayArg && op != EOpConstructStruct && ! type.isArrayOfArrays()) {
        error(loc, "constructing non-array constituent from array argument", "constructor", "");
        return true;
    }

    if (matrixInMatrix && ! type.isArray()) {
        profileRequires(loc, ENoProfile, 120, nullptr, "constructing matrix from matrix");

        // "If a matrix argument is given to a matrix constructor,
        // it is a compile-time error to have any other arguments."
        if (function.getParamCount() != 1)
            error(loc, "matrix constructed from matrix can only have one argument", "constructor", "");
        return false;
    }

    if (overFull) {
        error(loc, "too many arguments", "constructor", "");
        return true;
    }

    if (op == EOpConstructStruct && ! type.isArray() && (int)type.getStruct()->size() != function.getParamCount()) {
        error(loc, "Number of constructor parameters does not match the number of structure fields", "constructor", "");
        return true;
    }

    if ((op != EOpConstructStruct && size != 1 && size < type.computeNumComponents()) ||
        (op == EOpConstructStruct && size < type.computeNumComponents())) {
        error(loc, "not enough data provided for construction", "constructor", "");
        return true;
    }

    TIntermTyped* typed = node->getAsTyped();
    if (typed == nullptr) {
        error(loc, "constructor argument does not have a type", "constructor", "");
        return true;
    }
    if (op != EOpConstructStruct && typed->getBasicType() == EbtSampler) {
        error(loc, "cannot convert a sampler", "constructor", "");
        return true;
    }
    if (op != EOpConstructStruct && typed->getBasicType() == EbtAtomicUint) {
        error(loc, "cannot convert an atomic_uint", "constructor", "");
        return true;
    }
    if (typed->getBasicType() == EbtVoid) {
        error(loc, "cannot convert a void", "constructor", "");
        return true;
    }

    return false;
}

// Verify all the correct semantics for constructing a combined texture/sampler.
// Return true if the semantics are incorrect.
bool TParseContext::constructorTextureSamplerError(const TSourceLoc& loc, const TFunction& function)
{
    TString constructorName = function.getType().getBasicTypeString();  // TODO: performance: should not be making copy; interface needs to change
    const char* token = constructorName.c_str();

    // exactly two arguments needed
    if (function.getParamCount() != 2) {
        error(loc, "sampler-constructor requires two arguments", token, "");
        return true;
    }

    // For now, not allowing arrayed constructors, the rest of this function
    // is set up to allow them, if this test is removed:
    if (function.getType().isArray()) {
        error(loc, "sampler-constructor cannot make an array of samplers", token, "");
        return true;
    }

    // first argument
    //  * the constructor's first argument must be a texture type
    //  * the dimensionality (1D, 2D, 3D, Cube, Rect, Buffer, MS, and Array)
    //    of the texture type must match that of the constructed sampler type
    //    (that is, the suffixes of the type of the first argument and the
    //    type of the constructor will be spelled the same way)
    if (function[0].type->getBasicType() != EbtSampler ||
        ! function[0].type->getSampler().isTexture() ||
        function[0].type->isArray()) {
        error(loc, "sampler-constructor first argument must be a scalar textureXXX type", token, "");
        return true;
    }
    // simulate the first argument's impact on the result type, so it can be compared with the encapsulated operator!=()
    TSampler texture = function.getType().getSampler();
    texture.combined = false;
    texture.shadow = false;
    if (texture != function[0].type->getSampler()) {
        error(loc, "sampler-constructor first argument must match type and dimensionality of constructor type", token, "");
        return true;
    }

    // second argument
    //   * the constructor's second argument must be a scalar of type
    //     *sampler* or *samplerShadow*
    if (  function[1].type->getBasicType() != EbtSampler ||
        ! function[1].type->getSampler().isPureSampler() ||
          function[1].type->isArray()) {
        error(loc, "sampler-constructor second argument must be a scalar type 'sampler'", token, "");
        return true;
    }

    return false;
}

// Checks to see if a void variable has been declared and raise an error message for such a case
//
// returns true in case of an error
//
bool TParseContext::voidErrorCheck(const TSourceLoc& loc, const TString& identifier, const TBasicType basicType)
{
    if (basicType == EbtVoid) {
        error(loc, "illegal use of type 'void'", identifier.c_str(), "");
        return true;
    }

    return false;
}

// Checks to see if the node (for the expression) contains a scalar boolean expression or not
void TParseContext::boolCheck(const TSourceLoc& loc, const TIntermTyped* type)
{
    if (type->getBasicType() != EbtBool || type->isArray() || type->isMatrix() || type->isVector())
        error(loc, "boolean expression expected", "", "");
}

// This function checks to see if the node (for the expression) contains a scalar boolean expression or not
void TParseContext::boolCheck(const TSourceLoc& loc, const TPublicType& pType)
{
    if (pType.basicType != EbtBool || pType.arraySizes || pType.matrixCols > 1 || (pType.vectorSize > 1))
        error(loc, "boolean expression expected", "", "");
}

void TParseContext::samplerCheck(const TSourceLoc& loc, const TType& type, const TString& identifier, TIntermTyped* /*initializer*/)
{
    // Check that the appropriate extension is enabled if external sampler is used.
    // There are two extensions. The correct one must be used based on GLSL version.
    if (type.getBasicType() == EbtSampler && type.getSampler().external) {
        if (version < 300) {
            requireExtensions(loc, 1, &E_GL_OES_EGL_image_external, "samplerExternalOES");
        } else {
            requireExtensions(loc, 1, &E_GL_OES_EGL_image_external_essl3, "samplerExternalOES");
        }
    }

    if (type.getQualifier().storage == EvqUniform)
        return;

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtSampler))
        error(loc, "non-uniform struct contains a sampler or image:", type.getBasicTypeString().c_str(), identifier.c_str());
    else if (type.getBasicType() == EbtSampler && type.getQualifier().storage != EvqUniform) {
        // non-uniform sampler
        // not yet:  okay if it has an initializer
        // if (! initializer)
        error(loc, "sampler/image types can only be used in uniform variables or function parameters:", type.getBasicTypeString().c_str(), identifier.c_str());
    }
}

void TParseContext::atomicUintCheck(const TSourceLoc& loc, const TType& type, const TString& identifier)
{
    if (type.getQualifier().storage == EvqUniform)
        return;

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtAtomicUint))
        error(loc, "non-uniform struct contains an atomic_uint:", type.getBasicTypeString().c_str(), identifier.c_str());
    else if (type.getBasicType() == EbtAtomicUint && type.getQualifier().storage != EvqUniform)
        error(loc, "atomic_uints can only be used in uniform variables or function parameters:", type.getBasicTypeString().c_str(), identifier.c_str());
}
#ifdef NV_EXTENSIONS
void TParseContext::accStructNVCheck(const TSourceLoc& loc, const TType& type, const TString& identifier)
{
    if (type.getQualifier().storage == EvqUniform)
        return;

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtAccStructNV))
        error(loc, "non-uniform struct contains an accelerationStructureNVX:", type.getBasicTypeString().c_str(), identifier.c_str());
    else if (type.getBasicType() == EbtAccStructNV && type.getQualifier().storage != EvqUniform)
        error(loc, "accelerationStructureNVX can only be used in uniform variables or function parameters:",
            type.getBasicTypeString().c_str(), identifier.c_str());

}
#endif

void TParseContext::transparentOpaqueCheck(const TSourceLoc& loc, const TType& type, const TString& identifier)
{
    if (parsingBuiltins)
        return;

    if (type.getQualifier().storage != EvqUniform)
        return;

    if (type.containsNonOpaque()) {
        // Vulkan doesn't allow transparent uniforms outside of blocks
        if (spvVersion.vulkan > 0)
            vulkanRemoved(loc, "non-opaque uniforms outside a block");
        // OpenGL wants locations on these (unless they are getting automapped)
        if (spvVersion.openGl > 0 && !type.getQualifier().hasLocation() && !intermediate.getAutoMapLocations())
            error(loc, "non-opaque uniform variables need a layout(location=L)", identifier.c_str(), "");
    }
}

//
// Qualifier checks knowing the qualifier and that it is a member of a struct/block.
//
void TParseContext::memberQualifierCheck(glslang::TPublicType& publicType)
{
    globalQualifierFixCheck(publicType.loc, publicType.qualifier);
    checkNoShaderLayouts(publicType.loc, publicType.shaderQualifiers);
    if (publicType.qualifier.isNonUniform()) {
        error(publicType.loc, "not allowed on block or structure members", "nonuniformEXT", "");
        publicType.qualifier.nonUniform = false;
    }
}

//
// Check/fix just a full qualifier (no variables or types yet, but qualifier is complete) at global level.
//
void TParseContext::globalQualifierFixCheck(const TSourceLoc& loc, TQualifier& qualifier)
{
    bool nonuniformOkay = false;

    // move from parameter/unknown qualifiers to pipeline in/out qualifiers
    switch (qualifier.storage) {
    case EvqIn:
        profileRequires(loc, ENoProfile, 130, nullptr, "in for stage inputs");
        profileRequires(loc, EEsProfile, 300, nullptr, "in for stage inputs");
        qualifier.storage = EvqVaryingIn;
        nonuniformOkay = true;
        break;
    case EvqOut:
        profileRequires(loc, ENoProfile, 130, nullptr, "out for stage outputs");
        profileRequires(loc, EEsProfile, 300, nullptr, "out for stage outputs");
        qualifier.storage = EvqVaryingOut;
        break;
    case EvqInOut:
        qualifier.storage = EvqVaryingIn;
        error(loc, "cannot use 'inout' at global scope", "", "");
        break;
    case EvqGlobal:
    case EvqTemporary:
        nonuniformOkay = true;
        break;
    default:
        break;
    }

    if (!nonuniformOkay && qualifier.nonUniform)
        error(loc, "for non-parameter, can only apply to 'in' or no storage qualifier", "nonuniformEXT", "");

    invariantCheck(loc, qualifier);
}

//
// Check a full qualifier and type (no variable yet) at global level.
//
void TParseContext::globalQualifierTypeCheck(const TSourceLoc& loc, const TQualifier& qualifier, const TPublicType& publicType)
{
    if (! symbolTable.atGlobalLevel())
        return;

    if (qualifier.isMemoryQualifierImageAndSSBOOnly() && ! publicType.isImage() && publicType.qualifier.storage != EvqBuffer) {
        error(loc, "memory qualifiers cannot be used on this type", "", "");
    } else if (qualifier.isMemory() && (publicType.basicType != EbtSampler) && !publicType.qualifier.isUniformOrBuffer()) {
        error(loc, "memory qualifiers cannot be used on this type", "", "");
    }

    if (qualifier.storage == EvqBuffer && publicType.basicType != EbtBlock)
        error(loc, "buffers can be declared only as blocks", "buffer", "");

    if (qualifier.storage != EvqVaryingIn && qualifier.storage != EvqVaryingOut)
        return;

    if (publicType.shaderQualifiers.blendEquation)
        error(loc, "can only be applied to a standalone 'out'", "blend equation", "");

    // now, knowing it is a shader in/out, do all the in/out semantic checks

    if (publicType.basicType == EbtBool && !parsingBuiltins) {
        error(loc, "cannot be bool", GetStorageQualifierString(qualifier.storage), "");
        return;
    }

    if (isTypeInt(publicType.basicType) || publicType.basicType == EbtDouble)
        profileRequires(loc, EEsProfile, 300, nullptr, "shader input/output");

    if (!qualifier.flat
#ifdef AMD_EXTENSIONS
        && !qualifier.explicitInterp
#endif
#ifdef NV_EXTENSIONS
        && !qualifier.pervertexNV
#endif
        ) {
        if (isTypeInt(publicType.basicType) ||
            publicType.basicType == EbtDouble ||
            (publicType.userDef && (publicType.userDef->containsBasicType(EbtInt8)   ||
                                    publicType.userDef->containsBasicType(EbtUint8)  ||
                                    publicType.userDef->containsBasicType(EbtInt16)  ||
                                    publicType.userDef->containsBasicType(EbtUint16) ||
                                    publicType.userDef->containsBasicType(EbtInt)    ||
                                    publicType.userDef->containsBasicType(EbtUint)   ||
                                    publicType.userDef->containsBasicType(EbtInt64)  ||
                                    publicType.userDef->containsBasicType(EbtUint64) ||
                                    publicType.userDef->containsBasicType(EbtDouble)))) {
            if (qualifier.storage == EvqVaryingIn && language == EShLangFragment)
                error(loc, "must be qualified as flat", TType::getBasicString(publicType.basicType), GetStorageQualifierString(qualifier.storage));
            else if (qualifier.storage == EvqVaryingOut && language == EShLangVertex && version == 300)
                error(loc, "must be qualified as flat", TType::getBasicString(publicType.basicType), GetStorageQualifierString(qualifier.storage));
        }
    }

    if (qualifier.patch && qualifier.isInterpolation())
        error(loc, "cannot use interpolation qualifiers with patch", "patch", "");

#ifdef NV_EXTENSIONS
    if (qualifier.perTaskNV && publicType.basicType != EbtBlock)
        error(loc, "taskNV variables can be declared only as blocks", "taskNV", "");
#endif

    if (qualifier.storage == EvqVaryingIn) {
        switch (language) {
        case EShLangVertex:
            if (publicType.basicType == EbtStruct) {
                error(loc, "cannot be a structure or array", GetStorageQualifierString(qualifier.storage), "");
                return;
            }
            if (publicType.arraySizes) {
                requireProfile(loc, ~EEsProfile, "vertex input arrays");
                profileRequires(loc, ENoProfile, 150, nullptr, "vertex input arrays");
            }
            if (publicType.basicType == EbtDouble)
                profileRequires(loc, ~EEsProfile, 410, nullptr, "vertex-shader `double` type input");
            if (qualifier.isAuxiliary() || qualifier.isInterpolation() || qualifier.isMemory() || qualifier.invariant)
                error(loc, "vertex input cannot be further qualified", "", "");
            break;

        case EShLangTessControl:
            if (qualifier.patch)
                error(loc, "can only use on output in tessellation-control shader", "patch", "");
            break;

        case EShLangTessEvaluation:
            break;

        case EShLangGeometry:
            break;

        case EShLangFragment:
            if (publicType.userDef) {
                profileRequires(loc, EEsProfile, 300, nullptr, "fragment-shader struct input");
                profileRequires(loc, ~EEsProfile, 150, nullptr, "fragment-shader struct input");
                if (publicType.userDef->containsStructure())
                    requireProfile(loc, ~EEsProfile, "fragment-shader struct input containing structure");
                if (publicType.userDef->containsArray())
                    requireProfile(loc, ~EEsProfile, "fragment-shader struct input containing an array");
            }
            break;

        case EShLangCompute:
            if (! symbolTable.atBuiltInLevel())
                error(loc, "global storage input qualifier cannot be used in a compute shader", "in", "");
            break;

        default:
            break;
        }
    } else {
        // qualifier.storage == EvqVaryingOut
        switch (language) {
        case EShLangVertex:
            if (publicType.userDef) {
                profileRequires(loc, EEsProfile, 300, nullptr, "vertex-shader struct output");
                profileRequires(loc, ~EEsProfile, 150, nullptr, "vertex-shader struct output");
                if (publicType.userDef->containsStructure())
                    requireProfile(loc, ~EEsProfile, "vertex-shader struct output containing structure");
                if (publicType.userDef->containsArray())
                    requireProfile(loc, ~EEsProfile, "vertex-shader struct output containing an array");
            }

            break;

        case EShLangTessControl:
            break;

        case EShLangTessEvaluation:
            if (qualifier.patch)
                error(loc, "can only use on input in tessellation-evaluation shader", "patch", "");
            break;

        case EShLangGeometry:
            break;

        case EShLangFragment:
            profileRequires(loc, EEsProfile, 300, nullptr, "fragment shader output");
            if (publicType.basicType == EbtStruct) {
                error(loc, "cannot be a structure", GetStorageQualifierString(qualifier.storage), "");
                return;
            }
            if (publicType.matrixRows > 0) {
                error(loc, "cannot be a matrix", GetStorageQualifierString(qualifier.storage), "");
                return;
            }
            if (qualifier.isAuxiliary())
                error(loc, "can't use auxiliary qualifier on a fragment output", "centroid/sample/patch", "");
            if (qualifier.isInterpolation())
                error(loc, "can't use interpolation qualifier on a fragment output", "flat/smooth/noperspective", "");
            if (publicType.basicType == EbtDouble || publicType.basicType == EbtInt64 || publicType.basicType == EbtUint64)
                error(loc, "cannot contain a double, int64, or uint64", GetStorageQualifierString(qualifier.storage), "");
        break;

        case EShLangCompute:
            error(loc, "global storage output qualifier cannot be used in a compute shader", "out", "");
            break;

        default:
            break;
        }
    }
}

//
// Merge characteristics of the 'src' qualifier into the 'dst'.
// If there is duplication, issue error messages, unless 'force'
// is specified, which means to just override default settings.
//
// Also, when force is false, it will be assumed that 'src' follows
// 'dst', for the purpose of error checking order for versions
// that require specific orderings of qualifiers.
//
void TParseContext::mergeQualifiers(const TSourceLoc& loc, TQualifier& dst, const TQualifier& src, bool force)
{
    // Multiple auxiliary qualifiers (mostly done later by 'individual qualifiers')
    if (src.isAuxiliary() && dst.isAuxiliary())
        error(loc, "can only have one auxiliary qualifier (centroid, patch, and sample)", "", "");

    // Multiple interpolation qualifiers (mostly done later by 'individual qualifiers')
    if (src.isInterpolation() && dst.isInterpolation())
#ifdef AMD_EXTENSIONS
        error(loc, "can only have one interpolation qualifier (flat, smooth, noperspective, __explicitInterpAMD)", "", "");
#else
        error(loc, "can only have one interpolation qualifier (flat, smooth, noperspective)", "", "");
#endif

    // Ordering
    if (! force && ((profile != EEsProfile && version < 420) ||
                    (profile == EEsProfile && version < 310))
                && ! extensionTurnedOn(E_GL_ARB_shading_language_420pack)) {
        // non-function parameters
        if (src.noContraction && (dst.invariant || dst.isInterpolation() || dst.isAuxiliary() || dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "precise qualifier must appear first", "", "");
        if (src.invariant && (dst.isInterpolation() || dst.isAuxiliary() || dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "invariant qualifier must appear before interpolation, storage, and precision qualifiers ", "", "");
        else if (src.isInterpolation() && (dst.isAuxiliary() || dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "interpolation qualifiers must appear before storage and precision qualifiers", "", "");
        else if (src.isAuxiliary() && (dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "Auxiliary qualifiers (centroid, patch, and sample) must appear before storage and precision qualifiers", "", "");
        else if (src.storage != EvqTemporary && (dst.precision != EpqNone))
            error(loc, "precision qualifier must appear as last qualifier", "", "");

        // function parameters
        if (src.noContraction && (dst.storage == EvqConst || dst.storage == EvqIn || dst.storage == EvqOut))
            error(loc, "precise qualifier must appear first", "", "");
        if (src.storage == EvqConst && (dst.storage == EvqIn || dst.storage == EvqOut))
            error(loc, "in/out must appear before const", "", "");
    }

    // Storage qualification
    if (dst.storage == EvqTemporary || dst.storage == EvqGlobal)
        dst.storage = src.storage;
    else if ((dst.storage == EvqIn  && src.storage == EvqOut) ||
             (dst.storage == EvqOut && src.storage == EvqIn))
        dst.storage = EvqInOut;
    else if ((dst.storage == EvqIn    && src.storage == EvqConst) ||
             (dst.storage == EvqConst && src.storage == EvqIn))
        dst.storage = EvqConstReadOnly;
    else if (src.storage != EvqTemporary &&
             src.storage != EvqGlobal)
        error(loc, "too many storage qualifiers", GetStorageQualifierString(src.storage), "");

    // Precision qualifiers
    if (! force && src.precision != EpqNone && dst.precision != EpqNone)
        error(loc, "only one precision qualifier allowed", GetPrecisionQualifierString(src.precision), "");
    if (dst.precision == EpqNone || (force && src.precision != EpqNone))
        dst.precision = src.precision;

    if (!force && ((src.coherent && (dst.devicecoherent || dst.queuefamilycoherent || dst.workgroupcoherent || dst.subgroupcoherent)) ||
                   (src.devicecoherent && (dst.coherent || dst.queuefamilycoherent || dst.workgroupcoherent || dst.subgroupcoherent)) ||
                   (src.queuefamilycoherent && (dst.coherent || dst.devicecoherent || dst.workgroupcoherent || dst.subgroupcoherent)) ||
                   (src.workgroupcoherent && (dst.coherent || dst.devicecoherent || dst.queuefamilycoherent || dst.subgroupcoherent)) ||
                   (src.subgroupcoherent  && (dst.coherent || dst.devicecoherent || dst.queuefamilycoherent || dst.workgroupcoherent)))) {
        error(loc, "only one coherent/devicecoherent/queuefamilycoherent/workgroupcoherent/subgroupcoherent qualifier allowed", GetPrecisionQualifierString(src.precision), "");
    }
    // Layout qualifiers
    mergeObjectLayoutQualifiers(dst, src, false);

    // individual qualifiers
    bool repeated = false;
    #define MERGE_SINGLETON(field) repeated |= dst.field && src.field; dst.field |= src.field;
    MERGE_SINGLETON(invariant);
    MERGE_SINGLETON(noContraction);
    MERGE_SINGLETON(centroid);
    MERGE_SINGLETON(smooth);
    MERGE_SINGLETON(flat);
    MERGE_SINGLETON(nopersp);
#ifdef AMD_EXTENSIONS
    MERGE_SINGLETON(explicitInterp);
#endif
#ifdef NV_EXTENSIONS
    MERGE_SINGLETON(perPrimitiveNV);
    MERGE_SINGLETON(perViewNV);
    MERGE_SINGLETON(perTaskNV);
#endif
    MERGE_SINGLETON(patch);
    MERGE_SINGLETON(sample);
    MERGE_SINGLETON(coherent);
    MERGE_SINGLETON(devicecoherent);
    MERGE_SINGLETON(queuefamilycoherent);
    MERGE_SINGLETON(workgroupcoherent);
    MERGE_SINGLETON(subgroupcoherent);
    MERGE_SINGLETON(nonprivate);
    MERGE_SINGLETON(volatil);
    MERGE_SINGLETON(restrict);
    MERGE_SINGLETON(readonly);
    MERGE_SINGLETON(writeonly);
    MERGE_SINGLETON(specConstant);
    MERGE_SINGLETON(nonUniform);

    if (repeated)
        error(loc, "replicated qualifiers", "", "");
}

void TParseContext::setDefaultPrecision(const TSourceLoc& loc, TPublicType& publicType, TPrecisionQualifier qualifier)
{
    TBasicType basicType = publicType.basicType;

    if (basicType == EbtSampler) {
        defaultSamplerPrecision[computeSamplerTypeIndex(publicType.sampler)] = qualifier;

        return;  // all is well
    }

    if (basicType == EbtInt || basicType == EbtFloat) {
        if (publicType.isScalar()) {
            defaultPrecision[basicType] = qualifier;
            if (basicType == EbtInt) {
                defaultPrecision[EbtUint] = qualifier;
                precisionManager.explicitIntDefaultSeen();
            } else
                precisionManager.explicitFloatDefaultSeen();

            return;  // all is well
        }
    }

    if (basicType == EbtAtomicUint) {
        if (qualifier != EpqHigh)
            error(loc, "can only apply highp to atomic_uint", "precision", "");

        return;
    }

    error(loc, "cannot apply precision statement to this type; use 'float', 'int' or a sampler type", TType::getBasicString(basicType), "");
}

// used to flatten the sampler type space into a single dimension
// correlates with the declaration of defaultSamplerPrecision[]
int TParseContext::computeSamplerTypeIndex(TSampler& sampler)
{
    int arrayIndex    = sampler.arrayed ? 1 : 0;
    int shadowIndex   = sampler.shadow  ? 1 : 0;
    int externalIndex = sampler.external? 1 : 0;
    int imageIndex    = sampler.image   ? 1 : 0;
    int msIndex       = sampler.ms      ? 1 : 0;

    int flattened = EsdNumDims * (EbtNumTypes * (2 * (2 * (2 * (2 * arrayIndex + msIndex) + imageIndex) + shadowIndex) +
                                                 externalIndex) + sampler.type) + sampler.dim;
    assert(flattened < maxSamplerIndex);

    return flattened;
}

TPrecisionQualifier TParseContext::getDefaultPrecision(TPublicType& publicType)
{
    if (publicType.basicType == EbtSampler)
        return defaultSamplerPrecision[computeSamplerTypeIndex(publicType.sampler)];
    else
        return defaultPrecision[publicType.basicType];
}

void TParseContext::precisionQualifierCheck(const TSourceLoc& loc, TBasicType baseType, TQualifier& qualifier)
{
    // Built-in symbols are allowed some ambiguous precisions, to be pinned down
    // later by context.
    if (! obeyPrecisionQualifiers() || parsingBuiltins)
        return;

    if (baseType == EbtAtomicUint && qualifier.precision != EpqNone && qualifier.precision != EpqHigh)
        error(loc, "atomic counters can only be highp", "atomic_uint", "");

    if (baseType == EbtFloat || baseType == EbtUint || baseType == EbtInt || baseType == EbtSampler || baseType == EbtAtomicUint) {
        if (qualifier.precision == EpqNone) {
            if (relaxedErrors())
                warn(loc, "type requires declaration of default precision qualifier", TType::getBasicString(baseType), "substituting 'mediump'");
            else
                error(loc, "type requires declaration of default precision qualifier", TType::getBasicString(baseType), "");
            qualifier.precision = EpqMedium;
            defaultPrecision[baseType] = EpqMedium;
        }
    } else if (qualifier.precision != EpqNone)
        error(loc, "type cannot have precision qualifier", TType::getBasicString(baseType), "");
}

void TParseContext::parameterTypeCheck(const TSourceLoc& loc, TStorageQualifier qualifier, const TType& type)
{
    if ((qualifier == EvqOut || qualifier == EvqInOut) && type.isOpaque())
        error(loc, "samplers and atomic_uints cannot be output parameters", type.getBasicTypeString().c_str(), "");

    if (!parsingBuiltins && type.containsBasicType(EbtFloat16))
        requireFloat16Arithmetic(loc, type.getBasicTypeString().c_str(), "float16 types can only be in uniform block or buffer storage");
    if (!parsingBuiltins && type.contains16BitInt())
        requireInt16Arithmetic(loc, type.getBasicTypeString().c_str(), "(u)int16 types can only be in uniform block or buffer storage");
    if (!parsingBuiltins && type.contains8BitInt())
        requireInt8Arithmetic(loc, type.getBasicTypeString().c_str(), "(u)int8 types can only be in uniform block or buffer storage");
}

bool TParseContext::containsFieldWithBasicType(const TType& type, TBasicType basicType)
{
    if (type.getBasicType() == basicType)
        return true;

    if (type.getBasicType() == EbtStruct) {
        const TTypeList& structure = *type.getStruct();
        for (unsigned int i = 0; i < structure.size(); ++i) {
            if (containsFieldWithBasicType(*structure[i].type, basicType))
                return true;
        }
    }

    return false;
}

//
// Do size checking for an array type's size.
//
void TParseContext::arraySizeCheck(const TSourceLoc& loc, TIntermTyped* expr, TArraySize& sizePair)
{
    bool isConst = false;
    sizePair.node = nullptr;

    int size = 1;

    TIntermConstantUnion* constant = expr->getAsConstantUnion();
    if (constant) {
        // handle true (non-specialization) constant
        size = constant->getConstArray()[0].getIConst();
        isConst = true;
    } else {
        // see if it's a specialization constant instead
        if (expr->getQualifier().isSpecConstant()) {
            isConst = true;
            sizePair.node = expr;
            TIntermSymbol* symbol = expr->getAsSymbolNode();
            if (symbol && symbol->getConstArray().size() > 0)
                size = symbol->getConstArray()[0].getIConst();
        }
    }

    sizePair.size = size;

    if (! isConst || (expr->getBasicType() != EbtInt && expr->getBasicType() != EbtUint)) {
        error(loc, "array size must be a constant integer expression", "", "");
        return;
    }

    if (size <= 0) {
        error(loc, "array size must be a positive integer", "", "");
        return;
    }
}

//
// See if this qualifier can be an array.
//
// Returns true if there is an error.
//
bool TParseContext::arrayQualifierError(const TSourceLoc& loc, const TQualifier& qualifier)
{
    if (qualifier.storage == EvqConst) {
        profileRequires(loc, ENoProfile, 120, E_GL_3DL_array_objects, "const array");
        profileRequires(loc, EEsProfile, 300, nullptr, "const array");
    }

    if (qualifier.storage == EvqVaryingIn && language == EShLangVertex) {
        requireProfile(loc, ~EEsProfile, "vertex input arrays");
        profileRequires(loc, ENoProfile, 150, nullptr, "vertex input arrays");
    }

    return false;
}

//
// See if this qualifier and type combination can be an array.
// Assumes arrayQualifierError() was also called to catch the type-invariant tests.
//
// Returns true if there is an error.
//
bool TParseContext::arrayError(const TSourceLoc& loc, const TType& type)
{
    if (type.getQualifier().storage == EvqVaryingOut && language == EShLangVertex) {
        if (type.isArrayOfArrays())
            requireProfile(loc, ~EEsProfile, "vertex-shader array-of-array output");
        else if (type.isStruct())
            requireProfile(loc, ~EEsProfile, "vertex-shader array-of-struct output");
    }
    if (type.getQualifier().storage == EvqVaryingIn && language == EShLangFragment) {
        if (type.isArrayOfArrays())
            requireProfile(loc, ~EEsProfile, "fragment-shader array-of-array input");
        else if (type.isStruct())
            requireProfile(loc, ~EEsProfile, "fragment-shader array-of-struct input");
    }
    if (type.getQualifier().storage == EvqVaryingOut && language == EShLangFragment) {
        if (type.isArrayOfArrays())
            requireProfile(loc, ~EEsProfile, "fragment-shader array-of-array output");
    }

    return false;
}

//
// Require array to be completely sized
//
void TParseContext::arraySizeRequiredCheck(const TSourceLoc& loc, const TArraySizes& arraySizes)
{
    if (arraySizes.hasUnsized())
        error(loc, "array size required", "", "");
}

void TParseContext::structArrayCheck(const TSourceLoc& /*loc*/, const TType& type)
{
    const TTypeList& structure = *type.getStruct();
    for (int m = 0; m < (int)structure.size(); ++m) {
        const TType& member = *structure[m].type;
        if (member.isArray())
            arraySizeRequiredCheck(structure[m].loc, *member.getArraySizes());
    }
}

void TParseContext::arraySizesCheck(const TSourceLoc& loc, const TQualifier& qualifier, TArraySizes* arraySizes,
    const TIntermTyped* initializer, bool lastMember)
{
    assert(arraySizes);

    // always allow special built-in ins/outs sized to topologies
    if (parsingBuiltins)
        return;

    // initializer must be a sized array, in which case
    // allow the initializer to set any unknown array sizes
    if (initializer != nullptr) {
        if (initializer->getType().isUnsizedArray())
            error(loc, "array initializer must be sized", "[]", "");
        return;
    }

    // No environment allows any non-outer-dimension to be implicitly sized
    if (arraySizes->isInnerUnsized()) {
        error(loc, "only outermost dimension of an array of arrays can be implicitly sized", "[]", "");
        arraySizes->clearInnerUnsized();
    }

    if (arraySizes->isInnerSpecialization())
        error(loc, "only outermost dimension of an array of arrays can be a specialization constant", "[]", "");

    // desktop always allows outer-dimension-unsized variable arrays,
    if (profile != EEsProfile)
        return;

    // for ES, if size isn't coming from an initializer, it has to be explicitly declared now,
    // with very few exceptions

    // last member of ssbo block exception:
    if (qualifier.storage == EvqBuffer && lastMember)
        return;

    // implicitly-sized io exceptions:
    switch (language) {
    case EShLangGeometry:
        if (qualifier.storage == EvqVaryingIn)
            if ((profile == EEsProfile && version >= 320) ||
                extensionsTurnedOn(Num_AEP_geometry_shader, AEP_geometry_shader))
                return;
        break;
    case EShLangTessControl:
        if ( qualifier.storage == EvqVaryingIn ||
            (qualifier.storage == EvqVaryingOut && ! qualifier.patch))
            if ((profile == EEsProfile && version >= 320) ||
                extensionsTurnedOn(Num_AEP_tessellation_shader, AEP_tessellation_shader))
                return;
        break;
    case EShLangTessEvaluation:
        if ((qualifier.storage == EvqVaryingIn && ! qualifier.patch) ||
             qualifier.storage == EvqVaryingOut)
            if ((profile == EEsProfile && version >= 320) ||
                extensionsTurnedOn(Num_AEP_tessellation_shader, AEP_tessellation_shader))
                return;
        break;
#ifdef NV_EXTENSIONS
    case EShLangMeshNV:
        if (qualifier.storage == EvqVaryingOut)
            if ((profile == EEsProfile && version >= 320) ||
                extensionTurnedOn(E_GL_NV_mesh_shader))
                return;
        break;
#endif
    default:
        break;
    }

    arraySizeRequiredCheck(loc, *arraySizes);
}

void TParseContext::arrayOfArrayVersionCheck(const TSourceLoc& loc, const TArraySizes* sizes)
{
    if (sizes == nullptr || sizes->getNumDims() == 1)
        return;

    const char* feature = "arrays of arrays";

    requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, feature);
    profileRequires(loc, EEsProfile, 310, nullptr, feature);
    profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, nullptr, feature);
}

//
// Do all the semantic checking for declaring or redeclaring an array, with and
// without a size, and make the right changes to the symbol table.
//
void TParseContext::declareArray(const TSourceLoc& loc, const TString& identifier, const TType& type, TSymbol*& symbol)
{
    if (symbol == nullptr) {
        bool currentScope;
        symbol = symbolTable.find(identifier, nullptr, &currentScope);

        if (symbol && builtInName(identifier) && ! symbolTable.atBuiltInLevel()) {
            // bad shader (errors already reported) trying to redeclare a built-in name as an array
            symbol = nullptr;
            return;
        }
        if (symbol == nullptr || ! currentScope) {
            //
            // Successfully process a new definition.
            // (Redeclarations have to take place at the same scope; otherwise they are hiding declarations)
            //
            symbol = new TVariable(&identifier, type);
            symbolTable.insert(*symbol);
            if (symbolTable.atGlobalLevel())
                trackLinkage(*symbol);

            if (! symbolTable.atBuiltInLevel()) {
                if (isIoResizeArray(type)) {
                    ioArraySymbolResizeList.push_back(symbol);
                    checkIoArraysConsistency(loc, true, type.getQualifier().isPerPrimitive());
                } else
                    fixIoArraySize(loc, symbol->getWritableType());
            }

            return;
        }
        if (symbol->getAsAnonMember()) {
            error(loc, "cannot redeclare a user-block member array", identifier.c_str(), "");
            symbol = nullptr;
            return;
        }
    }

    //
    // Process a redeclaration.
    //

    if (symbol == nullptr) {
        error(loc, "array variable name expected", identifier.c_str(), "");
        return;
    }

    // redeclareBuiltinVariable() should have already done the copyUp()
    TType& existingType = symbol->getWritableType();

    if (! existingType.isArray()) {
        error(loc, "redeclaring non-array as array", identifier.c_str(), "");
        return;
    }

    if (! existingType.sameElementType(type)) {
        error(loc, "redeclaration of array with a different element type", identifier.c_str(), "");
        return;
    }

    if (! existingType.sameInnerArrayness(type)) {
        error(loc, "redeclaration of array with a different array dimensions or sizes", identifier.c_str(), "");
        return;
    }

    if (existingType.isSizedArray()) {
        // be more leniant for input arrays to geometry shaders and tessellation control outputs, where the redeclaration is the same size
        if (! (isIoResizeArray(type) && existingType.getOuterArraySize() == type.getOuterArraySize()))
            error(loc, "redeclaration of array with size", identifier.c_str(), "");
        return;
    }

    arrayLimitCheck(loc, identifier, type.getOuterArraySize());

    existingType.updateArraySizes(type);

    if (isIoResizeArray(type))
        checkIoArraysConsistency(loc, false, type.getQualifier().isPerPrimitive());
}

// Policy and error check for needing a runtime sized array.
void TParseContext::checkRuntimeSizable(const TSourceLoc& loc, const TIntermTyped& base)
{
    // runtime length implies runtime sizeable, so no problem
    if (isRuntimeLength(base))
        return;

    // check for additional things allowed by GL_EXT_nonuniform_qualifier
    if (base.getBasicType() == EbtSampler ||
            (base.getBasicType() == EbtBlock && base.getType().getQualifier().isUniformOrBuffer()))
        requireExtensions(loc, 1, &E_GL_EXT_nonuniform_qualifier, "variable index");
    else
        error(loc, "", "[", "array must be redeclared with a size before being indexed with a variable");
}

// Policy decision for whether a run-time .length() is allowed.
bool TParseContext::isRuntimeLength(const TIntermTyped& base) const
{
    if (base.getType().getQualifier().storage == EvqBuffer) {
        // in a buffer block
        const TIntermBinary* binary = base.getAsBinaryNode();
        if (binary != nullptr && binary->getOp() == EOpIndexDirectStruct) {
            // is it the last member?
            const int index = binary->getRight()->getAsConstantUnion()->getConstArray()[0].getIConst();
            const int memberCount = (int)binary->getLeft()->getType().getStruct()->size();
            if (index == memberCount - 1)
                return true;
        }
    }

    return false;
}

#ifdef NV_EXTENSIONS
// Fix mesh view output array dimension
void TParseContext::resizeMeshViewDimension(const TSourceLoc& loc, TType& type)
{
    // see if member is a per-view attribute
    if (type.getQualifier().isPerView()) {
        // since we don't have the maxMeshViewCountNV set during parsing builtins, we hardcode the value
        int maxViewCount = parsingBuiltins ? 4 : resources.maxMeshViewCountNV;

        if (! type.isArray()) {
            error(loc, "requires an view array dimension", "perviewNV", "");
        }
        else if (!type.isUnsizedArray() && type.getOuterArraySize() != maxViewCount) {
            error(loc, "mesh view output array size must be gl_MaxMeshViewCountNV or implicitly sized", "[]", "");
        }
        else if (type.isUnsizedArray()) {
            type.changeOuterArraySize(maxViewCount);
        }
    }
}
#endif

// Returns true if the first argument to the #line directive is the line number for the next line.
//
// Desktop, pre-version 3.30:  "After processing this directive
// (including its new-line), the implementation will behave as if it is compiling at line number line+1 and
// source string number source-string-number."
//
// Desktop, version 3.30 and later, and ES:  "After processing this directive
// (including its new-line), the implementation will behave as if it is compiling at line number line and
// source string number source-string-number.
bool TParseContext::lineDirectiveShouldSetNextLine() const
{
    return profile == EEsProfile || version >= 330;
}

//
// Enforce non-initializer type/qualifier rules.
//
void TParseContext::nonInitConstCheck(const TSourceLoc& loc, TString& identifier, TType& type)
{
    //
    // Make the qualifier make sense, given that there is not an initializer.
    //
    if (type.getQualifier().storage == EvqConst ||
        type.getQualifier().storage == EvqConstReadOnly) {
        type.getQualifier().makeTemporary();
        error(loc, "variables with qualifier 'const' must be initialized", identifier.c_str(), "");
    }
}

//
// See if the identifier is a built-in symbol that can be redeclared, and if so,
// copy the symbol table's read-only built-in variable to the current
// global level, where it can be modified based on the passed in type.
//
// Returns nullptr if no redeclaration took place; meaning a normal declaration still
// needs to occur for it, not necessarily an error.
//
// Returns a redeclared and type-modified variable if a redeclarated occurred.
//
TSymbol* TParseContext::redeclareBuiltinVariable(const TSourceLoc& loc, const TString& identifier,
                                                 const TQualifier& qualifier, const TShaderQualifiers& publicType)
{
    if (! builtInName(identifier) || symbolTable.atBuiltInLevel() || ! symbolTable.atGlobalLevel())
        return nullptr;

    bool nonEsRedecls = (profile != EEsProfile && (version >= 130 || identifier == "gl_TexCoord"));
    bool    esRedecls = (profile == EEsProfile &&
                         (version >= 320 || extensionsTurnedOn(Num_AEP_shader_io_blocks, AEP_shader_io_blocks)));
    if (! esRedecls && ! nonEsRedecls)
        return nullptr;

    // Special case when using GL_ARB_separate_shader_objects
    bool ssoPre150 = false;  // means the only reason this variable is redeclared is due to this combination
    if (profile != EEsProfile && version <= 140 && extensionTurnedOn(E_GL_ARB_separate_shader_objects)) {
        if (identifier == "gl_Position"     ||
            identifier == "gl_PointSize"    ||
            identifier == "gl_ClipVertex"   ||
            identifier == "gl_FogFragCoord")
            ssoPre150 = true;
    }

    // Potentially redeclaring a built-in variable...

    if (ssoPre150 ||
        (identifier == "gl_FragDepth"           && ((nonEsRedecls && version >= 420) || esRedecls)) ||
        (identifier == "gl_FragCoord"           && ((nonEsRedecls && version >= 150) || esRedecls)) ||
         identifier == "gl_ClipDistance"                                                            ||
         identifier == "gl_CullDistance"                                                            ||
         identifier == "gl_FrontColor"                                                              ||
         identifier == "gl_BackColor"                                                               ||
         identifier == "gl_FrontSecondaryColor"                                                     ||
         identifier == "gl_BackSecondaryColor"                                                      ||
         identifier == "gl_SecondaryColor"                                                          ||
        (identifier == "gl_Color"               && language == EShLangFragment)                     ||
#ifdef NV_EXTENSIONS
         identifier == "gl_SampleMask"                                                              ||
         identifier == "gl_Layer"                                                                   ||
#endif
         identifier == "gl_TexCoord") {

        // Find the existing symbol, if any.
        bool builtIn;
        TSymbol* symbol = symbolTable.find(identifier, &builtIn);

        // If the symbol was not found, this must be a version/profile/stage
        // that doesn't have it.
        if (! symbol)
            return nullptr;

        // If it wasn't at a built-in level, then it's already been redeclared;
        // that is, this is a redeclaration of a redeclaration; reuse that initial
        // redeclaration.  Otherwise, make the new one.
        if (builtIn)
            makeEditable(symbol);

        // Now, modify the type of the copy, as per the type of the current redeclaration.

        TQualifier& symbolQualifier = symbol->getWritableType().getQualifier();
        if (ssoPre150) {
            if (intermediate.inIoAccessed(identifier))
                error(loc, "cannot redeclare after use", identifier.c_str(), "");
            if (qualifier.hasLayout())
                error(loc, "cannot apply layout qualifier to", "redeclaration", symbol->getName().c_str());
            if (qualifier.isMemory() || qualifier.isAuxiliary() || (language == EShLangVertex   && qualifier.storage != EvqVaryingOut) ||
                                                                   (language == EShLangFragment && qualifier.storage != EvqVaryingIn))
                error(loc, "cannot change storage, memory, or auxiliary qualification of", "redeclaration", symbol->getName().c_str());
            if (! qualifier.smooth)
                error(loc, "cannot change interpolation qualification of", "redeclaration", symbol->getName().c_str());
        } else if (identifier == "gl_FrontColor"          ||
                   identifier == "gl_BackColor"           ||
                   identifier == "gl_FrontSecondaryColor" ||
                   identifier == "gl_BackSecondaryColor"  ||
                   identifier == "gl_SecondaryColor"      ||
                   identifier == "gl_Color") {
            symbolQualifier.flat = qualifier.flat;
            symbolQualifier.smooth = qualifier.smooth;
            symbolQualifier.nopersp = qualifier.nopersp;
            if (qualifier.hasLayout())
                error(loc, "cannot apply layout qualifier to", "redeclaration", symbol->getName().c_str());
            if (qualifier.isMemory() || qualifier.isAuxiliary() || symbol->getType().getQualifier().storage != qualifier.storage)
                error(loc, "cannot change storage, memory, or auxiliary qualification of", "redeclaration", symbol->getName().c_str());
        } else if (identifier == "gl_TexCoord"     ||
                   identifier == "gl_ClipDistance" ||
                   identifier == "gl_CullDistance") {
            if (qualifier.hasLayout() || qualifier.isMemory() || qualifier.isAuxiliary() ||
                qualifier.nopersp != symbolQualifier.nopersp || qualifier.flat != symbolQualifier.flat ||
                symbolQualifier.storage != qualifier.storage)
                error(loc, "cannot change qualification of", "redeclaration", symbol->getName().c_str());
        } else if (identifier == "gl_FragCoord") {
            if (intermediate.inIoAccessed("gl_FragCoord"))
                error(loc, "cannot redeclare after use", "gl_FragCoord", "");
            if (qualifier.nopersp != symbolQualifier.nopersp || qualifier.flat != symbolQualifier.flat ||
                qualifier.isMemory() || qualifier.isAuxiliary())
                error(loc, "can only change layout qualification of", "redeclaration", symbol->getName().c_str());
            if (qualifier.storage != EvqVaryingIn)
                error(loc, "cannot change input storage qualification of", "redeclaration", symbol->getName().c_str());
            if (! builtIn && (publicType.pixelCenterInteger != intermediate.getPixelCenterInteger() ||
                              publicType.originUpperLeft != intermediate.getOriginUpperLeft()))
                error(loc, "cannot redeclare with different qualification:", "redeclaration", symbol->getName().c_str());
            if (publicType.pixelCenterInteger)
                intermediate.setPixelCenterInteger();
            if (publicType.originUpperLeft)
                intermediate.setOriginUpperLeft();
        } else if (identifier == "gl_FragDepth") {
            if (qualifier.nopersp != symbolQualifier.nopersp || qualifier.flat != symbolQualifier.flat ||
                qualifier.isMemory() || qualifier.isAuxiliary())
                error(loc, "can only change layout qualification of", "redeclaration", symbol->getName().c_str());
            if (qualifier.storage != EvqVaryingOut)
                error(loc, "cannot change output storage qualification of", "redeclaration", symbol->getName().c_str());
            if (publicType.layoutDepth != EldNone) {
                if (intermediate.inIoAccessed("gl_FragDepth"))
                    error(loc, "cannot redeclare after use", "gl_FragDepth", "");
                if (! intermediate.setDepth(publicType.layoutDepth))
                    error(loc, "all redeclarations must use the same depth layout on", "redeclaration", symbol->getName().c_str());
            }
        }
#ifdef NV_EXTENSIONS
        else if (identifier == "gl_SampleMask") {
            if (!publicType.layoutOverrideCoverage) {
                error(loc, "redeclaration only allowed for override_coverage layout", "redeclaration", symbol->getName().c_str());
            }
            intermediate.setLayoutOverrideCoverage();
        }
        else if (identifier == "gl_Layer") {
            if (!qualifier.layoutViewportRelative && qualifier.layoutSecondaryViewportRelativeOffset == -2048)
                error(loc, "redeclaration only allowed for viewport_relative or secondary_view_offset layout", "redeclaration", symbol->getName().c_str());
            symbolQualifier.layoutViewportRelative = qualifier.layoutViewportRelative;
            symbolQualifier.layoutSecondaryViewportRelativeOffset = qualifier.layoutSecondaryViewportRelativeOffset;
        }
#endif

        // TODO: semantics quality: separate smooth from nothing declared, then use IsInterpolation for several tests above

        return symbol;
    }

    return nullptr;
}

//
// Either redeclare the requested block, or give an error message why it can't be done.
//
// TODO: functionality: explicitly sizing members of redeclared blocks is not giving them an explicit size
void TParseContext::redeclareBuiltinBlock(const TSourceLoc& loc, TTypeList& newTypeList, const TString& blockName,
    const TString* instanceName, TArraySizes* arraySizes)
{
    const char* feature = "built-in block redeclaration";
    profileRequires(loc, EEsProfile, 320, Num_AEP_shader_io_blocks, AEP_shader_io_blocks, feature);
    profileRequires(loc, ~EEsProfile, 410, E_GL_ARB_separate_shader_objects, feature);

    if (blockName != "gl_PerVertex" && blockName != "gl_PerFragment"
#ifdef NV_EXTENSIONS
        && blockName != "gl_MeshPerVertexNV" && blockName != "gl_MeshPerPrimitiveNV"
#endif
       )
    {
        error(loc, "cannot redeclare block: ", "block declaration", blockName.c_str());
        return;
    }

    // Redeclaring a built-in block...

    if (instanceName && ! builtInName(*instanceName)) {
        error(loc, "cannot redeclare a built-in block with a user name", instanceName->c_str(), "");
        return;
    }

    // Blocks with instance names are easy to find, lookup the instance name,
    // Anonymous blocks need to be found via a member.
    bool builtIn;
    TSymbol* block;
    if (instanceName)
        block = symbolTable.find(*instanceName, &builtIn);
    else
        block = symbolTable.find(newTypeList.front().type->getFieldName(), &builtIn);

    // If the block was not found, this must be a version/profile/stage
    // that doesn't have it, or the instance name is wrong.
    const char* errorName = instanceName ? instanceName->c_str() : newTypeList.front().type->getFieldName().c_str();
    if (! block) {
        error(loc, "no declaration found for redeclaration", errorName, "");
        return;
    }
    // Built-in blocks cannot be redeclared more than once, which if happened,
    // we'd be finding the already redeclared one here, rather than the built in.
    if (! builtIn) {
        error(loc, "can only redeclare a built-in block once, and before any use", blockName.c_str(), "");
        return;
    }

    // Copy the block to make a writable version, to insert into the block table after editing.
    block = symbolTable.copyUpDeferredInsert(block);

    if (block->getType().getBasicType() != EbtBlock) {
        error(loc, "cannot redeclare a non block as a block", errorName, "");
        return;
    }

    // Fix XFB stuff up, it applies to the order of the redeclaration, not
    // the order of the original members.
    if (currentBlockQualifier.storage == EvqVaryingOut && globalOutputDefaults.hasXfbBuffer()) {
        if (!currentBlockQualifier.hasXfbBuffer())
            currentBlockQualifier.layoutXfbBuffer = globalOutputDefaults.layoutXfbBuffer;
        fixBlockXfbOffsets(currentBlockQualifier, newTypeList);
    }

    // Edit and error check the container against the redeclaration
    //  - remove unused members
    //  - ensure remaining qualifiers/types match

    TType& type = block->getWritableType();

#ifdef NV_EXTENSIONS
    // if gl_PerVertex is redeclared for the purpose of passing through "gl_Position"
    // for passthrough purpose, the redeclared block should have the same qualifers as
    // the current one
    if (currentBlockQualifier.layoutPassthrough) {
        type.getQualifier().layoutPassthrough = currentBlockQualifier.layoutPassthrough;
        type.getQualifier().storage = currentBlockQualifier.storage;
        type.getQualifier().layoutStream = currentBlockQualifier.layoutStream;
        type.getQualifier().layoutXfbBuffer = currentBlockQualifier.layoutXfbBuffer;
    }
#endif

    TTypeList::iterator member = type.getWritableStruct()->begin();
    size_t numOriginalMembersFound = 0;
    while (member != type.getStruct()->end()) {
        // look for match
        bool found = false;
        TTypeList::const_iterator newMember;
        TSourceLoc memberLoc;
        memberLoc.init();
        for (newMember = newTypeList.begin(); newMember != newTypeList.end(); ++newMember) {
            if (member->type->getFieldName() == newMember->type->getFieldName()) {
                found = true;
                memberLoc = newMember->loc;
                break;
            }
        }

        if (found) {
            ++numOriginalMembersFound;
            // - ensure match between redeclared members' types
            // - check for things that can't be changed
            // - update things that can be changed
            TType& oldType = *member->type;
            const TType& newType = *newMember->type;
            if (! newType.sameElementType(oldType))
                error(memberLoc, "cannot redeclare block member with a different type", member->type->getFieldName().c_str(), "");
            if (oldType.isArray() != newType.isArray())
                error(memberLoc, "cannot change arrayness of redeclared block member", member->type->getFieldName().c_str(), "");
            else if (! oldType.getQualifier().isPerView() && ! oldType.sameArrayness(newType) && oldType.isSizedArray())
                error(memberLoc, "cannot change array size of redeclared block member", member->type->getFieldName().c_str(), "");
            else if (! oldType.getQualifier().isPerView() && newType.isArray())
                arrayLimitCheck(loc, member->type->getFieldName(), newType.getOuterArraySize());
#ifdef NV_EXTENSIONS
            if (oldType.getQualifier().isPerView() && ! newType.getQualifier().isPerView())
                error(memberLoc, "missing perviewNV qualifier to redeclared block member", member->type->getFieldName().c_str(), "");
            else if (! oldType.getQualifier().isPerView() && newType.getQualifier().isPerView())
                error(memberLoc, "cannot add perviewNV qualifier to redeclared block member", member->type->getFieldName().c_str(), "");
            else if (newType.getQualifier().isPerView()) {
                if (oldType.getArraySizes()->getNumDims() != newType.getArraySizes()->getNumDims())
                    error(memberLoc, "cannot change arrayness of redeclared block member", member->type->getFieldName().c_str(), "");
                else if (! newType.isUnsizedArray() && newType.getOuterArraySize() != resources.maxMeshViewCountNV)
                    error(loc, "mesh view output array size must be gl_MaxMeshViewCountNV or implicitly sized", "[]", "");
                else if (newType.getArraySizes()->getNumDims() == 2) {
                    int innerDimSize = newType.getArraySizes()->getDimSize(1);
                    arrayLimitCheck(memberLoc, member->type->getFieldName(), innerDimSize);
                    oldType.getArraySizes()->setDimSize(1, innerDimSize);
                }
            }
            if (oldType.getQualifier().isPerPrimitive() && ! newType.getQualifier().isPerPrimitive())
                error(memberLoc, "missing perprimitiveNV qualifier to redeclared block member", member->type->getFieldName().c_str(), "");
            else if (! oldType.getQualifier().isPerPrimitive() && newType.getQualifier().isPerPrimitive())
                error(memberLoc, "cannot add perprimitiveNV qualifier to redeclared block member", member->type->getFieldName().c_str(), "");
#endif
            if (newType.getQualifier().isMemory())
                error(memberLoc, "cannot add memory qualifier to redeclared block member", member->type->getFieldName().c_str(), "");
            if (newType.getQualifier().hasNonXfbLayout())
                error(memberLoc, "cannot add non-XFB layout to redeclared block member", member->type->getFieldName().c_str(), "");
            if (newType.getQualifier().patch)
                error(memberLoc, "cannot add patch to redeclared block member", member->type->getFieldName().c_str(), "");
            if (newType.getQualifier().hasXfbBuffer() &&
                newType.getQualifier().layoutXfbBuffer != currentBlockQualifier.layoutXfbBuffer)
                error(memberLoc, "member cannot contradict block (or what block inherited from global)", "xfb_buffer", "");
            oldType.getQualifier().centroid = newType.getQualifier().centroid;
            oldType.getQualifier().sample = newType.getQualifier().sample;
            oldType.getQualifier().invariant = newType.getQualifier().invariant;
            oldType.getQualifier().noContraction = newType.getQualifier().noContraction;
            oldType.getQualifier().smooth = newType.getQualifier().smooth;
            oldType.getQualifier().flat = newType.getQualifier().flat;
            oldType.getQualifier().nopersp = newType.getQualifier().nopersp;
            oldType.getQualifier().layoutXfbOffset = newType.getQualifier().layoutXfbOffset;
            oldType.getQualifier().layoutXfbBuffer = newType.getQualifier().layoutXfbBuffer;
            oldType.getQualifier().layoutXfbStride = newType.getQualifier().layoutXfbStride;
            if (oldType.getQualifier().layoutXfbOffset != TQualifier::layoutXfbBufferEnd) {
                // if any member as an xfb_offset, then the block's xfb_buffer inherents current xfb_buffer,
                // and for xfb processing, the member needs it as well, along with xfb_stride
                type.getQualifier().layoutXfbBuffer = currentBlockQualifier.layoutXfbBuffer;
                oldType.getQualifier().layoutXfbBuffer = currentBlockQualifier.layoutXfbBuffer;
            }
            if (oldType.isUnsizedArray() && newType.isSizedArray())
                oldType.changeOuterArraySize(newType.getOuterArraySize());

            //  check and process the member's type, which will include managing xfb information
            layoutTypeCheck(loc, oldType);

            // go to next member
            ++member;
        } else {
            // For missing members of anonymous blocks that have been redeclared,
            // hide the original (shared) declaration.
            // Instance-named blocks can just have the member removed.
            if (instanceName)
                member = type.getWritableStruct()->erase(member);
            else {
                member->type->hideMember();
                ++member;
            }
        }
    }

    if (numOriginalMembersFound < newTypeList.size())
        error(loc, "block redeclaration has extra members", blockName.c_str(), "");
    if (type.isArray() != (arraySizes != nullptr) ||
        (type.isArray() && arraySizes != nullptr && type.getArraySizes()->getNumDims() != arraySizes->getNumDims()))
        error(loc, "cannot change arrayness of redeclared block", blockName.c_str(), "");
    else if (type.isArray()) {
        // At this point, we know both are arrays and both have the same number of dimensions.

        // It is okay for a built-in block redeclaration to be unsized, and keep the size of the
        // original block declaration.
        if (!arraySizes->isSized() && type.isSizedArray())
            arraySizes->changeOuterSize(type.getOuterArraySize());

        // And, okay to be giving a size to the array, by the redeclaration
        if (!type.isSizedArray() && arraySizes->isSized())
            type.changeOuterArraySize(arraySizes->getOuterSize());

        // Now, they must match in all dimensions.
        if (type.isSizedArray() && *type.getArraySizes() != *arraySizes)
            error(loc, "cannot change array size of redeclared block", blockName.c_str(), "");
    }

    symbolTable.insert(*block);

    // Check for general layout qualifier errors
    layoutObjectCheck(loc, *block);

    // Tracking for implicit sizing of array
    if (isIoResizeArray(block->getType())) {
        ioArraySymbolResizeList.push_back(block);
        checkIoArraysConsistency(loc, true, block->getType().getQualifier().isPerPrimitive());
    } else if (block->getType().isArray())
        fixIoArraySize(loc, block->getWritableType());

    // Save it in the AST for linker use.
    trackLinkage(*block);
}

void TParseContext::paramCheckFixStorage(const TSourceLoc& loc, const TStorageQualifier& qualifier, TType& type)
{
    switch (qualifier) {
    case EvqConst:
    case EvqConstReadOnly:
        type.getQualifier().storage = EvqConstReadOnly;
        break;
    case EvqIn:
    case EvqOut:
    case EvqInOut:
        type.getQualifier().storage = qualifier;
        break;
    case EvqGlobal:
    case EvqTemporary:
        type.getQualifier().storage = EvqIn;
        break;
    default:
        type.getQualifier().storage = EvqIn;
        error(loc, "storage qualifier not allowed on function parameter", GetStorageQualifierString(qualifier), "");
        break;
    }
}

void TParseContext::paramCheckFix(const TSourceLoc& loc, const TQualifier& qualifier, TType& type)
{
    if (qualifier.isMemory()) {
        type.getQualifier().volatil   = qualifier.volatil;
        type.getQualifier().coherent  = qualifier.coherent;
        type.getQualifier().devicecoherent  = qualifier.devicecoherent ;
        type.getQualifier().queuefamilycoherent  = qualifier.queuefamilycoherent;
        type.getQualifier().workgroupcoherent  = qualifier.workgroupcoherent;
        type.getQualifier().subgroupcoherent  = qualifier.subgroupcoherent;
        type.getQualifier().nonprivate = qualifier.nonprivate;
        type.getQualifier().readonly  = qualifier.readonly;
        type.getQualifier().writeonly = qualifier.writeonly;
        type.getQualifier().restrict  = qualifier.restrict;
    }

    if (qualifier.isAuxiliary() ||
        qualifier.isInterpolation())
        error(loc, "cannot use auxiliary or interpolation qualifiers on a function parameter", "", "");
    if (qualifier.hasLayout())
        error(loc, "cannot use layout qualifiers on a function parameter", "", "");
    if (qualifier.invariant)
        error(loc, "cannot use invariant qualifier on a function parameter", "", "");
    if (qualifier.noContraction) {
        if (qualifier.isParamOutput())
            type.getQualifier().noContraction = true;
        else
            warn(loc, "qualifier has no effect on non-output parameters", "precise", "");
    }
    if (qualifier.isNonUniform())
        type.getQualifier().nonUniform = qualifier.nonUniform;

    paramCheckFixStorage(loc, qualifier.storage, type);
}

void TParseContext::nestedBlockCheck(const TSourceLoc& loc)
{
    if (structNestingLevel > 0)
        error(loc, "cannot nest a block definition inside a structure or block", "", "");
    ++structNestingLevel;
}

void TParseContext::nestedStructCheck(const TSourceLoc& loc)
{
    if (structNestingLevel > 0)
        error(loc, "cannot nest a structure definition inside a structure or block", "", "");
    ++structNestingLevel;
}

void TParseContext::arrayObjectCheck(const TSourceLoc& loc, const TType& type, const char* op)
{
    // Some versions don't allow comparing arrays or structures containing arrays
    if (type.containsArray()) {
        profileRequires(loc, ENoProfile, 120, E_GL_3DL_array_objects, op);
        profileRequires(loc, EEsProfile, 300, nullptr, op);
    }
}

void TParseContext::opaqueCheck(const TSourceLoc& loc, const TType& type, const char* op)
{
    if (containsFieldWithBasicType(type, EbtSampler))
        error(loc, "can't use with samplers or structs containing samplers", op, "");
}

void TParseContext::storage16BitAssignmentCheck(const TSourceLoc& loc, const TType& type, const char* op)
{
    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtFloat16))
        requireFloat16Arithmetic(loc, op, "can't use with structs containing float16");

    if (type.isArray() && type.getBasicType() == EbtFloat16)
        requireFloat16Arithmetic(loc, op, "can't use with arrays containing float16");

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtInt16))
        requireInt16Arithmetic(loc, op, "can't use with structs containing int16");

    if (type.isArray() && type.getBasicType() == EbtInt16)
        requireInt16Arithmetic(loc, op, "can't use with arrays containing int16");

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtUint16))
        requireInt16Arithmetic(loc, op, "can't use with structs containing uint16");

    if (type.isArray() && type.getBasicType() == EbtUint16)
        requireInt16Arithmetic(loc, op, "can't use with arrays containing uint16");

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtInt8))
        requireInt8Arithmetic(loc, op, "can't use with structs containing int8");

    if (type.isArray() && type.getBasicType() == EbtInt8)
        requireInt8Arithmetic(loc, op, "can't use with arrays containing int8");

    if (type.getBasicType() == EbtStruct && containsFieldWithBasicType(type, EbtUint8))
        requireInt8Arithmetic(loc, op, "can't use with structs containing uint8");

    if (type.isArray() && type.getBasicType() == EbtUint8)
        requireInt8Arithmetic(loc, op, "can't use with arrays containing uint8");
}

void TParseContext::specializationCheck(const TSourceLoc& loc, const TType& type, const char* op)
{
    if (type.containsSpecializationSize())
        error(loc, "can't use with types containing arrays sized with a specialization constant", op, "");
}

void TParseContext::structTypeCheck(const TSourceLoc& /*loc*/, TPublicType& publicType)
{
    const TTypeList& typeList = *publicType.userDef->getStruct();

    // fix and check for member storage qualifiers and types that don't belong within a structure
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier& memberQualifier = typeList[member].type->getQualifier();
        const TSourceLoc& memberLoc = typeList[member].loc;
        if (memberQualifier.isAuxiliary() ||
            memberQualifier.isInterpolation() ||
            (memberQualifier.storage != EvqTemporary && memberQualifier.storage != EvqGlobal))
            error(memberLoc, "cannot use storage or interpolation qualifiers on structure members", typeList[member].type->getFieldName().c_str(), "");
        if (memberQualifier.isMemory())
            error(memberLoc, "cannot use memory qualifiers on structure members", typeList[member].type->getFieldName().c_str(), "");
        if (memberQualifier.hasLayout()) {
            error(memberLoc, "cannot use layout qualifiers on structure members", typeList[member].type->getFieldName().c_str(), "");
            memberQualifier.clearLayout();
        }
        if (memberQualifier.invariant)
            error(memberLoc, "cannot use invariant qualifier on structure members", typeList[member].type->getFieldName().c_str(), "");
    }
}

//
// See if this loop satisfies the limitations for ES 2.0 (version 100) for loops in Appendex A:
//
// "The loop index has type int or float.
//
// "The for statement has the form:
//     for ( init-declaration ; condition ; expression )
//     init-declaration has the form: type-specifier identifier = constant-expression
//     condition has the form:  loop-index relational_operator constant-expression
//         where relational_operator is one of: > >= < <= == or !=
//     expression [sic] has one of the following forms:
//         loop-index++
//         loop-index--
//         loop-index += constant-expression
//         loop-index -= constant-expression
//
// The body is handled in an AST traversal.
//
void TParseContext::inductiveLoopCheck(const TSourceLoc& loc, TIntermNode* init, TIntermLoop* loop)
{
    // loop index init must exist and be a declaration, which shows up in the AST as an aggregate of size 1 of the declaration
    bool badInit = false;
    if (! init || ! init->getAsAggregate() || init->getAsAggregate()->getSequence().size() != 1)
        badInit = true;
    TIntermBinary* binaryInit = 0;
    if (! badInit) {
        // get the declaration assignment
        binaryInit = init->getAsAggregate()->getSequence()[0]->getAsBinaryNode();
        if (! binaryInit)
            badInit = true;
    }
    if (badInit) {
        error(loc, "inductive-loop init-declaration requires the form \"type-specifier loop-index = constant-expression\"", "limitations", "");
        return;
    }

    // loop index must be type int or float
    if (! binaryInit->getType().isScalar() || (binaryInit->getBasicType() != EbtInt && binaryInit->getBasicType() != EbtFloat)) {
        error(loc, "inductive loop requires a scalar 'int' or 'float' loop index", "limitations", "");
        return;
    }

    // init is the form "loop-index = constant"
    if (binaryInit->getOp() != EOpAssign || ! binaryInit->getLeft()->getAsSymbolNode() || ! binaryInit->getRight()->getAsConstantUnion()) {
        error(loc, "inductive-loop init-declaration requires the form \"type-specifier loop-index = constant-expression\"", "limitations", "");
        return;
    }

    // get the unique id of the loop index
    int loopIndex = binaryInit->getLeft()->getAsSymbolNode()->getId();
    inductiveLoopIds.insert(loopIndex);

    // condition's form must be "loop-index relational-operator constant-expression"
    bool badCond = ! loop->getTest();
    if (! badCond) {
        TIntermBinary* binaryCond = loop->getTest()->getAsBinaryNode();
        badCond = ! binaryCond;
        if (! badCond) {
            switch (binaryCond->getOp()) {
            case EOpGreaterThan:
            case EOpGreaterThanEqual:
            case EOpLessThan:
            case EOpLessThanEqual:
            case EOpEqual:
            case EOpNotEqual:
                break;
            default:
                badCond = true;
            }
        }
        if (binaryCond && (! binaryCond->getLeft()->getAsSymbolNode() ||
                           binaryCond->getLeft()->getAsSymbolNode()->getId() != loopIndex ||
                           ! binaryCond->getRight()->getAsConstantUnion()))
            badCond = true;
    }
    if (badCond) {
        error(loc, "inductive-loop condition requires the form \"loop-index <comparison-op> constant-expression\"", "limitations", "");
        return;
    }

    // loop-index++
    // loop-index--
    // loop-index += constant-expression
    // loop-index -= constant-expression
    bool badTerminal = ! loop->getTerminal();
    if (! badTerminal) {
        TIntermUnary* unaryTerminal = loop->getTerminal()->getAsUnaryNode();
        TIntermBinary* binaryTerminal = loop->getTerminal()->getAsBinaryNode();
        if (unaryTerminal || binaryTerminal) {
            switch(loop->getTerminal()->getAsOperator()->getOp()) {
            case EOpPostDecrement:
            case EOpPostIncrement:
            case EOpAddAssign:
            case EOpSubAssign:
                break;
            default:
                badTerminal = true;
            }
        } else
            badTerminal = true;
        if (binaryTerminal && (! binaryTerminal->getLeft()->getAsSymbolNode() ||
                               binaryTerminal->getLeft()->getAsSymbolNode()->getId() != loopIndex ||
                               ! binaryTerminal->getRight()->getAsConstantUnion()))
            badTerminal = true;
        if (unaryTerminal && (! unaryTerminal->getOperand()->getAsSymbolNode() ||
                              unaryTerminal->getOperand()->getAsSymbolNode()->getId() != loopIndex))
            badTerminal = true;
    }
    if (badTerminal) {
        error(loc, "inductive-loop termination requires the form \"loop-index++, loop-index--, loop-index += constant-expression, or loop-index -= constant-expression\"", "limitations", "");
        return;
    }

    // the body
    inductiveLoopBodyCheck(loop->getBody(), loopIndex, symbolTable);
}

// Do limit checks for built-in arrays.
void TParseContext::arrayLimitCheck(const TSourceLoc& loc, const TString& identifier, int size)
{
    if (identifier.compare("gl_TexCoord") == 0)
        limitCheck(loc, size, "gl_MaxTextureCoords", "gl_TexCoord array size");
    else if (identifier.compare("gl_ClipDistance") == 0)
        limitCheck(loc, size, "gl_MaxClipDistances", "gl_ClipDistance array size");
    else if (identifier.compare("gl_CullDistance") == 0)
        limitCheck(loc, size, "gl_MaxCullDistances", "gl_CullDistance array size");
#ifdef NV_EXTENSIONS
    else if (identifier.compare("gl_ClipDistancePerViewNV") == 0)
        limitCheck(loc, size, "gl_MaxClipDistances", "gl_ClipDistancePerViewNV array size");
    else if (identifier.compare("gl_CullDistancePerViewNV") == 0)
        limitCheck(loc, size, "gl_MaxCullDistances", "gl_CullDistancePerViewNV array size");
#endif
}

// See if the provided value is less than or equal to the symbol indicated by limit,
// which should be a constant in the symbol table.
void TParseContext::limitCheck(const TSourceLoc& loc, int value, const char* limit, const char* feature)
{
    TSymbol* symbol = symbolTable.find(limit);
    assert(symbol->getAsVariable());
    const TConstUnionArray& constArray = symbol->getAsVariable()->getConstArray();
    assert(! constArray.empty());
    if (value > constArray[0].getIConst())
        error(loc, "must be less than or equal to", feature, "%s (%d)", limit, constArray[0].getIConst());
}

//
// Do any additional error checking, etc., once we know the parsing is done.
//
void TParseContext::finish()
{
    TParseContextBase::finish();

    if (parsingBuiltins)
        return;

    // Check on array indexes for ES 2.0 (version 100) limitations.
    for (size_t i = 0; i < needsIndexLimitationChecking.size(); ++i)
        constantIndexExpressionCheck(needsIndexLimitationChecking[i]);

    // Check for stages that are enabled by extension.
    // Can't do this at the beginning, it is chicken and egg to add a stage by
    // extension.
    // Stage-specific features were correctly tested for already, this is just
    // about the stage itself.
    switch (language) {
    case EShLangGeometry:
        if (profile == EEsProfile && version == 310)
            requireExtensions(getCurrentLoc(), Num_AEP_geometry_shader, AEP_geometry_shader, "geometry shaders");
        break;
    case EShLangTessControl:
    case EShLangTessEvaluation:
        if (profile == EEsProfile && version == 310)
            requireExtensions(getCurrentLoc(), Num_AEP_tessellation_shader, AEP_tessellation_shader, "tessellation shaders");
        else if (profile != EEsProfile && version < 400)
            requireExtensions(getCurrentLoc(), 1, &E_GL_ARB_tessellation_shader, "tessellation shaders");
        break;
    case EShLangCompute:
        if (profile != EEsProfile && version < 430)
            requireExtensions(getCurrentLoc(), 1, &E_GL_ARB_compute_shader, "compute shaders");
        break;
#ifdef NV_EXTENSIONS
    case EShLangTaskNV:
    case EShLangMeshNV:
        requireExtensions(getCurrentLoc(), 1, &E_GL_NV_mesh_shader, "mesh shaders");
        break;
#endif
    default:
        break;
    }

#ifdef NV_EXTENSIONS
    // Set default outputs for GL_NV_geometry_shader_passthrough
    if (language == EShLangGeometry && extensionTurnedOn(E_SPV_NV_geometry_shader_passthrough)) {
        if (intermediate.getOutputPrimitive() == ElgNone) {
            switch (intermediate.getInputPrimitive()) {
            case ElgPoints:      intermediate.setOutputPrimitive(ElgPoints);    break;
            case ElgLines:       intermediate.setOutputPrimitive(ElgLineStrip); break;
            case ElgTriangles:   intermediate.setOutputPrimitive(ElgTriangleStrip); break;
            default: break;
            }
        }
        if (intermediate.getVertices() == TQualifier::layoutNotSet) {
            switch (intermediate.getInputPrimitive()) {
            case ElgPoints:      intermediate.setVertices(1); break;
            case ElgLines:       intermediate.setVertices(2); break;
            case ElgTriangles:   intermediate.setVertices(3); break;
            default: break;
            }
        }
    }
#endif
}

//
// Layout qualifier stuff.
//

// Put the id's layout qualification into the public type, for qualifiers not having a number set.
// This is before we know any type information for error checking.
void TParseContext::setLayoutQualifier(const TSourceLoc& loc, TPublicType& publicType, TString& id)
{
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);

    if (id == TQualifier::getLayoutMatrixString(ElmColumnMajor)) {
        publicType.qualifier.layoutMatrix = ElmColumnMajor;
        return;
    }
    if (id == TQualifier::getLayoutMatrixString(ElmRowMajor)) {
        publicType.qualifier.layoutMatrix = ElmRowMajor;
        return;
    }
    if (id == TQualifier::getLayoutPackingString(ElpPacked)) {
        if (spvVersion.spv != 0)
            spvRemoved(loc, "packed");
        publicType.qualifier.layoutPacking = ElpPacked;
        return;
    }
    if (id == TQualifier::getLayoutPackingString(ElpShared)) {
        if (spvVersion.spv != 0)
            spvRemoved(loc, "shared");
        publicType.qualifier.layoutPacking = ElpShared;
        return;
    }
    if (id == TQualifier::getLayoutPackingString(ElpStd140)) {
        publicType.qualifier.layoutPacking = ElpStd140;
        return;
    }
    if (id == TQualifier::getLayoutPackingString(ElpStd430)) {
        requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, "std430");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, nullptr, "std430");
        profileRequires(loc, EEsProfile, 310, nullptr, "std430");
        publicType.qualifier.layoutPacking = ElpStd430;
        return;
    }
    // TODO: compile-time performance: may need to stop doing linear searches
    for (TLayoutFormat format = (TLayoutFormat)(ElfNone + 1); format < ElfCount; format = (TLayoutFormat)(format + 1)) {
        if (id == TQualifier::getLayoutFormatString(format)) {
            if ((format > ElfEsFloatGuard && format < ElfFloatGuard) ||
                (format > ElfEsIntGuard && format < ElfIntGuard) ||
                (format > ElfEsUintGuard && format < ElfCount))
                requireProfile(loc, ENoProfile | ECoreProfile | ECompatibilityProfile, "image load-store format");
            profileRequires(loc, ENoProfile | ECoreProfile | ECompatibilityProfile, 420, E_GL_ARB_shader_image_load_store, "image load store");
            profileRequires(loc, EEsProfile, 310, E_GL_ARB_shader_image_load_store, "image load store");
            publicType.qualifier.layoutFormat = format;
            return;
        }
    }
    if (id == "push_constant") {
        requireVulkan(loc, "push_constant");
        publicType.qualifier.layoutPushConstant = true;
        return;
    }
    if (language == EShLangGeometry || language == EShLangTessEvaluation
#ifdef NV_EXTENSIONS
        || language == EShLangMeshNV
#endif
       ) {
        if (id == TQualifier::getGeometryString(ElgTriangles)) {
            publicType.shaderQualifiers.geometry = ElgTriangles;
            return;
        }
        if (language == EShLangGeometry
#ifdef NV_EXTENSIONS
            || language == EShLangMeshNV
#endif
           ) {
            if (id == TQualifier::getGeometryString(ElgPoints)) {
                publicType.shaderQualifiers.geometry = ElgPoints;
                return;
            }
            if (id == TQualifier::getGeometryString(ElgLines)) {
                publicType.shaderQualifiers.geometry = ElgLines;
                return;
            }
#ifdef NV_EXTENSIONS
            if (language == EShLangGeometry)
#endif
            {
                if (id == TQualifier::getGeometryString(ElgLineStrip)) {
                    publicType.shaderQualifiers.geometry = ElgLineStrip;
                    return;
                }
                if (id == TQualifier::getGeometryString(ElgLinesAdjacency)) {
                    publicType.shaderQualifiers.geometry = ElgLinesAdjacency;
                    return;
                }
                if (id == TQualifier::getGeometryString(ElgTrianglesAdjacency)) {
                    publicType.shaderQualifiers.geometry = ElgTrianglesAdjacency;
                    return;
                }
                if (id == TQualifier::getGeometryString(ElgTriangleStrip)) {
                    publicType.shaderQualifiers.geometry = ElgTriangleStrip;
                    return;
                }
#ifdef NV_EXTENSIONS
                if (id == "passthrough") {
                    requireExtensions(loc, 1, &E_SPV_NV_geometry_shader_passthrough, "geometry shader passthrough");
                    publicType.qualifier.layoutPassthrough = true;
                    intermediate.setGeoPassthroughEXT();
                    return;
                }
#endif
            }
        } else {
            assert(language == EShLangTessEvaluation);

            // input primitive
            if (id == TQualifier::getGeometryString(ElgTriangles)) {
                publicType.shaderQualifiers.geometry = ElgTriangles;
                return;
            }
            if (id == TQualifier::getGeometryString(ElgQuads)) {
                publicType.shaderQualifiers.geometry = ElgQuads;
                return;
            }
            if (id == TQualifier::getGeometryString(ElgIsolines)) {
                publicType.shaderQualifiers.geometry = ElgIsolines;
                return;
            }

            // vertex spacing
            if (id == TQualifier::getVertexSpacingString(EvsEqual)) {
                publicType.shaderQualifiers.spacing = EvsEqual;
                return;
            }
            if (id == TQualifier::getVertexSpacingString(EvsFractionalEven)) {
                publicType.shaderQualifiers.spacing = EvsFractionalEven;
                return;
            }
            if (id == TQualifier::getVertexSpacingString(EvsFractionalOdd)) {
                publicType.shaderQualifiers.spacing = EvsFractionalOdd;
                return;
            }

            // triangle order
            if (id == TQualifier::getVertexOrderString(EvoCw)) {
                publicType.shaderQualifiers.order = EvoCw;
                return;
            }
            if (id == TQualifier::getVertexOrderString(EvoCcw)) {
                publicType.shaderQualifiers.order = EvoCcw;
                return;
            }

            // point mode
            if (id == "point_mode") {
                publicType.shaderQualifiers.pointMode = true;
                return;
            }
        }
    }
    if (language == EShLangFragment) {
        if (id == "origin_upper_left") {
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, "origin_upper_left");
            publicType.shaderQualifiers.originUpperLeft = true;
            return;
        }
        if (id == "pixel_center_integer") {
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, "pixel_center_integer");
            publicType.shaderQualifiers.pixelCenterInteger = true;
            return;
        }
        if (id == "early_fragment_tests") {
            profileRequires(loc, ENoProfile | ECoreProfile | ECompatibilityProfile, 420, E_GL_ARB_shader_image_load_store, "early_fragment_tests");
            profileRequires(loc, EEsProfile, 310, nullptr, "early_fragment_tests");
            publicType.shaderQualifiers.earlyFragmentTests = true;
            return;
        }
        if (id == "post_depth_coverage") {
            requireExtensions(loc, Num_post_depth_coverageEXTs, post_depth_coverageEXTs, "post depth coverage");
            if (extensionTurnedOn(E_GL_ARB_post_depth_coverage)) {
                publicType.shaderQualifiers.earlyFragmentTests = true;
            }
            publicType.shaderQualifiers.postDepthCoverage = true;
            return;
        }
        for (TLayoutDepth depth = (TLayoutDepth)(EldNone + 1); depth < EldCount; depth = (TLayoutDepth)(depth+1)) {
            if (id == TQualifier::getLayoutDepthString(depth)) {
                requireProfile(loc, ECoreProfile | ECompatibilityProfile, "depth layout qualifier");
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 420, nullptr, "depth layout qualifier");
                publicType.shaderQualifiers.layoutDepth = depth;
                return;
            }
        }
        if (id.compare(0, 13, "blend_support") == 0) {
            bool found = false;
            for (TBlendEquationShift be = (TBlendEquationShift)0; be < EBlendCount; be = (TBlendEquationShift)(be + 1)) {
                if (id == TQualifier::getBlendEquationString(be)) {
                    profileRequires(loc, EEsProfile, 320, E_GL_KHR_blend_equation_advanced, "blend equation");
                    profileRequires(loc, ~EEsProfile, 0, E_GL_KHR_blend_equation_advanced, "blend equation");
                    intermediate.addBlendEquation(be);
                    publicType.shaderQualifiers.blendEquation = true;
                    found = true;
                    break;
                }
            }
            if (! found)
                error(loc, "unknown blend equation", "blend_support", "");
            return;
        }
#ifdef NV_EXTENSIONS
        if (id == "override_coverage") {
            requireExtensions(loc, 1, &E_GL_NV_sample_mask_override_coverage, "sample mask override coverage");
            publicType.shaderQualifiers.layoutOverrideCoverage = true;
            return;
        }
    }
    if (language == EShLangVertex ||
        language == EShLangTessControl ||
        language == EShLangTessEvaluation ||
        language == EShLangGeometry ) {
        if (id == "viewport_relative") {
            requireExtensions(loc, 1, &E_GL_NV_viewport_array2, "view port array2");
            publicType.qualifier.layoutViewportRelative = true;
            return;
        }
    } else {
        if (language == EShLangRayGenNV || language == EShLangIntersectNV ||
        language == EShLangAnyHitNV || language == EShLangClosestHitNV ||
        language == EShLangMissNV || language == EShLangCallableNV) {
            if (id == "shaderrecordnvx") {
                publicType.qualifier.layoutShaderRecordNV = true;
                return;
            }
        }
    }
    if (language == EShLangCompute) {
        if (id.compare(0, 17, "derivative_group_") == 0) {
            requireExtensions(loc, 1, &E_GL_NV_compute_shader_derivatives, "compute shader derivatives");
            if (id == "derivative_group_quadsnv") {
                publicType.shaderQualifiers.layoutDerivativeGroupQuads = true;
                return;
            } else if (id == "derivative_group_linearnv") {
                publicType.shaderQualifiers.layoutDerivativeGroupLinear = true;
                return;
            }
        }
    }
#else
    }
#endif
    error(loc, "unrecognized layout identifier, or qualifier requires assignment (e.g., binding = 4)", id.c_str(), "");
}

// Put the id's layout qualifier value into the public type, for qualifiers having a number set.
// This is before we know any type information for error checking.
void TParseContext::setLayoutQualifier(const TSourceLoc& loc, TPublicType& publicType, TString& id, const TIntermTyped* node)
{
    const char* feature = "layout-id value";
    const char* nonLiteralFeature = "non-literal layout-id value";

    integerCheck(node, feature);
    const TIntermConstantUnion* constUnion = node->getAsConstantUnion();
    int value;
    if (constUnion) {
        value = constUnion->getConstArray()[0].getIConst();
        if (! constUnion->isLiteral()) {
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, nonLiteralFeature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, nonLiteralFeature);
        }
    } else {
        // grammar should have give out the error message
        value = 0;
    }

    if (value < 0) {
        error(loc, "cannot be negative", feature, "");
        return;
    }

    std::transform(id.begin(), id.end(), id.begin(), ::tolower);

    if (id == "offset") {
        // "offset" can be for either
        //  - uniform offsets
        //  - atomic_uint offsets
        const char* feature = "offset";
        if (spvVersion.spv == 0) {
            requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, feature);
            const char* exts[2] = { E_GL_ARB_enhanced_layouts, E_GL_ARB_shader_atomic_counters };
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 420, 2, exts, feature);
            profileRequires(loc, EEsProfile, 310, nullptr, feature);
        }
        publicType.qualifier.layoutOffset = value;
        return;
    } else if (id == "align") {
        const char* feature = "uniform buffer-member align";
        if (spvVersion.spv == 0) {
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, feature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, feature);
        }
        // "The specified alignment must be a power of 2, or a compile-time error results."
        if (! IsPow2(value))
            error(loc, "must be a power of 2", "align", "");
        else
            publicType.qualifier.layoutAlign = value;
        return;
    } else if (id == "location") {
        profileRequires(loc, EEsProfile, 300, nullptr, "location");
        const char* exts[2] = { E_GL_ARB_separate_shader_objects, E_GL_ARB_explicit_attrib_location };
        profileRequires(loc, ~EEsProfile, 330, 2, exts, "location");
        if ((unsigned int)value >= TQualifier::layoutLocationEnd)
            error(loc, "location is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutLocation = value;
        return;
    } else if (id == "set") {
        if ((unsigned int)value >= TQualifier::layoutSetEnd)
            error(loc, "set is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutSet = value;
        if (value != 0)
            requireVulkan(loc, "descriptor set");
        return;
    } else if (id == "binding") {
        profileRequires(loc, ~EEsProfile, 420, E_GL_ARB_shading_language_420pack, "binding");
        profileRequires(loc, EEsProfile, 310, nullptr, "binding");
        if ((unsigned int)value >= TQualifier::layoutBindingEnd)
            error(loc, "binding is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutBinding = value;
        return;
    } else if (id == "component") {
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "component");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, "component");
        if ((unsigned)value >= TQualifier::layoutComponentEnd)
            error(loc, "component is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutComponent = value;
        return;
    } else if (id.compare(0, 4, "xfb_") == 0) {
        // "Any shader making any static use (after preprocessing) of any of these
        // *xfb_* qualifiers will cause the shader to be in a transform feedback
        // capturing mode and hence responsible for describing the transform feedback
        // setup."
        intermediate.setXfbMode();
        const char* feature = "transform feedback qualifier";
        requireStage(loc, (EShLanguageMask)(EShLangVertexMask | EShLangGeometryMask | EShLangTessControlMask | EShLangTessEvaluationMask), feature);
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, feature);
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, feature);
        if (id == "xfb_buffer") {
            // "It is a compile-time error to specify an *xfb_buffer* that is greater than
            // the implementation-dependent constant gl_MaxTransformFeedbackBuffers."
            if (value >= resources.maxTransformFeedbackBuffers)
                error(loc, "buffer is too large:", id.c_str(), "gl_MaxTransformFeedbackBuffers is %d", resources.maxTransformFeedbackBuffers);
            if (value >= (int)TQualifier::layoutXfbBufferEnd)
                error(loc, "buffer is too large:", id.c_str(), "internal max is %d", TQualifier::layoutXfbBufferEnd-1);
            else
                publicType.qualifier.layoutXfbBuffer = value;
            return;
        } else if (id == "xfb_offset") {
            if (value >= (int)TQualifier::layoutXfbOffsetEnd)
                error(loc, "offset is too large:", id.c_str(), "internal max is %d", TQualifier::layoutXfbOffsetEnd-1);
            else
                publicType.qualifier.layoutXfbOffset = value;
            return;
        } else if (id == "xfb_stride") {
            // "The resulting stride (implicit or explicit), when divided by 4, must be less than or equal to the
            // implementation-dependent constant gl_MaxTransformFeedbackInterleavedComponents."
            if (value > 4 * resources.maxTransformFeedbackInterleavedComponents)
                error(loc, "1/4 stride is too large:", id.c_str(), "gl_MaxTransformFeedbackInterleavedComponents is %d", resources.maxTransformFeedbackInterleavedComponents);
            else if (value >= (int)TQualifier::layoutXfbStrideEnd)
                error(loc, "stride is too large:", id.c_str(), "internal max is %d", TQualifier::layoutXfbStrideEnd-1);
            if (value < (int)TQualifier::layoutXfbStrideEnd)
                publicType.qualifier.layoutXfbStride = value;
            return;
        }
    }

    if (id == "input_attachment_index") {
        requireVulkan(loc, "input_attachment_index");
        if (value >= (int)TQualifier::layoutAttachmentEnd)
            error(loc, "attachment index is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutAttachment = value;
        return;
    }
    if (id == "constant_id") {
        requireSpv(loc, "constant_id");
        if (value >= (int)TQualifier::layoutSpecConstantIdEnd) {
            error(loc, "specialization-constant id is too large", id.c_str(), "");
        } else {
            publicType.qualifier.layoutSpecConstantId = value;
            publicType.qualifier.specConstant = true;
            if (! intermediate.addUsedConstantId(value))
                error(loc, "specialization-constant id already used", id.c_str(), "");
        }
        return;
    }
    if (id == "num_views") {
        requireExtensions(loc, Num_OVR_multiview_EXTs, OVR_multiview_EXTs, "num_views");
        publicType.shaderQualifiers.numViews = value;
        return;
    }

#if NV_EXTENSIONS
    if (language == EShLangVertex ||
        language == EShLangTessControl ||
        language == EShLangTessEvaluation ||
        language == EShLangGeometry) {
        if (id == "secondary_view_offset") {
            requireExtensions(loc, 1, &E_GL_NV_stereo_view_rendering, "stereo view rendering");
            publicType.qualifier.layoutSecondaryViewportRelativeOffset = value;
            return;
        }
    }
#endif

    switch (language) {
    case EShLangVertex:
        break;

    case EShLangTessControl:
        if (id == "vertices") {
            if (value == 0)
                error(loc, "must be greater than 0", "vertices", "");
            else
                publicType.shaderQualifiers.vertices = value;
            return;
        }
        break;

    case EShLangTessEvaluation:
        break;

    case EShLangGeometry:
        if (id == "invocations") {
            profileRequires(loc, ECompatibilityProfile | ECoreProfile, 400, nullptr, "invocations");
            if (value == 0)
                error(loc, "must be at least 1", "invocations", "");
            else
                publicType.shaderQualifiers.invocations = value;
            return;
        }
        if (id == "max_vertices") {
            publicType.shaderQualifiers.vertices = value;
            if (value > resources.maxGeometryOutputVertices)
                error(loc, "too large, must be less than gl_MaxGeometryOutputVertices", "max_vertices", "");
            return;
        }
        if (id == "stream") {
            requireProfile(loc, ~EEsProfile, "selecting output stream");
            publicType.qualifier.layoutStream = value;
            if (value > 0)
                intermediate.setMultiStream();
            return;
        }
        break;

    case EShLangFragment:
        if (id == "index") {
            requireProfile(loc, ECompatibilityProfile | ECoreProfile, "index layout qualifier on fragment output");
            const char* exts[2] = { E_GL_ARB_separate_shader_objects, E_GL_ARB_explicit_attrib_location };
            profileRequires(loc, ECompatibilityProfile | ECoreProfile, 330, 2, exts, "index layout qualifier on fragment output");

            // "It is also a compile-time error if a fragment shader sets a layout index to less than 0 or greater than 1."
            if (value < 0 || value > 1) {
                value = 0;
                error(loc, "value must be 0 or 1", "index", "");
            }

            publicType.qualifier.layoutIndex = value;
            return;
        }
        break;

#ifdef NV_EXTENSIONS
    case EShLangMeshNV:
        if (id == "max_vertices") {
            requireExtensions(loc, 1, &E_GL_NV_mesh_shader, "max_vertices");
            publicType.shaderQualifiers.vertices = value;
            if (value > resources.maxMeshOutputVerticesNV)
                error(loc, "too large, must be less than gl_MaxMeshOutputVerticesNV", "max_vertices", "");
            return;
        }
        if (id == "max_primitives") {
            requireExtensions(loc, 1, &E_GL_NV_mesh_shader, "max_primitives");
            publicType.shaderQualifiers.primitives = value;
            if (value > resources.maxMeshOutputPrimitivesNV)
                error(loc, "too large, must be less than gl_MaxMeshOutputPrimitivesNV", "max_primitives", "");
            return;
        }
        // Fall through

    case EShLangTaskNV:
        // Fall through
#endif
    case EShLangCompute:
        if (id.compare(0, 11, "local_size_") == 0) {
#ifdef NV_EXTENSIONS
            if (language == EShLangMeshNV || language == EShLangTaskNV) {
                requireExtensions(loc, 1, &E_GL_NV_mesh_shader, "gl_WorkGroupSize");
            }
            else
#endif
            {
                profileRequires(loc, EEsProfile, 310, 0, "gl_WorkGroupSize");
                profileRequires(loc, ~EEsProfile, 430, E_GL_ARB_compute_shader, "gl_WorkGroupSize");
            }
            if (id.size() == 12 && value == 0) {
                error(loc, "must be at least 1", id.c_str(), "");
                return;
            }
            if (id == "local_size_x") {
                publicType.shaderQualifiers.localSize[0] = value;
                return;
            }
            if (id == "local_size_y") {
                publicType.shaderQualifiers.localSize[1] = value;
                return;
            }
            if (id == "local_size_z") {
                publicType.shaderQualifiers.localSize[2] = value;
                return;
            }
            if (spvVersion.spv != 0) {
                if (id == "local_size_x_id") {
                    publicType.shaderQualifiers.localSizeSpecId[0] = value;
                    return;
                }
                if (id == "local_size_y_id") {
                    publicType.shaderQualifiers.localSizeSpecId[1] = value;
                    return;
                }
                if (id == "local_size_z_id") {
                    publicType.shaderQualifiers.localSizeSpecId[2] = value;
                    return;
                }
            }
        }
        break;

    default:
        break;
    }

    error(loc, "there is no such layout identifier for this stage taking an assigned value", id.c_str(), "");
}

// Merge any layout qualifier information from src into dst, leaving everything else in dst alone
//
// "More than one layout qualifier may appear in a single declaration.
// Additionally, the same layout-qualifier-name can occur multiple times
// within a layout qualifier or across multiple layout qualifiers in the
// same declaration. When the same layout-qualifier-name occurs
// multiple times, in a single declaration, the last occurrence overrides
// the former occurrence(s).  Further, if such a layout-qualifier-name
// will effect subsequent declarations or other observable behavior, it
// is only the last occurrence that will have any effect, behaving as if
// the earlier occurrence(s) within the declaration are not present.
// This is also true for overriding layout-qualifier-names, where one
// overrides the other (e.g., row_major vs. column_major); only the last
// occurrence has any effect."
void TParseContext::mergeObjectLayoutQualifiers(TQualifier& dst, const TQualifier& src, bool inheritOnly)
{
    if (src.hasMatrix())
        dst.layoutMatrix = src.layoutMatrix;
    if (src.hasPacking())
        dst.layoutPacking = src.layoutPacking;

    if (src.hasStream())
        dst.layoutStream = src.layoutStream;

    if (src.hasFormat())
        dst.layoutFormat = src.layoutFormat;

    if (src.hasXfbBuffer())
        dst.layoutXfbBuffer = src.layoutXfbBuffer;

    if (src.hasAlign())
        dst.layoutAlign = src.layoutAlign;

    if (! inheritOnly) {
        if (src.hasLocation())
            dst.layoutLocation = src.layoutLocation;
        if (src.hasComponent())
            dst.layoutComponent = src.layoutComponent;
        if (src.hasIndex())
            dst.layoutIndex = src.layoutIndex;

        if (src.hasOffset())
            dst.layoutOffset = src.layoutOffset;

        if (src.hasSet())
            dst.layoutSet = src.layoutSet;
        if (src.layoutBinding != TQualifier::layoutBindingEnd)
            dst.layoutBinding = src.layoutBinding;

        if (src.hasXfbStride())
            dst.layoutXfbStride = src.layoutXfbStride;
        if (src.hasXfbOffset())
            dst.layoutXfbOffset = src.layoutXfbOffset;
        if (src.hasAttachment())
            dst.layoutAttachment = src.layoutAttachment;
        if (src.hasSpecConstantId())
            dst.layoutSpecConstantId = src.layoutSpecConstantId;

        if (src.layoutPushConstant)
            dst.layoutPushConstant = true;

#ifdef NV_EXTENSIONS
        if (src.layoutPassthrough)
            dst.layoutPassthrough = true;
        if (src.layoutViewportRelative)
            dst.layoutViewportRelative = true;
        if (src.layoutSecondaryViewportRelativeOffset != -2048)
            dst.layoutSecondaryViewportRelativeOffset = src.layoutSecondaryViewportRelativeOffset;
        if (src.layoutShaderRecordNV)
            dst.layoutShaderRecordNV = true;
        if (src.pervertexNV)
            dst.pervertexNV = true;
#endif
    }
}

// Do error layout error checking given a full variable/block declaration.
void TParseContext::layoutObjectCheck(const TSourceLoc& loc, const TSymbol& symbol)
{
    const TType& type = symbol.getType();
    const TQualifier& qualifier = type.getQualifier();

    // first, cross check WRT to just the type
    layoutTypeCheck(loc, type);

    // now, any remaining error checking based on the object itself

    if (qualifier.hasAnyLocation()) {
        switch (qualifier.storage) {
        case EvqUniform:
        case EvqBuffer:
            if (symbol.getAsVariable() == nullptr)
                error(loc, "can only be used on variable declaration", "location", "");
            break;
        default:
            break;
        }
    }

    // user-variable location check, which are required for SPIR-V in/out:
    //  - variables have it directly,
    //  - blocks have it on each member (already enforced), so check first one
    if (spvVersion.spv > 0 && !parsingBuiltins && qualifier.builtIn == EbvNone &&
        !qualifier.hasLocation() && !intermediate.getAutoMapLocations()) {

        switch (qualifier.storage) {
        case EvqVaryingIn:
        case EvqVaryingOut:
            if (!type.getQualifier().isTaskMemory() &&
                (type.getBasicType() != EbtBlock ||
                 (!(*type.getStruct())[0].type->getQualifier().hasLocation() &&
                   (*type.getStruct())[0].type->getQualifier().builtIn == EbvNone)))
                error(loc, "SPIR-V requires location for user input/output", "location", "");
            break;
        default:
            break;
        }
    }

    // Check packing and matrix
    if (qualifier.hasUniformLayout()) {
        switch (qualifier.storage) {
        case EvqUniform:
        case EvqBuffer:
            if (type.getBasicType() != EbtBlock) {
                if (qualifier.hasMatrix())
                    error(loc, "cannot specify matrix layout on a variable declaration", "layout", "");
                if (qualifier.hasPacking())
                    error(loc, "cannot specify packing on a variable declaration", "layout", "");
                // "The offset qualifier can only be used on block members of blocks..."
                if (qualifier.hasOffset() && type.getBasicType() != EbtAtomicUint)
                    error(loc, "cannot specify on a variable declaration", "offset", "");
                // "The align qualifier can only be used on blocks or block members..."
                if (qualifier.hasAlign())
                    error(loc, "cannot specify on a variable declaration", "align", "");
                if (qualifier.layoutPushConstant)
                    error(loc, "can only specify on a uniform block", "push_constant", "");
#ifdef NV_EXTENSIONS
                if (qualifier.layoutShaderRecordNV)
                    error(loc, "can only specify on a buffer block", "shaderRecordNVX", "");
#endif
            }
            break;
        default:
            // these were already filtered by layoutTypeCheck() (or its callees)
            break;
        }
    }
}

// "For some blocks declared as arrays, the location can only be applied at the block level:
// When a block is declared as an array where additional locations are needed for each member
// for each block array element, it is a compile-time error to specify locations on the block
// members.  That is, when locations would be under specified by applying them on block members,
// they are not allowed on block members.  For arrayed interfaces (those generally having an
// extra level of arrayness due to interface expansion), the outer array is stripped before
// applying this rule."
void TParseContext::layoutMemberLocationArrayCheck(const TSourceLoc& loc, bool memberWithLocation,
    TArraySizes* arraySizes)
{
    if (memberWithLocation && arraySizes != nullptr) {
        if (arraySizes->getNumDims() > (currentBlockQualifier.isArrayedIo(language) ? 1 : 0))
            error(loc, "cannot use in a block array where new locations are needed for each block element",
                       "location", "");
    }
}

// Do layout error checking with respect to a type.
void TParseContext::layoutTypeCheck(const TSourceLoc& loc, const TType& type)
{
    const TQualifier& qualifier = type.getQualifier();

    // first, intra-layout qualifier-only error checking
    layoutQualifierCheck(loc, qualifier);

    // now, error checking combining type and qualifier

    if (qualifier.hasAnyLocation()) {
        if (qualifier.hasLocation()) {
            if (qualifier.storage == EvqVaryingOut && language == EShLangFragment) {
                if (qualifier.layoutLocation >= (unsigned int)resources.maxDrawBuffers)
                    error(loc, "too large for fragment output", "location", "");
            }
        }
        if (qualifier.hasComponent()) {
            // "It is a compile-time error if this sequence of components gets larger than 3."
            if (qualifier.layoutComponent + type.getVectorSize() * (type.getBasicType() == EbtDouble ? 2 : 1) > 4)
                error(loc, "type overflows the available 4 components", "component", "");

            // "It is a compile-time error to apply the component qualifier to a matrix, a structure, a block, or an array containing any of these."
            if (type.isMatrix() || type.getBasicType() == EbtBlock || type.getBasicType() == EbtStruct)
                error(loc, "cannot apply to a matrix, structure, or block", "component", "");

            // " It is a compile-time error to use component 1 or 3 as the beginning of a double or dvec2."
            if (type.getBasicType() == EbtDouble)
                if (qualifier.layoutComponent & 1)
                    error(loc, "doubles cannot start on an odd-numbered component", "component", "");
        }

        switch (qualifier.storage) {
        case EvqVaryingIn:
        case EvqVaryingOut:
            if (type.getBasicType() == EbtBlock)
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, "location qualifier on in/out block");
#ifdef NV_EXTENSIONS
            if (type.getQualifier().isTaskMemory())
                error(loc, "cannot apply to taskNV in/out blocks", "location", "");
#endif
            break;
        case EvqUniform:
        case EvqBuffer:
            if (type.getBasicType() == EbtBlock)
                error(loc, "cannot apply to uniform or buffer block", "location", "");
            break;
#ifdef NV_EXTENSIONS
        case EvqPayloadNV:
        case EvqPayloadInNV:
        case EvqHitAttrNV:
            break;
#endif
        default:
            error(loc, "can only apply to uniform, buffer, in, or out storage qualifiers", "location", "");
            break;
        }

        bool typeCollision;
        int repeated = intermediate.addUsedLocation(qualifier, type, typeCollision);
        if (repeated >= 0 && ! typeCollision)
            error(loc, "overlapping use of location", "location", "%d", repeated);
        // "fragment-shader outputs ... if two variables are placed within the same
        // location, they must have the same underlying type (floating-point or integer)"
        if (typeCollision && language == EShLangFragment && qualifier.isPipeOutput())
            error(loc, "fragment outputs sharing the same location must be the same basic type", "location", "%d", repeated);
    }

    if (qualifier.hasXfbOffset() && qualifier.hasXfbBuffer()) {
        int repeated = intermediate.addXfbBufferOffset(type);
        if (repeated >= 0)
            error(loc, "overlapping offsets at", "xfb_offset", "offset %d in buffer %d", repeated, qualifier.layoutXfbBuffer);

        // "The offset must be a multiple of the size of the first component of the first
        // qualified variable or block member, or a compile-time error results. Further, if applied to an aggregate
        // containing a double, the offset must also be a multiple of 8..."
        if (type.containsBasicType(EbtDouble) && ! IsMultipleOfPow2(qualifier.layoutXfbOffset, 8))
            error(loc, "type contains double; xfb_offset must be a multiple of 8", "xfb_offset", "");
        // ..., if applied to an aggregate containing a float16_t, the offset must also be a multiple of 2..."
        else if (type.containsBasicType(EbtFloat16) && !IsMultipleOfPow2(qualifier.layoutXfbOffset, 2))
            error(loc, "type contains half float; xfb_offset must be a multiple of 2", "xfb_offset", "");
        else if (! IsMultipleOfPow2(qualifier.layoutXfbOffset, 4))
            error(loc, "must be a multiple of size of first component", "xfb_offset", "");
    }

    if (qualifier.hasXfbStride() && qualifier.hasXfbBuffer()) {
        if (! intermediate.setXfbBufferStride(qualifier.layoutXfbBuffer, qualifier.layoutXfbStride))
            error(loc, "all stride settings must match for xfb buffer", "xfb_stride", "%d", qualifier.layoutXfbBuffer);
    }

    if (qualifier.hasBinding()) {
        // Binding checking, from the spec:
        //
        // "If the binding point for any uniform or shader storage block instance is less than zero, or greater than or
        // equal to the implementation-dependent maximum number of uniform buffer bindings, a compile-time
        // error will occur. When the binding identifier is used with a uniform or shader storage block instanced as
        // an array of size N, all elements of the array from binding through binding + N - 1 must be within this
        // range."
        //
        if (! type.isOpaque() && type.getBasicType() != EbtBlock)
            error(loc, "requires block, or sampler/image, or atomic-counter type", "binding", "");
        if (type.getBasicType() == EbtSampler) {
            int lastBinding = qualifier.layoutBinding;
            if (type.isArray()) {
                if (spvVersion.vulkan > 0)
                    lastBinding += 1;
                else {
                    if (type.isSizedArray())
                        lastBinding += type.getCumulativeArraySize();
                    else {
                        lastBinding += 1;
                        if (spvVersion.vulkan == 0)
                            warn(loc, "assuming binding count of one for compile-time checking of binding numbers for unsized array", "[]", "");
                    }
                }
            }
            if (spvVersion.vulkan == 0 && lastBinding >= resources.maxCombinedTextureImageUnits)
                error(loc, "sampler binding not less than gl_MaxCombinedTextureImageUnits", "binding", type.isArray() ? "(using array)" : "");
        }
        if (type.getBasicType() == EbtAtomicUint) {
            if (qualifier.layoutBinding >= (unsigned int)resources.maxAtomicCounterBindings) {
                error(loc, "atomic_uint binding is too large; see gl_MaxAtomicCounterBindings", "binding", "");
                return;
            }
        }
    } else if (!intermediate.getAutoMapBindings()) {
        // some types require bindings

        // atomic_uint
        if (type.getBasicType() == EbtAtomicUint)
            error(loc, "layout(binding=X) is required", "atomic_uint", "");

        // SPIR-V
        if (spvVersion.spv > 0) {
            if (qualifier.isUniformOrBuffer()) {
                if (type.getBasicType() == EbtBlock && !qualifier.layoutPushConstant &&
#ifdef NV_EXTENSIONS
                       !qualifier.layoutShaderRecordNV &&
#endif
                       !qualifier.layoutAttachment)
                    error(loc, "uniform/buffer blocks require layout(binding=X)", "binding", "");
                else if (spvVersion.vulkan > 0 && type.getBasicType() == EbtSampler)
                    error(loc, "sampler/texture/image requires layout(binding=X)", "binding", "");
            }
        }
    }

    // some things can't have arrays of arrays
    if (type.isArrayOfArrays()) {
        if (spvVersion.vulkan > 0) {
            if (type.isOpaque() || (type.getQualifier().isUniformOrBuffer() && type.getBasicType() == EbtBlock))
                warn(loc, "Generating SPIR-V array-of-arrays, but Vulkan only supports single array level for this resource", "[][]", "");
        }
    }

    // "The offset qualifier can only be used on block members of blocks..."
    if (qualifier.hasOffset()) {
        if (type.getBasicType() == EbtBlock)
            error(loc, "only applies to block members, not blocks", "offset", "");
    }

    // Image format
    if (qualifier.hasFormat()) {
        if (! type.isImage())
            error(loc, "only apply to images", TQualifier::getLayoutFormatString(qualifier.layoutFormat), "");
        else {
            if (type.getSampler().type == EbtFloat && qualifier.layoutFormat > ElfFloatGuard)
                error(loc, "does not apply to floating point images", TQualifier::getLayoutFormatString(qualifier.layoutFormat), "");
            if (type.getSampler().type == EbtInt && (qualifier.layoutFormat < ElfFloatGuard || qualifier.layoutFormat > ElfIntGuard))
                error(loc, "does not apply to signed integer images", TQualifier::getLayoutFormatString(qualifier.layoutFormat), "");
            if (type.getSampler().type == EbtUint && qualifier.layoutFormat < ElfIntGuard)
                error(loc, "does not apply to unsigned integer images", TQualifier::getLayoutFormatString(qualifier.layoutFormat), "");

            if (profile == EEsProfile) {
                // "Except for image variables qualified with the format qualifiers r32f, r32i, and r32ui, image variables must
                // specify either memory qualifier readonly or the memory qualifier writeonly."
                if (! (qualifier.layoutFormat == ElfR32f || qualifier.layoutFormat == ElfR32i || qualifier.layoutFormat == ElfR32ui)) {
                    if (! qualifier.readonly && ! qualifier.writeonly)
                        error(loc, "format requires readonly or writeonly memory qualifier", TQualifier::getLayoutFormatString(qualifier.layoutFormat), "");
                }
            }
        }
    } else if (type.isImage() && ! qualifier.writeonly) {
        const char *explanation = "image variables not declared 'writeonly' and without a format layout qualifier";
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, explanation);
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 0, E_GL_EXT_shader_image_load_formatted, explanation);
    }

    if (qualifier.layoutPushConstant && type.getBasicType() != EbtBlock)
        error(loc, "can only be used with a block", "push_constant", "");

#ifdef NV_EXTENSIONS
    if (qualifier.layoutShaderRecordNV && type.getBasicType() != EbtBlock)
        error(loc, "can only be used with a block", "shaderRecordNVX", "");
#endif

    // input attachment
    if (type.isSubpass()) {
        if (! qualifier.hasAttachment())
            error(loc, "requires an input_attachment_index layout qualifier", "subpass", "");
    } else {
        if (qualifier.hasAttachment())
            error(loc, "can only be used with a subpass", "input_attachment_index", "");
    }

    // specialization-constant id
    if (qualifier.hasSpecConstantId()) {
        if (type.getQualifier().storage != EvqConst)
            error(loc, "can only be applied to 'const'-qualified scalar", "constant_id", "");
        if (! type.isScalar())
            error(loc, "can only be applied to a scalar", "constant_id", "");
        switch (type.getBasicType())
        {
        case EbtInt8:
        case EbtUint8:
        case EbtInt16:
        case EbtUint16:
        case EbtInt:
        case EbtUint:
        case EbtInt64:
        case EbtUint64:
        case EbtBool:
        case EbtFloat:
        case EbtDouble:
        case EbtFloat16:
            break;
        default:
            error(loc, "cannot be applied to this type", "constant_id", "");
            break;
        }
    }
}

// Do layout error checking that can be done within a layout qualifier proper, not needing to know
// if there are blocks, atomic counters, variables, etc.
void TParseContext::layoutQualifierCheck(const TSourceLoc& loc, const TQualifier& qualifier)
{
    if (qualifier.storage == EvqShared && qualifier.hasLayout())
        error(loc, "cannot apply layout qualifiers to a shared variable", "shared", "");

    // "It is a compile-time error to use *component* without also specifying the location qualifier (order does not matter)."
    if (qualifier.hasComponent() && ! qualifier.hasLocation())
        error(loc, "must specify 'location' to use 'component'", "component", "");

    if (qualifier.hasAnyLocation()) {

        // "As with input layout qualifiers, all shaders except compute shaders
        // allow *location* layout qualifiers on output variable declarations,
        // output block declarations, and output block member declarations."

        switch (qualifier.storage) {
        case EvqVaryingIn:
        {
            const char* feature = "location qualifier on input";
            if (profile == EEsProfile && version < 310)
                requireStage(loc, EShLangVertex, feature);
            else
                requireStage(loc, (EShLanguageMask)~EShLangComputeMask, feature);
            if (language == EShLangVertex) {
                const char* exts[2] = { E_GL_ARB_separate_shader_objects, E_GL_ARB_explicit_attrib_location };
                profileRequires(loc, ~EEsProfile, 330, 2, exts, feature);
                profileRequires(loc, EEsProfile, 300, nullptr, feature);
            } else {
                profileRequires(loc, ~EEsProfile, 410, E_GL_ARB_separate_shader_objects, feature);
                profileRequires(loc, EEsProfile, 310, nullptr, feature);
            }
            break;
        }
        case EvqVaryingOut:
        {
            const char* feature = "location qualifier on output";
            if (profile == EEsProfile && version < 310)
                requireStage(loc, EShLangFragment, feature);
            else
                requireStage(loc, (EShLanguageMask)~EShLangComputeMask, feature);
            if (language == EShLangFragment) {
                const char* exts[2] = { E_GL_ARB_separate_shader_objects, E_GL_ARB_explicit_attrib_location };
                profileRequires(loc, ~EEsProfile, 330, 2, exts, feature);
                profileRequires(loc, EEsProfile, 300, nullptr, feature);
            } else {
                profileRequires(loc, ~EEsProfile, 410, E_GL_ARB_separate_shader_objects, feature);
                profileRequires(loc, EEsProfile, 310, nullptr, feature);
            }
            break;
        }
        case EvqUniform:
        case EvqBuffer:
        {
            const char* feature = "location qualifier on uniform or buffer";
            requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, feature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, nullptr, feature);
            profileRequires(loc, EEsProfile, 310, nullptr, feature);
            break;
        }
        default:
            break;
        }
        if (qualifier.hasIndex()) {
            if (qualifier.storage != EvqVaryingOut)
                error(loc, "can only be used on an output", "index", "");
            if (! qualifier.hasLocation())
                error(loc, "can only be used with an explicit location", "index", "");
        }
    }

    if (qualifier.hasBinding()) {
        if (! qualifier.isUniformOrBuffer() && !qualifier.isTaskMemory())
            error(loc, "requires uniform or buffer storage qualifier", "binding", "");
    }
    if (qualifier.hasStream()) {
        if (!qualifier.isPipeOutput())
            error(loc, "can only be used on an output", "stream", "");
    }
    if (qualifier.hasXfb()) {
        if (!qualifier.isPipeOutput())
            error(loc, "can only be used on an output", "xfb layout qualifier", "");
    }
    if (qualifier.hasUniformLayout()) {
        if (! qualifier.isUniformOrBuffer() && !qualifier.isTaskMemory()) {
            if (qualifier.hasMatrix() || qualifier.hasPacking())
                error(loc, "matrix or packing qualifiers can only be used on a uniform or buffer", "layout", "");
            if (qualifier.hasOffset() || qualifier.hasAlign())
                error(loc, "offset/align can only be used on a uniform or buffer", "layout", "");
        }
    }
    if (qualifier.layoutPushConstant) {
        if (qualifier.storage != EvqUniform)
            error(loc, "can only be used with a uniform", "push_constant", "");
        if (qualifier.hasSet())
            error(loc, "cannot be used with push_constant", "set", "");
    }
#ifdef NV_EXTENSIONS
    if (qualifier.layoutShaderRecordNV) {
        if (qualifier.storage != EvqBuffer)
            error(loc, "can only be used with a buffer", "shaderRecordNVX", "");
        if (qualifier.hasBinding())
            error(loc, "cannot be used with shaderRecordNVX", "binding", "");
        if (qualifier.hasSet())
            error(loc, "cannot be used with shaderRecordNVX", "set", "");
    }
#endif
}

// For places that can't have shader-level layout qualifiers
void TParseContext::checkNoShaderLayouts(const TSourceLoc& loc, const TShaderQualifiers& shaderQualifiers)
{
    const char* message = "can only apply to a standalone qualifier";

    if (shaderQualifiers.geometry != ElgNone)
        error(loc, message, TQualifier::getGeometryString(shaderQualifiers.geometry), "");
    if (shaderQualifiers.spacing != EvsNone)
        error(loc, message, TQualifier::getVertexSpacingString(shaderQualifiers.spacing), "");
    if (shaderQualifiers.order != EvoNone)
        error(loc, message, TQualifier::getVertexOrderString(shaderQualifiers.order), "");
    if (shaderQualifiers.pointMode)
        error(loc, message, "point_mode", "");
    if (shaderQualifiers.invocations != TQualifier::layoutNotSet)
        error(loc, message, "invocations", "");
    if (shaderQualifiers.earlyFragmentTests)
        error(loc, message, "early_fragment_tests", "");
    if (shaderQualifiers.postDepthCoverage)
        error(loc, message, "post_depth_coverage", "");
    for (int i = 0; i < 3; ++i) {
        if (shaderQualifiers.localSize[i] > 1)
            error(loc, message, "local_size", "");
        if (shaderQualifiers.localSizeSpecId[i] != TQualifier::layoutNotSet)
            error(loc, message, "local_size id", "");
    }
    if (shaderQualifiers.vertices != TQualifier::layoutNotSet) {
        if (language == EShLangGeometry
#ifdef NV_EXTENSIONS
            || language == EShLangMeshNV
#endif
           )
            error(loc, message, "max_vertices", "");
        else if (language == EShLangTessControl)
            error(loc, message, "vertices", "");
        else
            assert(0);
    }
#ifdef NV_EXTENSIONS
    if (shaderQualifiers.primitives != TQualifier::layoutNotSet) {
        if (language == EShLangMeshNV)
            error(loc, message, "max_primitives", "");
        else
            assert(0);
    }
#endif
    if (shaderQualifiers.blendEquation)
        error(loc, message, "blend equation", "");
    if (shaderQualifiers.numViews != TQualifier::layoutNotSet)
        error(loc, message, "num_views", "");
}

// Correct and/or advance an object's offset layout qualifier.
void TParseContext::fixOffset(const TSourceLoc& loc, TSymbol& symbol)
{
    const TQualifier& qualifier = symbol.getType().getQualifier();
    if (symbol.getType().getBasicType() == EbtAtomicUint) {
        if (qualifier.hasBinding() && (int)qualifier.layoutBinding < resources.maxAtomicCounterBindings) {

            // Set the offset
            int offset;
            if (qualifier.hasOffset())
                offset = qualifier.layoutOffset;
            else
                offset = atomicUintOffsets[qualifier.layoutBinding];
            symbol.getWritableType().getQualifier().layoutOffset = offset;

            // Check for overlap
            int numOffsets = 4;
            if (symbol.getType().isArray()) {
                if (symbol.getType().isSizedArray() && !symbol.getType().getArraySizes()->isInnerUnsized())
                    numOffsets *= symbol.getType().getCumulativeArraySize();
                else {
                    // "It is a compile-time error to declare an unsized array of atomic_uint."
                    error(loc, "array must be explicitly sized", "atomic_uint", "");
                }
            }
            int repeated = intermediate.addUsedOffsets(qualifier.layoutBinding, offset, numOffsets);
            if (repeated >= 0)
                error(loc, "atomic counters sharing the same offset:", "offset", "%d", repeated);

            // Bump the default offset
            atomicUintOffsets[qualifier.layoutBinding] = offset + numOffsets;
        }
    }
}

//
// Look up a function name in the symbol table, and make sure it is a function.
//
// Return the function symbol if found, otherwise nullptr.
//
const TFunction* TParseContext::findFunction(const TSourceLoc& loc, const TFunction& call, bool& builtIn)
{
    const TFunction* function = nullptr;

    if (symbolTable.isFunctionNameVariable(call.getName())) {
        error(loc, "can't use function syntax on variable", call.getName().c_str(), "");
        return nullptr;
    }

    bool explicitTypesEnabled = extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_int8) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_int16) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_int32) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_int64) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_float16) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_float32) ||
                                extensionTurnedOn(E_GL_KHX_shader_explicit_arithmetic_types_float64);

    if (profile == EEsProfile || version < 120)
        function = findFunctionExact(loc, call, builtIn);
    else if (version < 400)
        function = findFunction120(loc, call, builtIn);
    else if (explicitTypesEnabled)
        function = findFunctionExplicitTypes(loc, call, builtIn);
    else
        function = findFunction400(loc, call, builtIn);

    return function;
}

// Function finding algorithm for ES and desktop 110.
const TFunction* TParseContext::findFunctionExact(const TSourceLoc& loc, const TFunction& call, bool& builtIn)
{
    TSymbol* symbol = symbolTable.find(call.getMangledName(), &builtIn);
    if (symbol == nullptr) {
        error(loc, "no matching overloaded function found", call.getName().c_str(), "");

        return nullptr;
    }

    return symbol->getAsFunction();
}

// Function finding algorithm for desktop versions 120 through 330.
const TFunction* TParseContext::findFunction120(const TSourceLoc& loc, const TFunction& call, bool& builtIn)
{
    // first, look for an exact match
    TSymbol* symbol = symbolTable.find(call.getMangledName(), &builtIn);
    if (symbol)
        return symbol->getAsFunction();

    // exact match not found, look through a list of overloaded functions of the same name

    // "If no exact match is found, then [implicit conversions] will be applied to find a match. Mismatched types
    // on input parameters (in or inout or default) must have a conversion from the calling argument type to the
    // formal parameter type. Mismatched types on output parameters (out or inout) must have a conversion
    // from the formal parameter type to the calling argument type.  When argument conversions are used to find
    // a match, it is a semantic error if there are multiple ways to apply these conversions to make the call match
    // more than one function."

    const TFunction* candidate = nullptr;
    TVector<const TFunction*> candidateList;
    symbolTable.findFunctionNameList(call.getMangledName(), candidateList, builtIn);

    for (auto it = candidateList.begin(); it != candidateList.end(); ++it) {
        const TFunction& function = *(*it);

        // to even be a potential match, number of arguments has to match
        if (call.getParamCount() != function.getParamCount())
            continue;

        bool possibleMatch = true;
        for (int i = 0; i < function.getParamCount(); ++i) {
            // same types is easy
            if (*function[i].type == *call[i].type)
                continue;

            // We have a mismatch in type, see if it is implicitly convertible

            if (function[i].type->isArray() || call[i].type->isArray() ||
                ! function[i].type->sameElementShape(*call[i].type))
                possibleMatch = false;
            else {
                // do direction-specific checks for conversion of basic type
                if (function[i].type->getQualifier().isParamInput()) {
                    if (! intermediate.canImplicitlyPromote(call[i].type->getBasicType(), function[i].type->getBasicType()))
                        possibleMatch = false;
                }
                if (function[i].type->getQualifier().isParamOutput()) {
                    if (! intermediate.canImplicitlyPromote(function[i].type->getBasicType(), call[i].type->getBasicType()))
                        possibleMatch = false;
                }
            }
            if (! possibleMatch)
                break;
        }
        if (possibleMatch) {
            if (candidate) {
                // our second match, meaning ambiguity
                error(loc, "ambiguous function signature match: multiple signatures match under implicit type conversion", call.getName().c_str(), "");
            } else
                candidate = &function;
        }
    }

    if (candidate == nullptr)
        error(loc, "no matching overloaded function found", call.getName().c_str(), "");

    return candidate;
}

// Function finding algorithm for desktop version 400 and above.
//
// "When function calls are resolved, an exact type match for all the arguments
// is sought. If an exact match is found, all other functions are ignored, and
// the exact match is used. If no exact match is found, then the implicit
// conversions in section 4.1.10 Implicit Conversions will be applied to find
// a match. Mismatched types on input parameters (in or inout or default) must
// have a conversion from the calling argument type to the formal parameter type.
// Mismatched types on output parameters (out or inout) must have a conversion
// from the formal parameter type to the calling argument type.
//
// "If implicit conversions can be used to find more than one matching function,
// a single best-matching function is sought. To determine a best match, the
// conversions between calling argument and formal parameter types are compared
// for each function argument and pair of matching functions. After these
// comparisons are performed, each pair of matching functions are compared.
// A function declaration A is considered a better match than function
// declaration B if
//
//  * for at least one function argument, the conversion for that argument in A
//    is better than the corresponding conversion in B; and
//  * there is no function argument for which the conversion in B is better than
//    the corresponding conversion in A.
//
// "If a single function declaration is considered a better match than every
// other matching function declaration, it will be used. Otherwise, a
// compile-time semantic error for an ambiguous overloaded function call occurs.
//
// "To determine whether the conversion for a single argument in one match is
// better than that for another match, the following rules are applied, in order:
//
//  1. An exact match is better than a match involving any implicit conversion.
//  2. A match involving an implicit conversion from float to double is better
//     than a match involving any other implicit conversion.
//  3. A match involving an implicit conversion from either int or uint to float
//     is better than a match involving an implicit conversion from either int
//     or uint to double.
//
// "If none of the rules above apply to a particular pair of conversions, neither
// conversion is considered better than the other."
//
const TFunction* TParseContext::findFunction400(const TSourceLoc& loc, const TFunction& call, bool& builtIn)
{
    // first, look for an exact match
    TSymbol* symbol = symbolTable.find(call.getMangledName(), &builtIn);
    if (symbol)
        return symbol->getAsFunction();

    // no exact match, use the generic selector, parameterized by the GLSL rules

    // create list of candidates to send
    TVector<const TFunction*> candidateList;
    symbolTable.findFunctionNameList(call.getMangledName(), candidateList, builtIn);

    // can 'from' convert to 'to'?
    const auto convertible = [this](const TType& from, const TType& to, TOperator, int) -> bool {
        if (from == to)
            return true;
        if (from.isArray() || to.isArray() || ! from.sameElementShape(to))
            return false;
        return intermediate.canImplicitlyPromote(from.getBasicType(), to.getBasicType());
    };

    // Is 'to2' a better conversion than 'to1'?
    // Ties should not be considered as better.
    // Assumes 'convertible' already said true.
    const auto better = [](const TType& from, const TType& to1, const TType& to2) -> bool {
        // 1. exact match
        if (from == to2)
            return from != to1;
        if (from == to1)
            return false;

        // 2. float -> double is better
        if (from.getBasicType() == EbtFloat) {
            if (to2.getBasicType() == EbtDouble && to1.getBasicType() != EbtDouble)
                return true;
        }

        // 3. -> float is better than -> double
        return to2.getBasicType() == EbtFloat && to1.getBasicType() == EbtDouble;
    };

    // for ambiguity reporting
    bool tie = false;

    // send to the generic selector
    const TFunction* bestMatch = selectFunction(candidateList, call, convertible, better, tie);

    if (bestMatch == nullptr)
        error(loc, "no matching overloaded function found", call.getName().c_str(), "");
    else if (tie)
        error(loc, "ambiguous best function under implicit type conversion", call.getName().c_str(), "");

    return bestMatch;
}

// "To determine whether the conversion for a single argument in one match
//  is better than that for another match, the conversion is assigned of the
//  three ranks ordered from best to worst:
//   1. Exact match: no conversion.
//    2. Promotion: integral or floating-point promotion.
//    3. Conversion: integral conversion, floating-point conversion,
//       floating-integral conversion.
//  A conversion C1 is better than a conversion C2 if the rank of C1 is
//  better than the rank of C2."
const TFunction* TParseContext::findFunctionExplicitTypes(const TSourceLoc& loc, const TFunction& call, bool& builtIn)
{
    // first, look for an exact match
    TSymbol* symbol = symbolTable.find(call.getMangledName(), &builtIn);
    if (symbol)
        return symbol->getAsFunction();

    // no exact match, use the generic selector, parameterized by the GLSL rules

    // create list of candidates to send
    TVector<const TFunction*> candidateList;
    symbolTable.findFunctionNameList(call.getMangledName(), candidateList, builtIn);

    // can 'from' convert to 'to'?
    const auto convertible = [this](const TType& from, const TType& to, TOperator, int) -> bool {
        if (from == to)
            return true;
        if (from.isArray() || to.isArray() || ! from.sameElementShape(to))
            return false;
        return intermediate.canImplicitlyPromote(from.getBasicType(), to.getBasicType());
    };

    // Is 'to2' a better conversion than 'to1'?
    // Ties should not be considered as better.
    // Assumes 'convertible' already said true.
    const auto better = [this](const TType& from, const TType& to1, const TType& to2) -> bool {
        // 1. exact match
        if (from == to2)
            return from != to1;
        if (from == to1)
            return false;

        // 2. Promotion (integral, floating-point) is better
        TBasicType from_type = from.getBasicType();
        TBasicType to1_type = to1.getBasicType();
        TBasicType to2_type = to2.getBasicType();
        bool isPromotion1 = (intermediate.isIntegralPromotion(from_type, to1_type) ||
                             intermediate.isFPPromotion(from_type, to1_type));
        bool isPromotion2 = (intermediate.isIntegralPromotion(from_type, to2_type) ||
                             intermediate.isFPPromotion(from_type, to2_type));
        if (isPromotion2)
            return !isPromotion1;
        if(isPromotion1)
            return false;

        // 3. Conversion (integral, floating-point , floating-integral)
        bool isConversion1 = (intermediate.isIntegralConversion(from_type, to1_type) ||
                              intermediate.isFPConversion(from_type, to1_type) ||
                              intermediate.isFPIntegralConversion(from_type, to1_type));
        bool isConversion2 = (intermediate.isIntegralConversion(from_type, to2_type) ||
                              intermediate.isFPConversion(from_type, to2_type) ||
                              intermediate.isFPIntegralConversion(from_type, to2_type));

        return isConversion2 && !isConversion1;
    };

    // for ambiguity reporting
    bool tie = false;

    // send to the generic selector
    const TFunction* bestMatch = selectFunction(candidateList, call, convertible, better, tie);

    if (bestMatch == nullptr)
        error(loc, "no matching overloaded function found", call.getName().c_str(), "");
    else if (tie)
        error(loc, "ambiguous best function under implicit type conversion", call.getName().c_str(), "");

    return bestMatch;
}

// When a declaration includes a type, but not a variable name, it can be
// to establish defaults.
void TParseContext::declareTypeDefaults(const TSourceLoc& loc, const TPublicType& publicType)
{
    if (publicType.basicType == EbtAtomicUint && publicType.qualifier.hasBinding() && publicType.qualifier.hasOffset()) {
        if (publicType.qualifier.layoutBinding >= (unsigned int)resources.maxAtomicCounterBindings) {
            error(loc, "atomic_uint binding is too large", "binding", "");
            return;
        }
        atomicUintOffsets[publicType.qualifier.layoutBinding] = publicType.qualifier.layoutOffset;
        return;
    }

    if (publicType.qualifier.hasLayout())
        warn(loc, "useless application of layout qualifier", "layout", "");
}

//
// Do everything necessary to handle a variable (non-block) declaration.
// Either redeclaring a variable, or making a new one, updating the symbol
// table, and all error checking.
//
// Returns a subtree node that computes an initializer, if needed.
// Returns nullptr if there is no code to execute for initialization.
//
// 'publicType' is the type part of the declaration (to the left)
// 'arraySizes' is the arrayness tagged on the identifier (to the right)
//
TIntermNode* TParseContext::declareVariable(const TSourceLoc& loc, TString& identifier, const TPublicType& publicType,
    TArraySizes* arraySizes, TIntermTyped* initializer)
{
    // Make a fresh type that combines the characteristics from the individual
    // identifier syntax and the declaration-type syntax.
    TType type(publicType);
    type.transferArraySizes(arraySizes);
    type.copyArrayInnerSizes(publicType.arraySizes);
    arrayOfArrayVersionCheck(loc, type.getArraySizes());

    if (voidErrorCheck(loc, identifier, type.getBasicType()))
        return nullptr;

    if (initializer)
        rValueErrorCheck(loc, "initializer", initializer);
    else
        nonInitConstCheck(loc, identifier, type);

    samplerCheck(loc, type, identifier, initializer);
    atomicUintCheck(loc, type, identifier);
    transparentOpaqueCheck(loc, type, identifier);
#ifdef NV_EXTENSIONS
    accStructNVCheck(loc, type, identifier);
#endif

    if (type.getQualifier().storage != EvqUniform && type.getQualifier().storage != EvqBuffer) {
        if (type.containsBasicType(EbtFloat16))
            requireFloat16Arithmetic(loc, "qualifier", "float16 types can only be in uniform block or buffer storage");
        if (type.contains16BitInt())
            requireInt16Arithmetic(loc, "qualifier", "(u)int16 types can only be in uniform block or buffer storage");
        if (type.contains8BitInt())
            requireInt8Arithmetic(loc, "qualifier", "(u)int8 types can only be in uniform block or buffer storage");
    }

    if (identifier != "gl_FragCoord" && (publicType.shaderQualifiers.originUpperLeft || publicType.shaderQualifiers.pixelCenterInteger))
        error(loc, "can only apply origin_upper_left and pixel_center_origin to gl_FragCoord", "layout qualifier", "");
    if (identifier != "gl_FragDepth" && publicType.shaderQualifiers.layoutDepth != EldNone)
        error(loc, "can only apply depth layout to gl_FragDepth", "layout qualifier", "");

    // Check for redeclaration of built-ins and/or attempting to declare a reserved name
    TSymbol* symbol = redeclareBuiltinVariable(loc, identifier, type.getQualifier(), publicType.shaderQualifiers);
    if (symbol == nullptr)
        reservedErrorCheck(loc, identifier);

    inheritGlobalDefaults(type.getQualifier());

    // Declare the variable
    if (type.isArray()) {
        // Check that implicit sizing is only where allowed.
        arraySizesCheck(loc, type.getQualifier(), type.getArraySizes(), initializer, false);

        if (! arrayQualifierError(loc, type.getQualifier()) && ! arrayError(loc, type))
            declareArray(loc, identifier, type, symbol);

        if (initializer) {
            profileRequires(loc, ENoProfile, 120, E_GL_3DL_array_objects, "initializer");
            profileRequires(loc, EEsProfile, 300, nullptr, "initializer");
        }
    } else {
        // non-array case
        if (symbol == nullptr)
            symbol = declareNonArray(loc, identifier, type);
        else if (type != symbol->getType())
            error(loc, "cannot change the type of", "redeclaration", symbol->getName().c_str());
    }

    if (symbol == nullptr)
        return nullptr;

    // Deal with initializer
    TIntermNode* initNode = nullptr;
    if (symbol != nullptr && initializer) {
        TVariable* variable = symbol->getAsVariable();
        if (! variable) {
            error(loc, "initializer requires a variable, not a member", identifier.c_str(), "");
            return nullptr;
        }
        initNode = executeInitializer(loc, initializer, variable);
    }

    // look for errors in layout qualifier use
    layoutObjectCheck(loc, *symbol);

    // fix up
    fixOffset(loc, *symbol);

    return initNode;
}

// Pick up global defaults from the provide global defaults into dst.
void TParseContext::inheritGlobalDefaults(TQualifier& dst) const
{
    if (dst.storage == EvqVaryingOut) {
        if (! dst.hasStream() && language == EShLangGeometry)
            dst.layoutStream = globalOutputDefaults.layoutStream;
        if (! dst.hasXfbBuffer())
            dst.layoutXfbBuffer = globalOutputDefaults.layoutXfbBuffer;
    }
}

//
// Make an internal-only variable whose name is for debug purposes only
// and won't be searched for.  Callers will only use the return value to use
// the variable, not the name to look it up.  It is okay if the name
// is the same as other names; there won't be any conflict.
//
TVariable* TParseContext::makeInternalVariable(const char* name, const TType& type) const
{
    TString* nameString = NewPoolTString(name);
    TVariable* variable = new TVariable(nameString, type);
    symbolTable.makeInternalVariable(*variable);

    return variable;
}

//
// Declare a non-array variable, the main point being there is no redeclaration
// for resizing allowed.
//
// Return the successfully declared variable.
//
TVariable* TParseContext::declareNonArray(const TSourceLoc& loc, const TString& identifier, const TType& type)
{
    // make a new variable
    TVariable* variable = new TVariable(&identifier, type);

    ioArrayCheck(loc, type, identifier);

    // add variable to symbol table
    if (symbolTable.insert(*variable)) {
        if (symbolTable.atGlobalLevel())
            trackLinkage(*variable);
        return variable;
    }

    error(loc, "redefinition", variable->getName().c_str(), "");
    return nullptr;
}

//
// Handle all types of initializers from the grammar.
//
// Returning nullptr just means there is no code to execute to handle the
// initializer, which will, for example, be the case for constant initializers.
//
TIntermNode* TParseContext::executeInitializer(const TSourceLoc& loc, TIntermTyped* initializer, TVariable* variable)
{
    //
    // Identifier must be of type constant, a global, or a temporary, and
    // starting at version 120, desktop allows uniforms to have initializers.
    //
    TStorageQualifier qualifier = variable->getType().getQualifier().storage;
    if (! (qualifier == EvqTemporary || qualifier == EvqGlobal || qualifier == EvqConst ||
           (qualifier == EvqUniform && profile != EEsProfile && version >= 120))) {
        error(loc, " cannot initialize this type of qualifier ", variable->getType().getStorageQualifierString(), "");
        return nullptr;
    }
    arrayObjectCheck(loc, variable->getType(), "array initializer");

    //
    // If the initializer was from braces { ... }, we convert the whole subtree to a
    // constructor-style subtree, allowing the rest of the code to operate
    // identically for both kinds of initializers.
    //
    // Type can't be deduced from the initializer list, so a skeletal type to
    // follow has to be passed in.  Constness and specialization-constness
    // should be deduced bottom up, not dictated by the skeletal type.
    //
    TType skeletalType;
    skeletalType.shallowCopy(variable->getType());
    skeletalType.getQualifier().makeTemporary();
    initializer = convertInitializerList(loc, skeletalType, initializer);
    if (! initializer) {
        // error recovery; don't leave const without constant values
        if (qualifier == EvqConst)
            variable->getWritableType().getQualifier().makeTemporary();
        return nullptr;
    }

    // Fix outer arrayness if variable is unsized, getting size from the initializer
    if (initializer->getType().isSizedArray() && variable->getType().isUnsizedArray())
        variable->getWritableType().changeOuterArraySize(initializer->getType().getOuterArraySize());

    // Inner arrayness can also get set by an initializer
    if (initializer->getType().isArrayOfArrays() && variable->getType().isArrayOfArrays() &&
        initializer->getType().getArraySizes()->getNumDims() ==
           variable->getType().getArraySizes()->getNumDims()) {
        // adopt unsized sizes from the initializer's sizes
        for (int d = 1; d < variable->getType().getArraySizes()->getNumDims(); ++d) {
            if (variable->getType().getArraySizes()->getDimSize(d) == UnsizedArraySize) {
                variable->getWritableType().getArraySizes()->setDimSize(d,
                    initializer->getType().getArraySizes()->getDimSize(d));
            }
        }
    }

    // Uniforms require a compile-time constant initializer
    if (qualifier == EvqUniform && ! initializer->getType().getQualifier().isFrontEndConstant()) {
        error(loc, "uniform initializers must be constant", "=", "'%s'", variable->getType().getCompleteString().c_str());
        variable->getWritableType().getQualifier().makeTemporary();
        return nullptr;
    }
    // Global consts require a constant initializer (specialization constant is okay)
    if (qualifier == EvqConst && symbolTable.atGlobalLevel() && ! initializer->getType().getQualifier().isConstant()) {
        error(loc, "global const initializers must be constant", "=", "'%s'", variable->getType().getCompleteString().c_str());
        variable->getWritableType().getQualifier().makeTemporary();
        return nullptr;
    }

    // Const variables require a constant initializer, depending on version
    if (qualifier == EvqConst) {
        if (! initializer->getType().getQualifier().isConstant()) {
            const char* initFeature = "non-constant initializer";
            requireProfile(loc, ~EEsProfile, initFeature);
            profileRequires(loc, ~EEsProfile, 420, E_GL_ARB_shading_language_420pack, initFeature);
            variable->getWritableType().getQualifier().storage = EvqConstReadOnly;
            qualifier = EvqConstReadOnly;
        }
    } else {
        // Non-const global variables in ES need a const initializer.
        //
        // "In declarations of global variables with no storage qualifier or with a const
        // qualifier any initializer must be a constant expression."
        if (symbolTable.atGlobalLevel() && ! initializer->getType().getQualifier().isConstant()) {
            const char* initFeature = "non-constant global initializer (needs GL_EXT_shader_non_constant_global_initializers)";
            if (profile == EEsProfile) {
                if (relaxedErrors() && ! extensionTurnedOn(E_GL_EXT_shader_non_constant_global_initializers))
                    warn(loc, "not allowed in this version", initFeature, "");
                else
                    profileRequires(loc, EEsProfile, 0, E_GL_EXT_shader_non_constant_global_initializers, initFeature);
            }
        }
    }

    if (qualifier == EvqConst || qualifier == EvqUniform) {
        // Compile-time tagging of the variable with its constant value...

        initializer = intermediate.addConversion(EOpAssign, variable->getType(), initializer);
        if (! initializer || ! initializer->getType().getQualifier().isConstant() || variable->getType() != initializer->getType()) {
            error(loc, "non-matching or non-convertible constant type for const initializer",
                  variable->getType().getStorageQualifierString(), "");
            variable->getWritableType().getQualifier().makeTemporary();
            return nullptr;
        }

        // We either have a folded constant in getAsConstantUnion, or we have to use
        // the initializer's subtree in the AST to represent the computation of a
        // specialization constant.
        assert(initializer->getAsConstantUnion() || initializer->getType().getQualifier().isSpecConstant());
        if (initializer->getAsConstantUnion())
            variable->setConstArray(initializer->getAsConstantUnion()->getConstArray());
        else {
            // It's a specialization constant.
            variable->getWritableType().getQualifier().makeSpecConstant();

            // Keep the subtree that computes the specialization constant with the variable.
            // Later, a symbol node will adopt the subtree from the variable.
            variable->setConstSubtree(initializer);
        }
    } else {
        // normal assigning of a value to a variable...
        specializationCheck(loc, initializer->getType(), "initializer");
        TIntermSymbol* intermSymbol = intermediate.addSymbol(*variable, loc);
        TIntermTyped* initNode = intermediate.addAssign(EOpAssign, intermSymbol, initializer, loc);
        if (! initNode)
            assignError(loc, "=", intermSymbol->getCompleteString(), initializer->getCompleteString());

        return initNode;
    }

    return nullptr;
}

//
// Reprocess any initializer-list (the  "{ ... }" syntax) parts of the
// initializer.
//
// Need to hierarchically assign correct types and implicit
// conversions. Will do this mimicking the same process used for
// creating a constructor-style initializer, ensuring we get the
// same form.  However, it has to in parallel walk the 'type'
// passed in, as type cannot be deduced from an initializer list.
//
TIntermTyped* TParseContext::convertInitializerList(const TSourceLoc& loc, const TType& type, TIntermTyped* initializer)
{
    // Will operate recursively.  Once a subtree is found that is constructor style,
    // everything below it is already good: Only the "top part" of the initializer
    // can be an initializer list, where "top part" can extend for several (or all) levels.

    // see if we have bottomed out in the tree within the initializer-list part
    TIntermAggregate* initList = initializer->getAsAggregate();
    if (! initList || initList->getOp() != EOpNull)
        return initializer;

    // Of the initializer-list set of nodes, need to process bottom up,
    // so recurse deep, then process on the way up.

    // Go down the tree here...
    if (type.isArray()) {
        // The type's array might be unsized, which could be okay, so base sizes on the size of the aggregate.
        // Later on, initializer execution code will deal with array size logic.
        TType arrayType;
        arrayType.shallowCopy(type);                     // sharing struct stuff is fine
        arrayType.copyArraySizes(*type.getArraySizes());  // but get a fresh copy of the array information, to edit below

        // edit array sizes to fill in unsized dimensions
        arrayType.changeOuterArraySize((int)initList->getSequence().size());
        TIntermTyped* firstInit = initList->getSequence()[0]->getAsTyped();
        if (arrayType.isArrayOfArrays() && firstInit->getType().isArray() &&
            arrayType.getArraySizes()->getNumDims() == firstInit->getType().getArraySizes()->getNumDims() + 1) {
            for (int d = 1; d < arrayType.getArraySizes()->getNumDims(); ++d) {
                if (arrayType.getArraySizes()->getDimSize(d) == UnsizedArraySize)
                    arrayType.getArraySizes()->setDimSize(d, firstInit->getType().getArraySizes()->getDimSize(d - 1));
            }
        }

        TType elementType(arrayType, 0); // dereferenced type
        for (size_t i = 0; i < initList->getSequence().size(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, elementType, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == nullptr)
                return nullptr;
        }

        return addConstructor(loc, initList, arrayType);
    } else if (type.isStruct()) {
        if (type.getStruct()->size() != initList->getSequence().size()) {
            error(loc, "wrong number of structure members", "initializer list", "");
            return nullptr;
        }
        for (size_t i = 0; i < type.getStruct()->size(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, *(*type.getStruct())[i].type, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == nullptr)
                return nullptr;
        }
    } else if (type.isMatrix()) {
        if (type.getMatrixCols() != (int)initList->getSequence().size()) {
            error(loc, "wrong number of matrix columns:", "initializer list", type.getCompleteString().c_str());
            return nullptr;
        }
        TType vectorType(type, 0); // dereferenced type
        for (int i = 0; i < type.getMatrixCols(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, vectorType, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == nullptr)
                return nullptr;
        }
    } else if (type.isVector()) {
        if (type.getVectorSize() != (int)initList->getSequence().size()) {
            error(loc, "wrong vector size (or rows in a matrix column):", "initializer list", type.getCompleteString().c_str());
            return nullptr;
        }
    } else {
        error(loc, "unexpected initializer-list type:", "initializer list", type.getCompleteString().c_str());
        return nullptr;
    }

    // Now that the subtree is processed, process this node as if the
    // initializer list is a set of arguments to a constructor.
    TIntermNode* emulatedConstructorArguments;
    if (initList->getSequence().size() == 1)
        emulatedConstructorArguments = initList->getSequence()[0];
    else
        emulatedConstructorArguments = initList;
    return addConstructor(loc, emulatedConstructorArguments, type);
}

//
// Test for the correctness of the parameters passed to various constructor functions
// and also convert them to the right data type, if allowed and required.
//
// 'node' is what to construct from.
// 'type' is what type to construct.
//
// Returns nullptr for an error or the constructed node (aggregate or typed) for no error.
//
TIntermTyped* TParseContext::addConstructor(const TSourceLoc& loc, TIntermNode* node, const TType& type)
{
    if (node == nullptr || node->getAsTyped() == nullptr)
        return nullptr;
    rValueErrorCheck(loc, "constructor", node->getAsTyped());

    TIntermAggregate* aggrNode = node->getAsAggregate();
    TOperator op = intermediate.mapTypeToConstructorOp(type);

    // Combined texture-sampler constructors are completely semantic checked
    // in constructorTextureSamplerError()
    if (op == EOpConstructTextureSampler) {
        if (aggrNode->getSequence()[1]->getAsTyped()->getType().getSampler().shadow) {
            // Transfer depth into the texture (SPIR-V image) type, as a hint
            // for tools to know this texture/image is a depth image.
            aggrNode->getSequence()[0]->getAsTyped()->getWritableType().getSampler().shadow = true;
        }
        return intermediate.setAggregateOperator(aggrNode, op, type, loc);
    }

    TTypeList::const_iterator memberTypes;
    if (op == EOpConstructStruct)
        memberTypes = type.getStruct()->begin();

    TType elementType;
    if (type.isArray()) {
        TType dereferenced(type, 0);
        elementType.shallowCopy(dereferenced);
    } else
        elementType.shallowCopy(type);

    bool singleArg;
    if (aggrNode) {
        if (aggrNode->getOp() != EOpNull)
            singleArg = true;
        else
            singleArg = false;
    } else
        singleArg = true;

    TIntermTyped *newNode;
    if (singleArg) {
        // If structure constructor or array constructor is being called
        // for only one parameter inside the structure, we need to call constructAggregate function once.
        if (type.isArray())
            newNode = constructAggregate(node, elementType, 1, node->getLoc());
        else if (op == EOpConstructStruct)
            newNode = constructAggregate(node, *(*memberTypes).type, 1, node->getLoc());
        else
            newNode = constructBuiltIn(type, op, node->getAsTyped(), node->getLoc(), false);

        if (newNode && (type.isArray() || op == EOpConstructStruct))
            newNode = intermediate.setAggregateOperator(newNode, EOpConstructStruct, type, loc);

        return newNode;
    }

    //
    // Handle list of arguments.
    //
    TIntermSequence &sequenceVector = aggrNode->getSequence();    // Stores the information about the parameter to the constructor
    // if the structure constructor contains more than one parameter, then construct
    // each parameter

    int paramCount = 0;  // keeps track of the constructor parameter number being checked

    // for each parameter to the constructor call, check to see if the right type is passed or convert them
    // to the right type if possible (and allowed).
    // for structure constructors, just check if the right type is passed, no conversion is allowed.
    for (TIntermSequence::iterator p = sequenceVector.begin();
                                   p != sequenceVector.end(); p++, paramCount++) {
        if (type.isArray())
            newNode = constructAggregate(*p, elementType, paramCount+1, node->getLoc());
        else if (op == EOpConstructStruct)
            newNode = constructAggregate(*p, *(memberTypes[paramCount]).type, paramCount+1, node->getLoc());
        else
            newNode = constructBuiltIn(type, op, (*p)->getAsTyped(), node->getLoc(), true);

        if (newNode)
            *p = newNode;
        else
            return nullptr;
    }

    return intermediate.setAggregateOperator(aggrNode, op, type, loc);
}

// Function for constructor implementation. Calls addUnaryMath with appropriate EOp value
// for the parameter to the constructor (passed to this function). Essentially, it converts
// the parameter types correctly. If a constructor expects an int (like ivec2) and is passed a
// float, then float is converted to int.
//
// Returns nullptr for an error or the constructed node.
//
TIntermTyped* TParseContext::constructBuiltIn(const TType& type, TOperator op, TIntermTyped* node, const TSourceLoc& loc,
    bool subset)
{
    // If we are changing a matrix in both domain of basic type and to a non matrix,
    // do the shape change first (by default, below, basic type is changed before shape).
    // This avoids requesting a matrix of a new type that is going to be discarded anyway.
    // TODO: This could be generalized to more type combinations, but that would require
    // more extensive testing and full algorithm rework. For now, the need to do two changes makes
    // the recursive call work, and avoids the most aggregious case of creating integer matrices.
    if (node->getType().isMatrix() && (type.isScalar() || type.isVector()) &&
            type.isFloatingDomain() != node->getType().isFloatingDomain()) {
        TType transitionType(node->getBasicType(), glslang::EvqTemporary, type.getVectorSize(), 0, 0, node->isVector());
        TOperator transitionOp = intermediate.mapTypeToConstructorOp(transitionType);
        node = constructBuiltIn(transitionType, transitionOp, node, loc, false);
    }

    TIntermTyped* newNode;
    TOperator basicOp;

    //
    // First, convert types as needed.
    //
    switch (op) {
    case EOpConstructVec2:
    case EOpConstructVec3:
    case EOpConstructVec4:
    case EOpConstructMat2x2:
    case EOpConstructMat2x3:
    case EOpConstructMat2x4:
    case EOpConstructMat3x2:
    case EOpConstructMat3x3:
    case EOpConstructMat3x4:
    case EOpConstructMat4x2:
    case EOpConstructMat4x3:
    case EOpConstructMat4x4:
    case EOpConstructFloat:
        basicOp = EOpConstructFloat;
        break;

    case EOpConstructDVec2:
    case EOpConstructDVec3:
    case EOpConstructDVec4:
    case EOpConstructDMat2x2:
    case EOpConstructDMat2x3:
    case EOpConstructDMat2x4:
    case EOpConstructDMat3x2:
    case EOpConstructDMat3x3:
    case EOpConstructDMat3x4:
    case EOpConstructDMat4x2:
    case EOpConstructDMat4x3:
    case EOpConstructDMat4x4:
    case EOpConstructDouble:
        basicOp = EOpConstructDouble;
        break;

    case EOpConstructF16Vec2:
    case EOpConstructF16Vec3:
    case EOpConstructF16Vec4:
    case EOpConstructF16Mat2x2:
    case EOpConstructF16Mat2x3:
    case EOpConstructF16Mat2x4:
    case EOpConstructF16Mat3x2:
    case EOpConstructF16Mat3x3:
    case EOpConstructF16Mat3x4:
    case EOpConstructF16Mat4x2:
    case EOpConstructF16Mat4x3:
    case EOpConstructF16Mat4x4:
    case EOpConstructFloat16:
        basicOp = EOpConstructFloat16;
        break;

    case EOpConstructI8Vec2:
    case EOpConstructI8Vec3:
    case EOpConstructI8Vec4:
    case EOpConstructInt8:
        basicOp = EOpConstructInt8;
        break;

    case EOpConstructU8Vec2:
    case EOpConstructU8Vec3:
    case EOpConstructU8Vec4:
    case EOpConstructUint8:
        basicOp = EOpConstructUint8;
        break;

    case EOpConstructI16Vec2:
    case EOpConstructI16Vec3:
    case EOpConstructI16Vec4:
    case EOpConstructInt16:
        basicOp = EOpConstructInt16;
        break;

    case EOpConstructU16Vec2:
    case EOpConstructU16Vec3:
    case EOpConstructU16Vec4:
    case EOpConstructUint16:
        basicOp = EOpConstructUint16;
        break;

    case EOpConstructIVec2:
    case EOpConstructIVec3:
    case EOpConstructIVec4:
    case EOpConstructInt:
        basicOp = EOpConstructInt;
        break;

    case EOpConstructUVec2:
    case EOpConstructUVec3:
    case EOpConstructUVec4:
    case EOpConstructUint:
        basicOp = EOpConstructUint;
        break;

    case EOpConstructI64Vec2:
    case EOpConstructI64Vec3:
    case EOpConstructI64Vec4:
    case EOpConstructInt64:
        basicOp = EOpConstructInt64;
        break;

    case EOpConstructU64Vec2:
    case EOpConstructU64Vec3:
    case EOpConstructU64Vec4:
    case EOpConstructUint64:
        basicOp = EOpConstructUint64;
        break;

    case EOpConstructBVec2:
    case EOpConstructBVec3:
    case EOpConstructBVec4:
    case EOpConstructBool:
        basicOp = EOpConstructBool;
        break;

    case EOpConstructNonuniform:
        node->getWritableType().getQualifier().nonUniform = true;
        return node;
        break;

    default:
        error(loc, "unsupported construction", "", "");

        return nullptr;
    }
    newNode = intermediate.addUnaryMath(basicOp, node, node->getLoc());
    if (newNode == nullptr) {
        error(loc, "can't convert", "constructor", "");
        return nullptr;
    }

    //
    // Now, if there still isn't an operation to do the construction, and we need one, add one.
    //

    // Otherwise, skip out early.
    if (subset || (newNode != node && newNode->getType() == type))
        return newNode;

    // setAggregateOperator will insert a new node for the constructor, as needed.
    return intermediate.setAggregateOperator(newNode, op, type, loc);
}

// This function tests for the type of the parameters to the structure or array constructor. Raises
// an error message if the expected type does not match the parameter passed to the constructor.
//
// Returns nullptr for an error or the input node itself if the expected and the given parameter types match.
//
TIntermTyped* TParseContext::constructAggregate(TIntermNode* node, const TType& type, int paramCount, const TSourceLoc& loc)
{
    TIntermTyped* converted = intermediate.addConversion(EOpConstructStruct, type, node->getAsTyped());
    if (! converted || converted->getType() != type) {
        error(loc, "", "constructor", "cannot convert parameter %d from '%s' to '%s'", paramCount,
              node->getAsTyped()->getType().getCompleteString().c_str(), type.getCompleteString().c_str());

        return nullptr;
    }

    return converted;
}

//
// Do everything needed to add an interface block.
//
void TParseContext::declareBlock(const TSourceLoc& loc, TTypeList& typeList, const TString* instanceName,
    TArraySizes* arraySizes)
{
    blockStageIoCheck(loc, currentBlockQualifier);
    blockQualifierCheck(loc, currentBlockQualifier, instanceName != nullptr);
    if (arraySizes != nullptr) {
        arraySizesCheck(loc, currentBlockQualifier, arraySizes, nullptr, false);
        arrayOfArrayVersionCheck(loc, arraySizes);
        if (arraySizes->getNumDims() > 1)
            requireProfile(loc, ~EEsProfile, "array-of-array of block");
    }

    // fix and check for member storage qualifiers and types that don't belong within a block
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TType& memberType = *typeList[member].type;
        TQualifier& memberQualifier = memberType.getQualifier();
        const TSourceLoc& memberLoc = typeList[member].loc;
        globalQualifierFixCheck(memberLoc, memberQualifier);
        if (memberQualifier.storage != EvqTemporary && memberQualifier.storage != EvqGlobal && memberQualifier.storage != currentBlockQualifier.storage)
            error(memberLoc, "member storage qualifier cannot contradict block storage qualifier", memberType.getFieldName().c_str(), "");
        memberQualifier.storage = currentBlockQualifier.storage;
#ifdef NV_EXTENSIONS
        if (currentBlockQualifier.perPrimitiveNV)
            memberQualifier.perPrimitiveNV = currentBlockQualifier.perPrimitiveNV;
        if (currentBlockQualifier.perViewNV)
            memberQualifier.perViewNV = currentBlockQualifier.perViewNV;
        if (currentBlockQualifier.perTaskNV)
            memberQualifier.perTaskNV = currentBlockQualifier.perTaskNV;
#endif
        if ((currentBlockQualifier.storage == EvqUniform || currentBlockQualifier.storage == EvqBuffer) && (memberQualifier.isInterpolation() || memberQualifier.isAuxiliary()))
            error(memberLoc, "member of uniform or buffer block cannot have an auxiliary or interpolation qualifier", memberType.getFieldName().c_str(), "");
        if (memberType.isArray())
            arraySizesCheck(memberLoc, currentBlockQualifier, memberType.getArraySizes(), nullptr, member == typeList.size() - 1);
        if (memberQualifier.hasOffset()) {
            if (spvVersion.spv == 0) {
                requireProfile(memberLoc, ~EEsProfile, "offset on block member");
                profileRequires(memberLoc, ~EEsProfile, 440, E_GL_ARB_enhanced_layouts, "offset on block member");
            }
        }

        if (memberType.containsOpaque())
            error(memberLoc, "member of block cannot be or contain a sampler, image, or atomic_uint type", typeList[member].type->getFieldName().c_str(), "");
    }

    // This might be a redeclaration of a built-in block.  If so, redeclareBuiltinBlock() will
    // do all the rest.
    if (! symbolTable.atBuiltInLevel() && builtInName(*blockName)) {
        redeclareBuiltinBlock(loc, typeList, *blockName, instanceName, arraySizes);
        return;
    }

    // Not a redeclaration of a built-in; check that all names are user names.
    reservedErrorCheck(loc, *blockName);
    if (instanceName)
        reservedErrorCheck(loc, *instanceName);
    for (unsigned int member = 0; member < typeList.size(); ++member)
        reservedErrorCheck(typeList[member].loc, typeList[member].type->getFieldName());

    // Make default block qualification, and adjust the member qualifications

    TQualifier defaultQualification;
    switch (currentBlockQualifier.storage) {
    case EvqUniform:    defaultQualification = globalUniformDefaults;    break;
    case EvqBuffer:     defaultQualification = globalBufferDefaults;     break;
    case EvqVaryingIn:  defaultQualification = globalInputDefaults;      break;
    case EvqVaryingOut: defaultQualification = globalOutputDefaults;     break;
    default:            defaultQualification.clear();                    break;
    }

    // Special case for "push_constant uniform", which has a default of std430,
    // contrary to normal uniform defaults, and can't have a default tracked for it.
    if ((currentBlockQualifier.layoutPushConstant && !currentBlockQualifier.hasPacking())
#ifdef NV_EXTENSIONS
        || (currentBlockQualifier.layoutShaderRecordNV && !currentBlockQualifier.hasPacking())
#endif
       )
        currentBlockQualifier.layoutPacking = ElpStd430;

#ifdef NV_EXTENSIONS
    // Special case for "taskNV in/out", which has a default of std430,
    if (currentBlockQualifier.perTaskNV && !currentBlockQualifier.hasPacking())
        currentBlockQualifier.layoutPacking = ElpStd430;
#endif

    // fix and check for member layout qualifiers

    mergeObjectLayoutQualifiers(defaultQualification, currentBlockQualifier, true);

    // "The align qualifier can only be used on blocks or block members, and only for blocks declared with std140 or std430 layouts."
    if (currentBlockQualifier.hasAlign()) {
        if (defaultQualification.layoutPacking != ElpStd140 && defaultQualification.layoutPacking != ElpStd430) {
            error(loc, "can only be used with std140 or std430 layout packing", "align", "");
            defaultQualification.layoutAlign = -1;
        }
    }

    bool memberWithLocation = false;
    bool memberWithoutLocation = false;
#ifdef NV_EXTENSIONS
    bool memberWithPerViewQualifier = false;
#endif
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier& memberQualifier = typeList[member].type->getQualifier();
        const TSourceLoc& memberLoc = typeList[member].loc;
        if (memberQualifier.hasStream()) {
            if (defaultQualification.layoutStream != memberQualifier.layoutStream)
                error(memberLoc, "member cannot contradict block", "stream", "");
        }

        // "This includes a block's inheritance of the
        // current global default buffer, a block member's inheritance of the block's
        // buffer, and the requirement that any *xfb_buffer* declared on a block
        // member must match the buffer inherited from the block."
        if (memberQualifier.hasXfbBuffer()) {
            if (defaultQualification.layoutXfbBuffer != memberQualifier.layoutXfbBuffer)
                error(memberLoc, "member cannot contradict block (or what block inherited from global)", "xfb_buffer", "");
        }

        if (memberQualifier.hasPacking())
            error(memberLoc, "member of block cannot have a packing layout qualifier", typeList[member].type->getFieldName().c_str(), "");
        if (memberQualifier.hasLocation()) {
            const char* feature = "location on block member";
            switch (currentBlockQualifier.storage) {
            case EvqVaryingIn:
            case EvqVaryingOut:
                requireProfile(memberLoc, ECoreProfile | ECompatibilityProfile | EEsProfile, feature);
                profileRequires(memberLoc, ECoreProfile | ECompatibilityProfile, 440, E_GL_ARB_enhanced_layouts, feature);
                profileRequires(memberLoc, EEsProfile, 320, Num_AEP_shader_io_blocks, AEP_shader_io_blocks, feature);
                memberWithLocation = true;
                break;
            default:
                error(memberLoc, "can only use in an in/out block", feature, "");
                break;
            }
        } else
            memberWithoutLocation = true;

        // "The offset qualifier can only be used on block members of blocks declared with std140 or std430 layouts."
        // "The align qualifier can only be used on blocks or block members, and only for blocks declared with std140 or std430 layouts."
        if (memberQualifier.hasAlign() || memberQualifier.hasOffset()) {
            if (defaultQualification.layoutPacking != ElpStd140 && defaultQualification.layoutPacking != ElpStd430)
                error(memberLoc, "can only be used with std140 or std430 layout packing", "offset/align", "");
        }

#ifdef NV_EXTENSIONS
        if (memberQualifier.isPerView()) {
            memberWithPerViewQualifier = true;
        }
#endif

        TQualifier newMemberQualification = defaultQualification;
        mergeQualifiers(memberLoc, newMemberQualification, memberQualifier, false);
        memberQualifier = newMemberQualification;
    }

    layoutMemberLocationArrayCheck(loc, memberWithLocation, arraySizes);

    // Process the members
    fixBlockLocations(loc, currentBlockQualifier, typeList, memberWithLocation, memberWithoutLocation);
    fixBlockXfbOffsets(currentBlockQualifier, typeList);
    fixBlockUniformOffsets(currentBlockQualifier, typeList);
    for (unsigned int member = 0; member < typeList.size(); ++member)
        layoutTypeCheck(typeList[member].loc, *typeList[member].type);

#ifdef NV_EXTENSIONS
    if (memberWithPerViewQualifier) {
        for (unsigned int member = 0; member < typeList.size(); ++member) {
            resizeMeshViewDimension(typeList[member].loc, *typeList[member].type);
        }
    }
#endif

    // reverse merge, so that currentBlockQualifier now has all layout information
    // (can't use defaultQualification directly, it's missing other non-layout-default-class qualifiers)
    mergeObjectLayoutQualifiers(currentBlockQualifier, defaultQualification, true);

    //
    // Build and add the interface block as a new type named 'blockName'
    //

    TType blockType(&typeList, *blockName, currentBlockQualifier);
    if (arraySizes != nullptr)
        blockType.transferArraySizes(arraySizes);
    else
        ioArrayCheck(loc, blockType, instanceName ? *instanceName : *blockName);

    //
    // Don't make a user-defined type out of block name; that will cause an error
    // if the same block name gets reused in a different interface.
    //
    // "Block names have no other use within a shader
    // beyond interface matching; it is a compile-time error to use a block name at global scope for anything
    // other than as a block name (e.g., use of a block name for a global variable name or function name is
    // currently reserved)."
    //
    // Use the symbol table to prevent normal reuse of the block's name, as a variable entry,
    // whose type is EbtBlock, but without all the structure; that will come from the type
    // the instances point to.
    //
    TType blockNameType(EbtBlock, blockType.getQualifier().storage);
    TVariable* blockNameVar = new TVariable(blockName, blockNameType);
    if (! symbolTable.insert(*blockNameVar)) {
        TSymbol* existingName = symbolTable.find(*blockName);
        if (existingName->getType().getBasicType() == EbtBlock) {
            if (existingName->getType().getQualifier().storage == blockType.getQualifier().storage) {
                error(loc, "Cannot reuse block name within the same interface:", blockName->c_str(), blockType.getStorageQualifierString());
                return;
            }
        } else {
            error(loc, "block name cannot redefine a non-block name", blockName->c_str(), "");
            return;
        }
    }

    // Add the variable, as anonymous or named instanceName.
    // Make an anonymous variable if no name was provided.
    if (! instanceName)
        instanceName = NewPoolTString("");

    TVariable& variable = *new TVariable(instanceName, blockType);
    if (! symbolTable.insert(variable)) {
        if (*instanceName == "")
            error(loc, "nameless block contains a member that already has a name at global scope", blockName->c_str(), "");
        else
            error(loc, "block instance name redefinition", variable.getName().c_str(), "");

        return;
    }

    // Check for general layout qualifier errors
    layoutObjectCheck(loc, variable);

    // fix up
    if (isIoResizeArray(blockType)) {
        ioArraySymbolResizeList.push_back(&variable);
        checkIoArraysConsistency(loc, true, blockType.getQualifier().isPerPrimitive());
    } else
        fixIoArraySize(loc, variable.getWritableType());

    // Save it in the AST for linker use.
    trackLinkage(variable);
}

// Do all block-declaration checking regarding the combination of in/out/uniform/buffer
// with a particular stage.
void TParseContext::blockStageIoCheck(const TSourceLoc& loc, const TQualifier& qualifier)
{
    switch (qualifier.storage) {
    case EvqUniform:
        profileRequires(loc, EEsProfile, 300, nullptr, "uniform block");
        profileRequires(loc, ENoProfile, 140, nullptr, "uniform block");
        if (currentBlockQualifier.layoutPacking == ElpStd430 && ! currentBlockQualifier.layoutPushConstant)
            error(loc, "requires the 'buffer' storage qualifier", "std430", "");
        break;
    case EvqBuffer:
        requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, "buffer block");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, nullptr, "buffer block");
        profileRequires(loc, EEsProfile, 310, nullptr, "buffer block");
        break;
    case EvqVaryingIn:
        profileRequires(loc, ~EEsProfile, 150, E_GL_ARB_separate_shader_objects, "input block");
        // It is a compile-time error to have an input block in a vertex shader or an output block in a fragment shader
        // "Compute shaders do not permit user-defined input variables..."
        requireStage(loc, (EShLanguageMask)(EShLangTessControlMask|EShLangTessEvaluationMask|EShLangGeometryMask|EShLangFragmentMask
#ifdef NV_EXTENSIONS
                                            |EShLangMeshNVMask
#endif
                                           ), "input block");
        if (language == EShLangFragment) {
            profileRequires(loc, EEsProfile, 320, Num_AEP_shader_io_blocks, AEP_shader_io_blocks, "fragment input block");
        }
#ifdef NV_EXTENSIONS
        else if (language == EShLangMeshNV && ! qualifier.isTaskMemory()) {
            error(loc, "input blocks cannot be used in a mesh shader", "out", "");
        }
#endif
        break;
    case EvqVaryingOut:
        profileRequires(loc, ~EEsProfile, 150, E_GL_ARB_separate_shader_objects, "output block");
        requireStage(loc, (EShLanguageMask)(EShLangVertexMask|EShLangTessControlMask|EShLangTessEvaluationMask|EShLangGeometryMask
#ifdef NV_EXTENSIONS
                                            |EShLangMeshNVMask|EShLangTaskNVMask
#endif
                                           ), "output block");
        // ES 310 can have a block before shader_io is turned on, so skip this test for built-ins
        if (language == EShLangVertex && ! parsingBuiltins) {
            profileRequires(loc, EEsProfile, 320, Num_AEP_shader_io_blocks, AEP_shader_io_blocks, "vertex output block");
        }
#ifdef NV_EXTENSIONS
        else if (language == EShLangMeshNV && qualifier.isTaskMemory()) {
            error(loc, "can only use on input blocks in mesh shader", "taskNV", "");
        }
        else if (language == EShLangTaskNV && ! qualifier.isTaskMemory()) {
            error(loc, "output blocks cannot be used in a task shader", "out", "");
        }
#endif
        break;
#ifdef NV_EXTENSIONS
    case EvqPayloadNV:
        profileRequires(loc, ~EEsProfile, 450, E_GL_NVX_raytracing, "rayPayloadNVX block");
        requireStage(loc, (EShLanguageMask)(EShLangRayGenNVMask | EShLangAnyHitNVMask | EShLangClosestHitNVMask | EShLangMissNVMask),
            "rayPayloadNVX block");
        break;
    case EvqPayloadInNV:
        profileRequires(loc, ~EEsProfile, 450, E_GL_NVX_raytracing, "rayPayloadInNVX block");
        requireStage(loc, (EShLanguageMask)(EShLangAnyHitNVMask | EShLangClosestHitNVMask | EShLangMissNVMask),
            "rayPayloadInNVX block");
        break;
    case EvqHitAttrNV:
        profileRequires(loc, ~EEsProfile, 450, E_GL_NVX_raytracing, "hitAttributeNVX block");
        requireStage(loc, (EShLanguageMask)(EShLangIntersectNVMask | EShLangAnyHitNVMask | EShLangClosestHitNVMask), "hitAttributeNVX block");
        break;
#endif
    default:
        error(loc, "only uniform, buffer, in, or out blocks are supported", blockName->c_str(), "");
        break;
    }
}

// Do all block-declaration checking regarding its qualifiers.
void TParseContext::blockQualifierCheck(const TSourceLoc& loc, const TQualifier& qualifier, bool /*instanceName*/)
{
    // The 4.5 specification says:
    //
    // interface-block :
    //    layout-qualifieropt interface-qualifier  block-name { member-list } instance-nameopt ;
    //
    // interface-qualifier :
    //    in
    //    out
    //    patch in
    //    patch out
    //    uniform
    //    buffer
    //
    // Note however memory qualifiers aren't included, yet the specification also says
    //
    // "...memory qualifiers may also be used in the declaration of shader storage blocks..."

    if (qualifier.isInterpolation())
        error(loc, "cannot use interpolation qualifiers on an interface block", "flat/smooth/noperspective", "");
    if (qualifier.centroid)
        error(loc, "cannot use centroid qualifier on an interface block", "centroid", "");
    if (qualifier.sample)
        error(loc, "cannot use sample qualifier on an interface block", "sample", "");
    if (qualifier.invariant)
        error(loc, "cannot use invariant qualifier on an interface block", "invariant", "");
    if (qualifier.layoutPushConstant)
        intermediate.addPushConstantCount();
#ifdef NV_EXTENSIONS
    if (qualifier.layoutShaderRecordNV)
        intermediate.addShaderRecordNVCount();
    if (qualifier.perTaskNV)
        intermediate.addTaskNVCount();
#endif
}

//
// "For a block, this process applies to the entire block, or until the first member
// is reached that has a location layout qualifier. When a block member is declared with a location
// qualifier, its location comes from that qualifier: The member's location qualifier overrides the block-level
// declaration. Subsequent members are again assigned consecutive locations, based on the newest location,
// until the next member declared with a location qualifier. The values used for locations do not have to be
// declared in increasing order."
void TParseContext::fixBlockLocations(const TSourceLoc& loc, TQualifier& qualifier, TTypeList& typeList, bool memberWithLocation, bool memberWithoutLocation)
{
    // "If a block has no block-level location layout qualifier, it is required that either all or none of its members
    // have a location layout qualifier, or a compile-time error results."
    if (! qualifier.hasLocation() && memberWithLocation && memberWithoutLocation)
        error(loc, "either the block needs a location, or all members need a location, or no members have a location", "location", "");
    else {
        if (memberWithLocation) {
            // remove any block-level location and make it per *every* member
            int nextLocation = 0;  // by the rule above, initial value is not relevant
            if (qualifier.hasAnyLocation()) {
                nextLocation = qualifier.layoutLocation;
                qualifier.layoutLocation = TQualifier::layoutLocationEnd;
                if (qualifier.hasComponent()) {
                    // "It is a compile-time error to apply the *component* qualifier to a ... block"
                    error(loc, "cannot apply to a block", "component", "");
                }
                if (qualifier.hasIndex()) {
                    error(loc, "cannot apply to a block", "index", "");
                }
            }
            for (unsigned int member = 0; member < typeList.size(); ++member) {
                TQualifier& memberQualifier = typeList[member].type->getQualifier();
                const TSourceLoc& memberLoc = typeList[member].loc;
                if (! memberQualifier.hasLocation()) {
                    if (nextLocation >= (int)TQualifier::layoutLocationEnd)
                        error(memberLoc, "location is too large", "location", "");
                    memberQualifier.layoutLocation = nextLocation;
                    memberQualifier.layoutComponent = TQualifier::layoutComponentEnd;
                }
                nextLocation = memberQualifier.layoutLocation + intermediate.computeTypeLocationSize(
                                    *typeList[member].type, language);
            }
        }
    }
}

void TParseContext::fixBlockXfbOffsets(TQualifier& qualifier, TTypeList& typeList)
{
    // "If a block is qualified with xfb_offset, all its
    // members are assigned transform feedback buffer offsets. If a block is not qualified with xfb_offset, any
    // members of that block not qualified with an xfb_offset will not be assigned transform feedback buffer
    // offsets."

    if (! qualifier.hasXfbBuffer() || ! qualifier.hasXfbOffset())
        return;

    int nextOffset = qualifier.layoutXfbOffset;
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier& memberQualifier = typeList[member].type->getQualifier();
        bool containsDouble = false;
        int memberSize = intermediate.computeTypeXfbSize(*typeList[member].type, containsDouble);
        // see if we need to auto-assign an offset to this member
        if (! memberQualifier.hasXfbOffset()) {
            // "if applied to an aggregate containing a double, the offset must also be a multiple of 8"
            if (containsDouble)
                RoundToPow2(nextOffset, 8);
            memberQualifier.layoutXfbOffset = nextOffset;
        } else
            nextOffset = memberQualifier.layoutXfbOffset;
        nextOffset += memberSize;
    }

    // The above gave all block members an offset, so we can take it off the block now,
    // which will avoid double counting the offset usage.
    qualifier.layoutXfbOffset = TQualifier::layoutXfbOffsetEnd;
}

// Calculate and save the offset of each block member, using the recursively
// defined block offset rules and the user-provided offset and align.
//
// Also, compute and save the total size of the block. For the block's size, arrayness
// is not taken into account, as each element is backed by a separate buffer.
//
void TParseContext::fixBlockUniformOffsets(TQualifier& qualifier, TTypeList& typeList)
{
    if (!qualifier.isUniformOrBuffer() && !qualifier.isTaskMemory())
        return;
    if (qualifier.layoutPacking != ElpStd140 && qualifier.layoutPacking != ElpStd430)
        return;

    int offset = 0;
    int memberSize;
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier& memberQualifier = typeList[member].type->getQualifier();
        const TSourceLoc& memberLoc = typeList[member].loc;

        // "When align is applied to an array, it effects only the start of the array, not the array's internal stride."

        // modify just the children's view of matrix layout, if there is one for this member
        TLayoutMatrix subMatrixLayout = typeList[member].type->getQualifier().layoutMatrix;
        int dummyStride;
        int memberAlignment = intermediate.getBaseAlignment(*typeList[member].type, memberSize, dummyStride, qualifier.layoutPacking == ElpStd140,
                                                            subMatrixLayout != ElmNone ? subMatrixLayout == ElmRowMajor : qualifier.layoutMatrix == ElmRowMajor);
        if (memberQualifier.hasOffset()) {
            // "The specified offset must be a multiple
            // of the base alignment of the type of the block member it qualifies, or a compile-time error results."
            if (! IsMultipleOfPow2(memberQualifier.layoutOffset, memberAlignment))
                error(memberLoc, "must be a multiple of the member's alignment", "offset", "");

            // GLSL: "It is a compile-time error to specify an offset that is smaller than the offset of the previous
            // member in the block or that lies within the previous member of the block"
            if (spvVersion.spv == 0) {
                if (memberQualifier.layoutOffset < offset)
                    error(memberLoc, "cannot lie in previous members", "offset", "");

                // "The offset qualifier forces the qualified member to start at or after the specified
                // integral-constant expression, which will be its byte offset from the beginning of the buffer.
                // "The actual offset of a member is computed as
                // follows: If offset was declared, start with that offset, otherwise start with the next available offset."
                offset = std::max(offset, memberQualifier.layoutOffset);
            } else {
                // TODO: Vulkan: "It is a compile-time error to have any offset, explicit or assigned,
                // that lies within another member of the block."

                offset = memberQualifier.layoutOffset;
            }
        }

        // "The actual alignment of a member will be the greater of the specified align alignment and the standard
        // (e.g., std140) base alignment for the member's type."
        if (memberQualifier.hasAlign())
            memberAlignment = std::max(memberAlignment, memberQualifier.layoutAlign);

        // "If the resulting offset is not a multiple of the actual alignment,
        // increase it to the first offset that is a multiple of
        // the actual alignment."
        RoundToPow2(offset, memberAlignment);
        typeList[member].type->getQualifier().layoutOffset = offset;
        offset += memberSize;
    }
}

// For an identifier that is already declared, add more qualification to it.
void TParseContext::addQualifierToExisting(const TSourceLoc& loc, TQualifier qualifier, const TString& identifier)
{
    TSymbol* symbol = symbolTable.find(identifier);
    if (! symbol) {
        error(loc, "identifier not previously declared", identifier.c_str(), "");
        return;
    }
    if (symbol->getAsFunction()) {
        error(loc, "cannot re-qualify a function name", identifier.c_str(), "");
        return;
    }

    if (qualifier.isAuxiliary() ||
        qualifier.isMemory() ||
        qualifier.isInterpolation() ||
        qualifier.hasLayout() ||
        qualifier.storage != EvqTemporary ||
        qualifier.precision != EpqNone) {
        error(loc, "cannot add storage, auxiliary, memory, interpolation, layout, or precision qualifier to an existing variable", identifier.c_str(), "");
        return;
    }

    // For read-only built-ins, add a new symbol for holding the modified qualifier.
    // This will bring up an entire block, if a block type has to be modified (e.g., gl_Position inside a block)
    if (symbol->isReadOnly())
        symbol = symbolTable.copyUp(symbol);

    if (qualifier.invariant) {
        if (intermediate.inIoAccessed(identifier))
            error(loc, "cannot change qualification after use", "invariant", "");
        symbol->getWritableType().getQualifier().invariant = true;
        invariantCheck(loc, symbol->getType().getQualifier());
    } else if (qualifier.noContraction) {
        if (intermediate.inIoAccessed(identifier))
            error(loc, "cannot change qualification after use", "precise", "");
        symbol->getWritableType().getQualifier().noContraction = true;
    } else if (qualifier.specConstant) {
        symbol->getWritableType().getQualifier().makeSpecConstant();
        if (qualifier.hasSpecConstantId())
            symbol->getWritableType().getQualifier().layoutSpecConstantId = qualifier.layoutSpecConstantId;
    } else
        warn(loc, "unknown requalification", "", "");
}

void TParseContext::addQualifierToExisting(const TSourceLoc& loc, TQualifier qualifier, TIdentifierList& identifiers)
{
    for (unsigned int i = 0; i < identifiers.size(); ++i)
        addQualifierToExisting(loc, qualifier, *identifiers[i]);
}

// Make sure 'invariant' isn't being applied to a non-allowed object.
void TParseContext::invariantCheck(const TSourceLoc& loc, const TQualifier& qualifier)
{
    if (! qualifier.invariant)
        return;

    bool pipeOut = qualifier.isPipeOutput();
    bool pipeIn = qualifier.isPipeInput();
    if (version >= 300 || (profile != EEsProfile && version >= 420)) {
        if (! pipeOut)
            error(loc, "can only apply to an output", "invariant", "");
    } else {
        if ((language == EShLangVertex && pipeIn) || (! pipeOut && ! pipeIn))
            error(loc, "can only apply to an output, or to an input in a non-vertex stage\n", "invariant", "");
    }
}

//
// Updating default qualifier for the case of a declaration with just a qualifier,
// no type, block, or identifier.
//
void TParseContext::updateStandaloneQualifierDefaults(const TSourceLoc& loc, const TPublicType& publicType)
{
    if (publicType.shaderQualifiers.vertices != TQualifier::layoutNotSet) {
#ifdef NV_EXTENSIONS
        assert(language == EShLangTessControl || language == EShLangGeometry || language == EShLangMeshNV);
#else
        assert(language == EShLangTessControl || language == EShLangGeometry);
#endif
        const char* id = (language == EShLangTessControl) ? "vertices" : "max_vertices";

        if (publicType.qualifier.storage != EvqVaryingOut)
            error(loc, "can only apply to 'out'", id, "");
        if (! intermediate.setVertices(publicType.shaderQualifiers.vertices))
            error(loc, "cannot change previously set layout value", id, "");

        if (language == EShLangTessControl)
            checkIoArraysConsistency(loc);
    }
#ifdef NV_EXTENSIONS
    if (publicType.shaderQualifiers.primitives != TQualifier::layoutNotSet) {
        assert(language == EShLangMeshNV);
        const char* id = "max_primitives";

        if (publicType.qualifier.storage != EvqVaryingOut)
            error(loc, "can only apply to 'out'", id, "");
        if (! intermediate.setPrimitives(publicType.shaderQualifiers.primitives))
            error(loc, "cannot change previously set layout value", id, "");
    }
#endif
    if (publicType.shaderQualifiers.invocations != TQualifier::layoutNotSet) {
        if (publicType.qualifier.storage != EvqVaryingIn)
            error(loc, "can only apply to 'in'", "invocations", "");
        if (! intermediate.setInvocations(publicType.shaderQualifiers.invocations))
            error(loc, "cannot change previously set layout value", "invocations", "");
    }
    if (publicType.shaderQualifiers.geometry != ElgNone) {
        if (publicType.qualifier.storage == EvqVaryingIn) {
            switch (publicType.shaderQualifiers.geometry) {
            case ElgPoints:
            case ElgLines:
            case ElgLinesAdjacency:
            case ElgTriangles:
            case ElgTrianglesAdjacency:
            case ElgQuads:
            case ElgIsolines:
#ifdef NV_EXTENSIONS
                if (language == EShLangMeshNV) {
                    error(loc, "cannot apply to input", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
                    break;
                }
#endif
                if (intermediate.setInputPrimitive(publicType.shaderQualifiers.geometry)) {
                    if (language == EShLangGeometry)
                        checkIoArraysConsistency(loc);
                } else
                    error(loc, "cannot change previously set input primitive", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
                break;
            default:
                error(loc, "cannot apply to input", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
            }
        } else if (publicType.qualifier.storage == EvqVaryingOut) {
            switch (publicType.shaderQualifiers.geometry) {
#ifdef NV_EXTENSIONS
            case ElgLines:
            case ElgTriangles:
                if (language != EShLangMeshNV) {
                    error(loc, "cannot apply to 'out'", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
                    break;
                }
#endif
                // Fall through
            case ElgPoints:
            case ElgLineStrip:
            case ElgTriangleStrip:
                if (! intermediate.setOutputPrimitive(publicType.shaderQualifiers.geometry))
                    error(loc, "cannot change previously set output primitive", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
                break;
            default:
                error(loc, "cannot apply to 'out'", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), "");
            }
        } else
            error(loc, "cannot apply to:", TQualifier::getGeometryString(publicType.shaderQualifiers.geometry), GetStorageQualifierString(publicType.qualifier.storage));
    }
    if (publicType.shaderQualifiers.spacing != EvsNone) {
        if (publicType.qualifier.storage == EvqVaryingIn) {
            if (! intermediate.setVertexSpacing(publicType.shaderQualifiers.spacing))
                error(loc, "cannot change previously set vertex spacing", TQualifier::getVertexSpacingString(publicType.shaderQualifiers.spacing), "");
        } else
            error(loc, "can only apply to 'in'", TQualifier::getVertexSpacingString(publicType.shaderQualifiers.spacing), "");
    }
    if (publicType.shaderQualifiers.order != EvoNone) {
        if (publicType.qualifier.storage == EvqVaryingIn) {
            if (! intermediate.setVertexOrder(publicType.shaderQualifiers.order))
                error(loc, "cannot change previously set vertex order", TQualifier::getVertexOrderString(publicType.shaderQualifiers.order), "");
        } else
            error(loc, "can only apply to 'in'", TQualifier::getVertexOrderString(publicType.shaderQualifiers.order), "");
    }
    if (publicType.shaderQualifiers.pointMode) {
        if (publicType.qualifier.storage == EvqVaryingIn)
            intermediate.setPointMode();
        else
            error(loc, "can only apply to 'in'", "point_mode", "");
    }
    for (int i = 0; i < 3; ++i) {
        if (publicType.shaderQualifiers.localSize[i] > 1) {
            if (publicType.qualifier.storage == EvqVaryingIn) {
                if (! intermediate.setLocalSize(i, publicType.shaderQualifiers.localSize[i]))
                    error(loc, "cannot change previously set size", "local_size", "");
                else {
                    int max = 0;
                    if (language == EShLangCompute) {
                        switch (i) {
                        case 0: max = resources.maxComputeWorkGroupSizeX; break;
                        case 1: max = resources.maxComputeWorkGroupSizeY; break;
                        case 2: max = resources.maxComputeWorkGroupSizeZ; break;
                        default: break;
                        }
                        if (intermediate.getLocalSize(i) > (unsigned int)max)
                            error(loc, "too large; see gl_MaxComputeWorkGroupSize", "local_size", "");
                    }
#ifdef NV_EXTENSIONS
                    else if (language == EShLangMeshNV) {
                        switch (i) {
                        case 0: max = resources.maxMeshWorkGroupSizeX_NV; break;
                        case 1: max = resources.maxMeshWorkGroupSizeY_NV; break;
                        case 2: max = resources.maxMeshWorkGroupSizeZ_NV; break;
                        default: break;
                        }
                        if (intermediate.getLocalSize(i) > (unsigned int)max)
                            error(loc, "too large; see gl_MaxMeshWorkGroupSizeNV", "local_size", "");
                    }
                    else if (language == EShLangTaskNV) {
                        switch (i) {
                        case 0: max = resources.maxTaskWorkGroupSizeX_NV; break;
                        case 1: max = resources.maxTaskWorkGroupSizeY_NV; break;
                        case 2: max = resources.maxTaskWorkGroupSizeZ_NV; break;
                        default: break;
                        }
                        if (intermediate.getLocalSize(i) > (unsigned int)max)
                            error(loc, "too large; see gl_MaxTaskWorkGroupSizeNV", "local_size", "");
                    }
#endif
                    else {
                        assert(0);
                    }

                    // Fix the existing constant gl_WorkGroupSize with this new information.
                    TVariable* workGroupSize = getEditableVariable("gl_WorkGroupSize");
                    if (workGroupSize != nullptr)
                        workGroupSize->getWritableConstArray()[i].setUConst(intermediate.getLocalSize(i));
                }
            } else
                error(loc, "can only apply to 'in'", "local_size", "");
        }
        if (publicType.shaderQualifiers.localSizeSpecId[i] != TQualifier::layoutNotSet) {
            if (publicType.qualifier.storage == EvqVaryingIn) {
                if (! intermediate.setLocalSizeSpecId(i, publicType.shaderQualifiers.localSizeSpecId[i]))
                    error(loc, "cannot change previously set size", "local_size", "");
            } else
                error(loc, "can only apply to 'in'", "local_size id", "");
            // Set the workgroup built-in variable as a specialization constant
            TVariable* workGroupSize = getEditableVariable("gl_WorkGroupSize");
            if (workGroupSize != nullptr)
                workGroupSize->getWritableType().getQualifier().specConstant = true;
        }
    }
    if (publicType.shaderQualifiers.earlyFragmentTests) {
        if (publicType.qualifier.storage == EvqVaryingIn)
            intermediate.setEarlyFragmentTests();
        else
            error(loc, "can only apply to 'in'", "early_fragment_tests", "");
    }
    if (publicType.shaderQualifiers.postDepthCoverage) {
        if (publicType.qualifier.storage == EvqVaryingIn)
            intermediate.setPostDepthCoverage();
        else
            error(loc, "can only apply to 'in'", "post_coverage_coverage", "");
    }
    if (publicType.shaderQualifiers.blendEquation) {
        if (publicType.qualifier.storage != EvqVaryingOut)
            error(loc, "can only apply to 'out'", "blend equation", "");
    }

#ifdef NV_EXTENSIONS
    if (publicType.shaderQualifiers.layoutDerivativeGroupQuads &&
        publicType.shaderQualifiers.layoutDerivativeGroupLinear) {
        error(loc, "cannot be both specified", "derivative_group_quadsNV and derivative_group_linearNV", "");
    }

    if (publicType.shaderQualifiers.layoutDerivativeGroupQuads) {
        if (publicType.qualifier.storage == EvqVaryingIn) {
            if ((intermediate.getLocalSize(0) & 1) ||
                (intermediate.getLocalSize(1) & 1))
                error(loc, "requires local_size_x and local_size_y to be multiple of two", "derivative_group_quadsNV", "");
            else
                intermediate.setLayoutDerivativeMode(LayoutDerivativeGroupQuads);
        }
        else
            error(loc, "can only apply to 'in'", "derivative_group_quadsNV", "");
    }
    if (publicType.shaderQualifiers.layoutDerivativeGroupLinear) {
        if (publicType.qualifier.storage == EvqVaryingIn) {
            if((intermediate.getLocalSize(0) *
                intermediate.getLocalSize(1) *
                intermediate.getLocalSize(2)) % 4 != 0)
                error(loc, "requires total group size to be multiple of four", "derivative_group_linearNV", "");
            else
                intermediate.setLayoutDerivativeMode(LayoutDerivativeGroupLinear);
        }
        else
            error(loc, "can only apply to 'in'", "derivative_group_linearNV", "");
    }
#endif 
    const TQualifier& qualifier = publicType.qualifier;

    if (qualifier.isAuxiliary() ||
        qualifier.isMemory() ||
        qualifier.isInterpolation() ||
        qualifier.precision != EpqNone)
        error(loc, "cannot use auxiliary, memory, interpolation, or precision qualifier in a default qualifier declaration (declaration with no type)", "qualifier", "");
    // "The offset qualifier can only be used on block members of blocks..."
    // "The align qualifier can only be used on blocks or block members..."
    if (qualifier.hasOffset() ||
        qualifier.hasAlign())
        error(loc, "cannot use offset or align qualifiers in a default qualifier declaration (declaration with no type)", "layout qualifier", "");

    layoutQualifierCheck(loc, qualifier);

    switch (qualifier.storage) {
    case EvqUniform:
        if (qualifier.hasMatrix())
            globalUniformDefaults.layoutMatrix = qualifier.layoutMatrix;
        if (qualifier.hasPacking())
            globalUniformDefaults.layoutPacking = qualifier.layoutPacking;
        break;
    case EvqBuffer:
        if (qualifier.hasMatrix())
            globalBufferDefaults.layoutMatrix = qualifier.layoutMatrix;
        if (qualifier.hasPacking())
            globalBufferDefaults.layoutPacking = qualifier.layoutPacking;
        break;
    case EvqVaryingIn:
        break;
    case EvqVaryingOut:
        if (qualifier.hasStream())
            globalOutputDefaults.layoutStream = qualifier.layoutStream;
        if (qualifier.hasXfbBuffer())
            globalOutputDefaults.layoutXfbBuffer = qualifier.layoutXfbBuffer;
        if (globalOutputDefaults.hasXfbBuffer() && qualifier.hasXfbStride()) {
            if (! intermediate.setXfbBufferStride(globalOutputDefaults.layoutXfbBuffer, qualifier.layoutXfbStride))
                error(loc, "all stride settings must match for xfb buffer", "xfb_stride", "%d", qualifier.layoutXfbBuffer);
        }
        break;
    default:
        error(loc, "default qualifier requires 'uniform', 'buffer', 'in', or 'out' storage qualification", "", "");
        return;
    }

    if (qualifier.hasBinding())
        error(loc, "cannot declare a default, include a type or full declaration", "binding", "");
    if (qualifier.hasAnyLocation())
        error(loc, "cannot declare a default, use a full declaration", "location/component/index", "");
    if (qualifier.hasXfbOffset())
        error(loc, "cannot declare a default, use a full declaration", "xfb_offset", "");
    if (qualifier.layoutPushConstant)
        error(loc, "cannot declare a default, can only be used on a block", "push_constant", "");
    if (qualifier.hasSpecConstantId())
        error(loc, "cannot declare a default, can only be used on a scalar", "constant_id", "");
#ifdef NV_EXTENSIONS
    if (qualifier.layoutShaderRecordNV)
        error(loc, "cannot declare a default, can only be used on a block", "shaderRecordNVX", "");
#endif
}

//
// Take the sequence of statements that has been built up since the last case/default,
// put it on the list of top-level nodes for the current (inner-most) switch statement,
// and follow that by the case/default we are on now.  (See switch topology comment on
// TIntermSwitch.)
//
void TParseContext::wrapupSwitchSubsequence(TIntermAggregate* statements, TIntermNode* branchNode)
{
    TIntermSequence* switchSequence = switchSequenceStack.back();

    if (statements) {
        if (switchSequence->size() == 0)
            error(statements->getLoc(), "cannot have statements before first case/default label", "switch", "");
        statements->setOperator(EOpSequence);
        switchSequence->push_back(statements);
    }
    if (branchNode) {
        // check all previous cases for the same label (or both are 'default')
        for (unsigned int s = 0; s < switchSequence->size(); ++s) {
            TIntermBranch* prevBranch = (*switchSequence)[s]->getAsBranchNode();
            if (prevBranch) {
                TIntermTyped* prevExpression = prevBranch->getExpression();
                TIntermTyped* newExpression = branchNode->getAsBranchNode()->getExpression();
                if (prevExpression == nullptr && newExpression == nullptr)
                    error(branchNode->getLoc(), "duplicate label", "default", "");
                else if (prevExpression != nullptr &&
                          newExpression != nullptr &&
                         prevExpression->getAsConstantUnion() &&
                          newExpression->getAsConstantUnion() &&
                         prevExpression->getAsConstantUnion()->getConstArray()[0].getIConst() ==
                          newExpression->getAsConstantUnion()->getConstArray()[0].getIConst())
                    error(branchNode->getLoc(), "duplicated value", "case", "");
            }
        }
        switchSequence->push_back(branchNode);
    }
}

//
// Turn the top-level node sequence built up of wrapupSwitchSubsequence9)
// into a switch node.
//
TIntermNode* TParseContext::addSwitch(const TSourceLoc& loc, TIntermTyped* expression, TIntermAggregate* lastStatements)
{
    profileRequires(loc, EEsProfile, 300, nullptr, "switch statements");
    profileRequires(loc, ENoProfile, 130, nullptr, "switch statements");

    wrapupSwitchSubsequence(lastStatements, nullptr);

    if (expression == nullptr ||
        (expression->getBasicType() != EbtInt && expression->getBasicType() != EbtUint) ||
        expression->getType().isArray() || expression->getType().isMatrix() || expression->getType().isVector())
            error(loc, "condition must be a scalar integer expression", "switch", "");

    // If there is nothing to do, drop the switch but still execute the expression
    TIntermSequence* switchSequence = switchSequenceStack.back();
    if (switchSequence->size() == 0)
        return expression;

    if (lastStatements == nullptr) {
        // This was originally an ERRROR, because early versions of the specification said
        // "it is an error to have no statement between a label and the end of the switch statement."
        // The specifications were updated to remove this (being ill-defined what a "statement" was),
        // so, this became a warning.  However, 3.0 tests still check for the error.
        if (profile == EEsProfile && version <= 300 && ! relaxedErrors())
            error(loc, "last case/default label not followed by statements", "switch", "");
        else
            warn(loc, "last case/default label not followed by statements", "switch", "");

        // emulate a break for error recovery
        lastStatements = intermediate.makeAggregate(intermediate.addBranch(EOpBreak, loc));
        lastStatements->setOperator(EOpSequence);
        switchSequence->push_back(lastStatements);
    }

    TIntermAggregate* body = new TIntermAggregate(EOpSequence);
    body->getSequence() = *switchSequenceStack.back();
    body->setLoc(loc);

    TIntermSwitch* switchNode = new TIntermSwitch(expression, body);
    switchNode->setLoc(loc);

    return switchNode;
}

} // end namespace glslang

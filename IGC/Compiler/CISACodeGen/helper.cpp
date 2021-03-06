/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/CISACodeGen.h"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Analysis/ValueTracking.h>

#include <llvmWrapper/Support/KnownBits.h>
#include <llvmWrapper/IR/Instructions.h>

#include "common/LLVMWarningsPop.hpp"

#include "GenISAIntrinsics/GenIntrinsicInst.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"

#include "common/secure_mem.h"

#include <stack>

using namespace llvm;
using namespace GenISAIntrinsic;

/************************************************************************
This file contains helper functions for the code generator
Many functions use X-MACRO, that allow us to separate data about encoding
to the logic of the helper functions

************************************************************************/

namespace IGC
{
typedef union _gfxResourceAddrSpace
{
    struct _bits
    {
        unsigned int       bufId                 : 16;
        unsigned int       bufType               : 4;
        unsigned int       indirect              : 1;     // bool
        unsigned int       reserved              : 11;
    } bits;
    uint32_t u32Val;
} GFXResourceAddrSpace;

unsigned EncodeAS4GFXResource(
    const llvm::Value& bufIdx,
    BufferType bufType,
    unsigned uniqueIndAS)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = 0;
    assert( (bufType+1) < 16 );
    temp.bits.bufType = bufType + 1;
    if (bufType == SLM)
    {
        return ADDRESS_SPACE_LOCAL; // We use addrspace 3 for SLM
    }
    else if (bufType == STATELESS_READONLY)
    {
        return ADDRESS_SPACE_CONSTANT; 
    }
    else if (bufType == STATELESS)
    {
        return ADDRESS_SPACE_GLOBAL;
    }
    else if (llvm::isa<llvm::ConstantInt>(&bufIdx))
    {
        unsigned int bufId = (unsigned int)(llvm::cast<llvm::ConstantInt>(&bufIdx)->getZExtValue());
        assert( bufId < (1 << 31) );
        temp.bits.bufId = bufId;
        return temp.u32Val;
    }

    // if it is indirect-buf, it is front-end's job to give a proper(unique) address-space per access
    temp.bits.bufId = uniqueIndAS;
    temp.bits.indirect = 1;
    return temp.u32Val;
}

unsigned SetBufferAsBindless(unsigned addressSpaceOfPtr, BufferType bufferType)
{
    GFXResourceAddrSpace temp = {};
    temp.u32Val = addressSpaceOfPtr;

    // Mark buffer as it is bindless for further processing
    if (bufferType == BufferType::RESOURCE ||
        bufferType == BufferType::CONSTANT_BUFFER ||
        bufferType == BufferType::UAV)
    {
        temp.bits.bufType = IGC::BINDLESS + 1;
    }
    else if (bufferType == BufferType::SAMPLER)
    {
        temp.bits.bufType = IGC::BINDLESS_SAMPLER + 1;
    }
    else
    {
        // other types of buffers shouldn't reach this part.
        assert(0);
    }

    return temp.u32Val;
}

bool UsesTypedConstantBuffer(CodeGenContext* pContext)
{
    if(pContext->m_DriverInfo.UsesTypedConstantBuffers3D() &&
        pContext->type != ShaderType::COMPUTE_SHADER)
    {
        return true;
    }
    if(pContext->m_DriverInfo.UsesTypedConstantBuffersGPGPU() &&
        pContext->type == ShaderType::COMPUTE_SHADER)
    {
        return true;
    }
    return false;
}

///
/// if you want resource-dimension, use GetBufferDimension()
///
BufferType DecodeAS4GFXResource(unsigned addrSpace, bool& directIndexing, unsigned& bufId)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = addrSpace;

    directIndexing = (temp.bits.indirect == 0);
    bufId = temp.bits.bufId;

    if(addrSpace == ADDRESS_SPACE_LOCAL)
    {
        return SLM;
    }
    unsigned bufType = temp.bits.bufType - 1;
    if (bufType < BUFFER_TYPE_UNKNOWN)
    {
        return (BufferType)bufType;
    }
    return BUFFER_TYPE_UNKNOWN;
}
///
/// returns constant buffer load offset
///
int getConstantBufferLoadOffset(llvm::LoadInst *ld)
{
    int offset = 0;
    Value* ptr = ld->getPointerOperand();
    if (isa<ConstantPointerNull>(ptr))
    {
        offset = 0;
    }
    else if (IntToPtrInst* itop = dyn_cast<IntToPtrInst>(ptr))
    {
        ConstantInt* ci = dyn_cast<ConstantInt>(
            itop->getOperand(0));
        if (ci)
        {
            offset = int_cast<unsigned>(ci->getZExtValue());
        }
    }
    else if (ConstantExpr* itop = dyn_cast<ConstantExpr>(ptr))
    {
        if (itop->getOpcode() == Instruction::IntToPtr)
        {
            offset = int_cast<unsigned>(
                cast<ConstantInt>(itop->getOperand(0))->getZExtValue());
        }
    }
    return offset;
}
///
/// returns info if direct addressing is used
///
bool IsDirectIdx(unsigned addrSpace)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = addrSpace;
    return (temp.bits.indirect == 0);
}

llvm::LoadInst* cloneLoad(llvm::LoadInst *Orig, llvm::Value *Ptr)
{
    llvm::LoadInst *LI = new llvm::LoadInst(Ptr, "", Orig);
    LI->setVolatile(Orig->isVolatile());
    LI->setAlignment(Orig->getAlignment());
    if (LI->isAtomic())
    {
        LI->setAtomic(Orig->getOrdering(), IGCLLVM::getSyncScopeID(Orig));
    }
    // Clone metadata
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> MDs;
    Orig->getAllMetadata(MDs);
    for (llvm::SmallVectorImpl<std::pair<unsigned, llvm::MDNode *> >::iterator
         MI = MDs.begin(), ME = MDs.end(); MI != ME; ++MI)
    {
        LI->setMetadata(MI->first, MI->second);
    }
    return LI;
}

llvm::StoreInst* cloneStore(llvm::StoreInst *Orig, llvm::Value *Val, llvm::Value *Ptr)
{
    llvm::StoreInst *SI = new llvm::StoreInst(Val, Ptr, Orig);
    SI->setVolatile(Orig->isVolatile());
    SI->setAlignment(Orig->getAlignment());
    if (SI->isAtomic())
    {
        SI->setAtomic(Orig->getOrdering(), IGCLLVM::getSyncScopeID(Orig));
    }
    // Clone metadata
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> MDs;
    Orig->getAllMetadata(MDs);
    for (llvm::SmallVectorImpl<std::pair<unsigned, llvm::MDNode *> >::iterator
         MI = MDs.begin(), ME = MDs.end(); MI != ME; ++MI)
    {
        SI->setMetadata(MI->first, MI->second);
    }
    return SI;
}

// Create a ldraw from a load instruction
Value* CreateLoadRawIntrinsic(LoadInst *inst, Instruction* bufPtr, Value *offsetVal)
{
	Module* module = inst->getParent()->getParent()->getParent();
    Function* func = nullptr;
    IRBuilder<> builder(inst);

    llvm::Type* tys[2];
    tys[0] = inst->getType();
    tys[1] = bufPtr->getType();
    func = GenISAIntrinsic::getDeclaration(module, inst->getType()->isVectorTy() ? llvm::GenISAIntrinsic::GenISA_ldrawvector_indexed : llvm::GenISAIntrinsic::GenISA_ldraw_indexed, tys);

	unsigned alignment = (inst->getType()->getScalarSizeInBits() / 8);
	if (inst->getAlignment() > 0)
	{
		alignment = inst->getAlignment();
	}

	Value* attr[] =
	{
		bufPtr,
		offsetVal,
		builder.getInt32(alignment)
	};
	Value* ld = builder.CreateCall(func, attr);
	assert(ld->getType() == inst->getType());
	return ld;
}

// Creates a storeraw from a store instruction
Value* CreateStoreRawIntrinsic(StoreInst *inst, Instruction* bufPtr, Value* offsetVal)
{
	Module* module = inst->getParent()->getParent()->getParent();
	Function* func = nullptr;
	IRBuilder<> builder(inst);
	Value *storeVal = inst->getValueOperand();
	if (storeVal->getType()->isVectorTy())
	{
		llvm::Type* tys[2];
		tys[0] = bufPtr->getType();
		tys[1] = inst->getValueOperand()->getType();
		func = GenISAIntrinsic::getDeclaration(module, llvm::GenISAIntrinsic::GenISA_storerawvector_indexed, tys);
	}
	else
	{
		llvm::Type* dataType = storeVal->getType();
		assert(dataType->getPrimitiveSizeInBits() == 16 || dataType->getPrimitiveSizeInBits() == 32);

		llvm::Type* types[2] = {
			bufPtr->getType(),
			storeVal->getType() };

		func = GenISAIntrinsic::getDeclaration(module, llvm::GenISAIntrinsic::GenISA_storeraw_indexed, types);
	}
	Value* attr[] =
	{
		bufPtr,
		offsetVal,
		storeVal
	};
	Value* st = builder.CreateCall(func, attr);
	return st;
}

///
/// Tries to trace a resource pointer (texture/sampler/buffer) back to
/// the pointer source. Also returns a vector of all instructions in the search path
///
Value* TracePointerSource(Value* resourcePtr, bool hasBranching, bool fillList, 
    std::vector<Value*> &instList, llvm::SmallSet<PHINode*, 8>& visitedPHIs)
{
    Value* srcPtr = nullptr;
    Value* baseValue = resourcePtr;

    while (true)
    {
        if (fillList)
        {
            instList.push_back(baseValue);
        }

        unsigned bufId = 0;
        IGC::BufferType bufTy = BUFFER_TYPE_UNKNOWN;
        IGC::BufferAccessType accessTy = BUFFER_ACCESS_TYPE_UNKNOWN;
        if (GetResourcePointerInfo(baseValue, bufId, bufTy, accessTy))
        {
            srcPtr = baseValue;
            break;
        }
        else if (isa<Argument>(baseValue))
        {
            // For compute, resource comes from the kernel args
            srcPtr = baseValue;
            break;
        }
        else if (CastInst* inst = dyn_cast<CastInst>(baseValue))
        {
            baseValue = inst->getOperand(0);
        }
        else if (GetElementPtrInst* inst = dyn_cast<GetElementPtrInst>(baseValue))
        {
            baseValue = inst->getOperand(0);
        }
        else if (PHINode* inst = dyn_cast<PHINode>(baseValue))
        {
            if (visitedPHIs.count(inst) != 0)
            { 
                // stop if we've seen this phi node before
				return baseValue;
            }
            visitedPHIs.insert(inst);
            for(unsigned int i = 0; i < inst->getNumIncomingValues(); ++i)
            {
                // All phi paths must be trace-able and trace back to the same source
                Value* phiVal = inst->getIncomingValue(i);
				std::vector<Value*> splitList;
                Value* phiSrcPtr = TracePointerSource(phiVal, true, fillList, splitList, visitedPHIs);
                if (phiSrcPtr == nullptr)
                {
					// Incoming value not trace-able, bail out.
                    return nullptr;
                }
				else if (isa<PHINode>(phiSrcPtr) && phiSrcPtr == baseValue)
				{
					// Found a loop in one of the phi paths. We can still trace as long as all the other paths match
					continue;
				}
				else if (srcPtr == nullptr)
				{
					// Found a path to the source pointer. We only save the instructions used in this path
					srcPtr = phiSrcPtr;
					instList.insert(instList.end(), splitList.begin(), splitList.end());
				}
                else if (srcPtr != phiSrcPtr)
                {
					// The source pointers have diverged. Bail out.
                    return nullptr;
                }
            }
            break;
        }
        else if (SelectInst *inst = dyn_cast<SelectInst>(baseValue))
        {
            if (hasBranching)
            {
                // only allow a single branching instruction to be supported for now
                // if both select and PHI are present, or there are multiples of each, we bail
                break;
            }
            // Trace both operands of the select instruction. Both have to be traced back to the same
            // source pointer, otherwise we can't determine which one to use.
            Value* selectSrc0 = TracePointerSource(inst->getOperand(1), true, fillList, instList, visitedPHIs);
            Value* selectSrc1 = TracePointerSource(inst->getOperand(2), true, false, instList, visitedPHIs);
            if (selectSrc0 && selectSrc1 && selectSrc0 == selectSrc1)
            {
                srcPtr = selectSrc0;
                break;
            }
            return nullptr;
        }
        else
        {
            // Unsupported instruction in search chain. Don't continue.
            break;
        }
    }
    return srcPtr;
}

///
/// Only trace the GetBufferPtr instruction (ignore GetElementPtr)
///
Value* TracePointerSource(Value* resourcePtr)
{
    std::vector<Value*> tempList; //unused
    llvm::SmallSet<PHINode*, 8> visitedPHIs;
    return TracePointerSource(resourcePtr, false, false, tempList, visitedPHIs);
}

Value* TracePointerSource(Value* resourcePtr, bool hasBranching, bool fillList, std::vector<Value*> &instList)
{
    llvm::SmallSet<PHINode*, 8> visitedPHIs;
    return TracePointerSource(resourcePtr, hasBranching, fillList, instList, visitedPHIs);
}

static BufferAccessType getDefaultAccessType(BufferType bufTy)
{
    switch (bufTy)
    {
    case BufferType::CONSTANT_BUFFER:
    case BufferType::RESOURCE:
    case BufferType::BINDLESS_READONLY:
    case BufferType::STATELESS_READONLY:
    case BufferType::SAMPLER:
        return BufferAccessType::ACCESS_READ;

    case BufferType::UAV:
    case BufferType::SLM:
    case BufferType::POINTER:
    case BufferType::BINDLESS:
    case BufferType::STATELESS:
        return BufferAccessType::ACCESS_READWRITE;

    case BufferType::RENDER_TARGET:
        return BufferAccessType::ACCESS_WRITE;

    default:
        assert(false && "Invalid buffer type");
        return BufferAccessType::ACCESS_READWRITE;
    }
}

bool GetResourcePointerInfo(Value* srcPtr, unsigned &resID, IGC::BufferType &resTy, BufferAccessType& accessTy)
{
    accessTy = BufferAccessType::ACCESS_READWRITE;
	if (GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(srcPtr))
    {
        // For bindless pointers with encoded metadata
        if(inst->getIntrinsicID() == GenISAIntrinsic::GenISA_RuntimeValue)
        {
            if (inst->hasOperandBundles())
            {
                auto resIDBundle = inst->getOperandBundle("resID");
                auto resTyBundle = inst->getOperandBundle("resTy");
                auto accessTyBundle = inst->getOperandBundle("accessTy");
                if (resIDBundle && resTyBundle)
                {
                    resID = (unsigned) (cast<ConstantInt>(resIDBundle->Inputs.front()))->getZExtValue();
                    resTy = (BufferType) (cast<ConstantInt>(resTyBundle->Inputs.front()))->getZExtValue();

                    if (accessTyBundle)
                        accessTy = (BufferAccessType) (cast<ConstantInt>(accessTyBundle->Inputs.front()))->getZExtValue();
                    else
                        accessTy = getDefaultAccessType(resTy);
                    return true;
                }
            }
        }
		// For GetBufferPtr instructions with buffer info in the operands
        else if(inst->getIntrinsicID() == GenISAIntrinsic::GenISA_GetBufferPtr)
		{
            Value *bufIdV = inst->getOperand(0);
            Value *bufTyV = inst->getOperand(1);
            if (isa<ConstantInt>(bufIdV) && isa<ConstantInt>(bufTyV))
            {
                resID = (unsigned)(cast<ConstantInt>(bufIdV)->getZExtValue());
                resTy = (IGC::BufferType)(cast<ConstantInt>(bufTyV)->getZExtValue());
				accessTy = getDefaultAccessType(resTy);
                return true;
            }
        }
    }
    return false;
}

///
/// Replaces oldPtr with newPtr in a sample/ld intrinsic's argument list. The new instrinsic will
/// replace the old one in the module
///
void ChangePtrTypeInIntrinsic(llvm::GenIntrinsicInst *&pIntr, llvm::Value* oldPtr, llvm::Value* newPtr)
{
    llvm::Module *pModule = pIntr->getParent()->getParent()->getParent();
    llvm::Function *pCalledFunc = pIntr->getCalledFunction();

    // Look at the intrinsic and figure out which pointer to change
    int num_ops = pIntr->getNumArgOperands();
    llvm::SmallVector<llvm::Value*, 5> args;

    for(int i = 0; i < num_ops; ++i)
    {
        if(pIntr->getArgOperand(i) == oldPtr)
            args.push_back(newPtr);
        else
            args.push_back(pIntr->getArgOperand(i));
    }

    llvm::Function *pNewIntr = nullptr;
    llvm::SmallVector<llvm::Type*, 4> overloadedTys;
    GenISAIntrinsic::ID id = pIntr->getIntrinsicID();
    switch(id)
    {
        case llvm::GenISAIntrinsic::GenISA_ldmcsptr:
            overloadedTys.push_back(pCalledFunc->getReturnType());
            overloadedTys.push_back(args[0]->getType());
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_ldptr:
        case llvm::GenISAIntrinsic::GenISA_ldmsptr:
            overloadedTys.push_back(pCalledFunc->getReturnType());
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_resinfoptr:
        case llvm::GenISAIntrinsic::GenISA_readsurfaceinfoptr:
        case llvm::GenISAIntrinsic::GenISA_sampleinfoptr:
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_sampleptr:
        case llvm::GenISAIntrinsic::GenISA_sampleBptr:
        case llvm::GenISAIntrinsic::GenISA_sampleCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleDptr:
        case llvm::GenISAIntrinsic::GenISA_sampleLptr:
        case llvm::GenISAIntrinsic::GenISA_sampleBCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleDCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleLCptr:
        case llvm::GenISAIntrinsic::GenISA_gather4ptr:
        case llvm::GenISAIntrinsic::GenISA_gather4POptr:
        case llvm::GenISAIntrinsic::GenISA_gather4Cptr:
        case llvm::GenISAIntrinsic::GenISA_gather4POCptr:
        case llvm::GenISAIntrinsic::GenISA_lodptr:
        {
            // Figure out the intrinsic operands for texture & sampler
            llvm::Value *pTextureValue = nullptr, *pSamplerValue = nullptr;
            getTextureAndSamplerOperands(pIntr, pTextureValue, pSamplerValue);

            overloadedTys.push_back(pCalledFunc->getReturnType());
            overloadedTys.push_back(pIntr->getOperand(0)->getType());

            if(pTextureValue == oldPtr)
            {
                overloadedTys.push_back(newPtr->getType());
                if(pSamplerValue)
                {
                    // Samplerless messages will not have sampler in signature.
                    overloadedTys.push_back(pSamplerValue->getType());
                }
            }
            else if(pSamplerValue == oldPtr)
            {
                overloadedTys.push_back(pTextureValue->getType());
                overloadedTys.push_back(newPtr->getType());
            }

            break;
        }
        case llvm::GenISAIntrinsic::GenISA_typedread:
        case llvm::GenISAIntrinsic::GenISA_typedwrite:
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_intatomicraw:
        case llvm::GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
            overloadedTys.push_back(pIntr->getType());
            overloadedTys.push_back(newPtr->getType());
            if(id == GenISAIntrinsic::GenISA_intatomicrawA64)
            {
                args[0] = args[1];
                args[1] = CastInst::CreatePointerCast(args[1], Type::getInt32Ty(pModule->getContext()), "", pIntr);
                id = GenISAIntrinsic::GenISA_intatomicraw;
            }
            else if(id == GenISAIntrinsic::GenISA_icmpxchgatomicrawA64)
            {
                args[0] = args[1];
                args[1] = CastInst::CreatePointerCast(args[1], Type::getInt32Ty(pModule->getContext()), "", pIntr);
                id = GenISAIntrinsic::GenISA_icmpxchgatomicraw;
            }
            break;
        default:
            assert(0 && "Unknown intrinsic encountered while changing pointer types");
            break;
    }

    pNewIntr = llvm::GenISAIntrinsic::getDeclaration(
                                                     pModule,
                                                     id,
                                                     overloadedTys);

    llvm::CallInst *pNewCall = llvm::CallInst::Create(pNewIntr, args, "", pIntr);

    pIntr->replaceAllUsesWith(pNewCall);
    pIntr->eraseFromParent();

    pIntr = llvm::cast<llvm::GenIntrinsicInst>(pNewCall);
}

///
/// Returns the sampler/texture pointers for resource access intrinsics
///
void getTextureAndSamplerOperands(llvm::GenIntrinsicInst *pIntr, llvm::Value*& pTextureValue, llvm::Value*& pSamplerValue)
{
    if (llvm::SamplerLoadIntrinsic *pSamplerLoadInst = llvm::dyn_cast<llvm::SamplerLoadIntrinsic>(pIntr))
    {
        pTextureValue = pSamplerLoadInst->getTextureValue();
        pSamplerValue = nullptr;
    }
    else if (llvm::SampleIntrinsic *pSampleInst = llvm::dyn_cast<llvm::SampleIntrinsic>(pIntr))
    {
        pTextureValue = pSampleInst->getTextureValue();
        pSamplerValue = pSampleInst->getSamplerValue();
    }
    else if (llvm::SamplerGatherIntrinsic *pGatherInst = llvm::dyn_cast<llvm::SamplerGatherIntrinsic>(pIntr))
    {
        pTextureValue = pGatherInst->getTextureValue();
        pSamplerValue = pGatherInst->getSamplerValue();
    }
    else
    {
        pTextureValue = nullptr;
        pSamplerValue = nullptr;
        switch (pIntr->getIntrinsicID())
        {
            case llvm::GenISAIntrinsic::GenISA_resinfoptr:
            case llvm::GenISAIntrinsic::GenISA_readsurfaceinfoptr:
            case llvm::GenISAIntrinsic::GenISA_sampleinfoptr:
            case llvm::GenISAIntrinsic::GenISA_typedwrite:
            case llvm::GenISAIntrinsic::GenISA_typedread:
                pTextureValue = pIntr->getOperand(0);
                break;
            default:
                break;
        }
    }
}

EOPCODE GetOpCode(const llvm::Instruction* inst)
{
    if(const GenIntrinsicInst *CI = dyn_cast<GenIntrinsicInst>( inst ))
    {
        unsigned ID = CI->getIntrinsicID();
        return (EOPCODE)(OPCODE(ID,e_Intrinsic));
    }
    else if(const IntrinsicInst *CI = llvm::dyn_cast<llvm::IntrinsicInst>( inst ))
    {
        unsigned ID = CI->getIntrinsicID();
        return (EOPCODE)(OPCODE(ID,e_Intrinsic));
    }
    return (EOPCODE)(OPCODE(inst->getOpcode(),e_Instruction));
}

BufferType GetBufferType(uint addrSpace)
{
    bool directIndexing = false;
    unsigned int bufId = 0;
    return DecodeAS4GFXResource(addrSpace, directIndexing, bufId);
}

bool IsReadOnlyLoadDirectCB(llvm::Instruction *pLLVMInst,
    uint& cbId, llvm::Value* &eltPtrVal, BufferType& bufType)
{
    LoadInst *inst = dyn_cast<LoadInst>(pLLVMInst);
    if(!inst)
    {
        return false;
    }
    bool isInvLoad = inst->getMetadata(LLVMContext::MD_invariant_load) != nullptr;
    unsigned as = inst->getPointerAddressSpace();
    bool directBuf;
    // cbId gets filled in the following call;
    bufType = IGC::DecodeAS4GFXResource(as, directBuf, cbId);
    if((bufType == CONSTANT_BUFFER || bufType == RESOURCE || isInvLoad) && directBuf)
    {
        Value *ptrVal = inst->getPointerOperand();
        // skip bitcast and find the real address computation
        while(isa<BitCastInst>(ptrVal))
        {
            ptrVal = cast<BitCastInst>(ptrVal)->getOperand(0);
        }
        if(isa<ConstantPointerNull>(ptrVal) ||
            isa<IntToPtrInst>(ptrVal) ||
            isa<GetElementPtrInst>(ptrVal) ||
            isa<ConstantExpr>(ptrVal) ||
            isa<Argument>(ptrVal))
        {
            eltPtrVal = ptrVal;
            return true;
        }
    }
    return false;
}

bool IsLoadFromDirectCB(llvm::Instruction *pLLVMInst, uint& cbId, llvm::Value* &eltPtrVal)
{
    BufferType bufType = BUFFER_TYPE_UNKNOWN;
    bool isReadOnly = IsReadOnlyLoadDirectCB(pLLVMInst, cbId, eltPtrVal, bufType);
    return isReadOnly && bufType == CONSTANT_BUFFER;
}
    
/// this is texture-load not buffer-load
bool isLdInstruction(llvm::Instruction* inst)
{
    return isa<SamplerLoadIntrinsic>(inst);
}

// function returns the position of the texture operand for sample/ld instructions
llvm::Value* getTextureIndexArgBasedOnOpcode(llvm::Instruction* inst)
{
    if (isLdInstruction(inst))
    {
        return cast<SamplerLoadIntrinsic>(inst)->getTextureValue();
    }
    else if (isSampleInstruction(inst))
    {
        return cast<SampleIntrinsic>(inst)->getTextureValue();
    }
    else if (isGather4Instruction(inst))
    {
        return cast<SamplerGatherIntrinsic>(inst)->getTextureValue();
    }

    return nullptr;
}

int findSampleInstructionTextureIdx(llvm::Instruction* inst)
{
    // fetch the textureArgIdx.
    Value* ptr = getTextureIndexArgBasedOnOpcode(inst);
    unsigned textureIdx = -1;

    if (ptr && ptr->getType()->isPointerTy())
    {
        BufferType bufType = BUFFER_TYPE_UNKNOWN;
        if (!(isa<GenIntrinsicInst>(ptr) &&
            cast<GenIntrinsicInst>(ptr)->getIntrinsicID() == GenISAIntrinsic::GenISA_GetBufferPtr))
        {
            uint as = ptr->getType()->getPointerAddressSpace();
            bool directIndexing;
            bufType = DecodeAS4GFXResource(as, directIndexing, textureIdx);
            if (bufType == UAV)
            {
                // dont do any clustering on read/write images
                textureIdx = -1;
            }
        }
    }
    else if (ptr)
    {
        if (llvm::dyn_cast<llvm::ConstantInt>(ptr))
        {
            textureIdx = int_cast<unsigned>(GetImmediateVal(ptr));
        }
    }

    return textureIdx;
}

bool isSampleLoadGather4InfoInstruction(llvm::Instruction* inst)
{
    if (isa<GenIntrinsicInst>(inst))
    {
        switch ((cast<GenIntrinsicInst>(inst))->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_sampleptr:
        case GenISAIntrinsic::GenISA_sampleBptr:
        case GenISAIntrinsic::GenISA_sampleCptr:
        case GenISAIntrinsic::GenISA_sampleDptr:
        case GenISAIntrinsic::GenISA_sampleDCptr:
        case GenISAIntrinsic::GenISA_sampleLptr:
        case GenISAIntrinsic::GenISA_sampleLCptr:
        case GenISAIntrinsic::GenISA_sampleBCptr:
        case GenISAIntrinsic::GenISA_lodptr:
        case GenISAIntrinsic::GenISA_ldptr:
        case GenISAIntrinsic::GenISA_ldmsptr:
        case GenISAIntrinsic::GenISA_ldmsptr16bit:
        case GenISAIntrinsic::GenISA_ldmcsptr:
        case GenISAIntrinsic::GenISA_sampleinfoptr:
        case GenISAIntrinsic::GenISA_resinfoptr:
        case GenISAIntrinsic::GenISA_gather4ptr:
        case GenISAIntrinsic::GenISA_gather4Cptr:
        case GenISAIntrinsic::GenISA_gather4POptr:
        case GenISAIntrinsic::GenISA_gather4POCptr:
            return true;
        default:
            return false;
        }
    }

    return false;
}

bool isSampleInstruction(llvm::Instruction* inst)
{
    return isa<SampleIntrinsic>(inst);
}

bool isInfoInstruction(llvm::Instruction* inst)
{
    return isa<InfoIntrinsic>(inst);
}

bool isGather4Instruction(llvm::Instruction* inst)
{
    return isa<SamplerGatherIntrinsic>(inst);
}

bool IsMediaIOIntrinsic(llvm::Instruction* inst)
{
    if (auto *pGI = dyn_cast<llvm::GenIntrinsicInst>(inst))
    {
        GenISAIntrinsic::ID id = pGI->getIntrinsicID();

        return id == GenISAIntrinsic::GenISA_MediaBlockRead ||
            id == GenISAIntrinsic::GenISA_MediaBlockWrite;
    }

    return false;
}

bool isSubGroupIntrinsic(const llvm::Instruction *I)
{
    const GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(I);
    if (!GII)
        return false;

    switch (GII->getIntrinsicID())
    {
    case GenISAIntrinsic::GenISA_WaveShuffleIndex:
    case GenISAIntrinsic::GenISA_simdShuffleDown:
    case GenISAIntrinsic::GenISA_simdBlockRead:
    case GenISAIntrinsic::GenISA_simdBlockWrite:
    case GenISAIntrinsic::GenISA_simdMediaBlockRead:
    case GenISAIntrinsic::GenISA_simdMediaBlockWrite:
    case GenISAIntrinsic::GenISA_MediaBlockWrite:
    case GenISAIntrinsic::GenISA_MediaBlockRead:
        return true;
    default:
        return false;
    }
}

bool isURBWriteIntrinsic(const llvm::Instruction *I)
{
    const GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(I);
    if (!GII)
        return false;

    return GII->getIntrinsicID() == GenISA_URBWrite;
  
}

bool isReadInput(llvm::Instruction *pLLVMInstr);

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return modifiers;
bool SupportsModifier(llvm::Instruction* inst)
{
    if(llvm::CmpInst* cmp = dyn_cast<llvm::ICmpInst>(inst))
    {
        // special case, cmp supports modifier unless it is unsigned
        return !cmp->isUnsigned();
    }
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return sat;
bool SupportsSaturate(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        break;
    }
    return false;
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return pred;
bool SupportsPredicate(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return condMod;
bool SupportsCondModifier(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return regioning;
bool SupportsRegioning(llvm::Instruction* inst)
{
    switch (GetOpCode(inst))
    {
#include "opCode.h"
    default:
        break;
    }
    return false;
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return mathIntrinsic;
bool IsMathIntrinsic(EOPCODE opcode)
{
    switch(opcode)
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return atomicIntrinsic;
bool IsAtomicIntrinsic(EOPCODE opcode)
{
    switch (opcode)
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

// for now just include shuffle, reduce and scan,
// which have simd32 implementations and should not be split into two instances
bool IsSubGroupIntrinsicWithSimd32Implementation(EOPCODE opcode)
{
    return (opcode == llvm_waveAll || 
            opcode == llvm_wavePrefix || 
            opcode == llvm_waveShuffleIndex);
}


bool IsGradientIntrinsic(EOPCODE opcode)
{
    return(opcode == llvm_gradientX ||
        opcode == llvm_gradientY ||
        opcode == llvm_gradientXfine ||
        opcode == llvm_gradientYfine);
}

bool ComputesGradient(llvm::Instruction *inst)
{
    llvm::SampleIntrinsic *sampleInst = dyn_cast<llvm::SampleIntrinsic>(inst);
    if (sampleInst && sampleInst->IsDerivative())
    {
        return true;
    }
    if (IsGradientIntrinsic(GetOpCode(inst)))
    {
        return true;
    }
    return false;
}

llvm::Value* ExtractElementFromInsertChain(llvm::Value *inst, int pos)
{

    llvm::ConstantDataVector *cstV = llvm::dyn_cast<llvm::ConstantDataVector>(inst);
    if (cstV != NULL) {
        return cstV->getElementAsConstant(pos);
    }

    llvm::InsertElementInst *ie = llvm::dyn_cast<llvm::InsertElementInst>(inst);
    while (ie != NULL) {
        int64_t iOffset = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))->getSExtValue();
        assert(iOffset>=0);
        if (iOffset == pos) {
            return ie->getOperand(1);
        }
        llvm::Value *insertBase = ie->getOperand(0);
        ie = llvm::dyn_cast<llvm::InsertElementInst>(insertBase);
    }
    return NULL;
}

bool ExtractVec4FromInsertChain(llvm::Value *inst, llvm::Value *elem[4], llvm::SmallVector<llvm::Instruction*, 10> &instructionToRemove)
{
    llvm::ConstantDataVector *cstV = llvm::dyn_cast<llvm::ConstantDataVector>(inst);
    if (cstV != NULL) {
        assert(cstV->getNumElements() == 4);
        for (int i = 0; i < 4; i++) {
            elem[i] = cstV->getElementAsConstant(i);
        }
        return true;
    }

    for (int i = 0; i<4; i++) {
        elem[i] = NULL;
    }
    
    int count = 0;
    llvm::InsertElementInst *ie = llvm::dyn_cast<llvm::InsertElementInst>(inst);
    while (ie != NULL) {
        int64_t iOffset = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))->getSExtValue();
        assert(iOffset>=0);
        if (elem[iOffset] == NULL) {
            elem[iOffset] = ie->getOperand(1);
            count++;
            if (ie->hasOneUse()) {
                instructionToRemove.push_back(ie);
            }
        }
        llvm::Value *insertBase = ie->getOperand(0);
        ie = llvm::dyn_cast<llvm::InsertElementInst>(insertBase);
    }
    return (count == 4);
}

void VectorToElement(llvm::Value *inst, llvm::Value *elem[], llvm::Type *int32Ty, llvm::Instruction *insert_before, int vsize)
{
    for (int i = 0; i < vsize; i++) {
        if (elem[i] == nullptr) {
            // Create an ExtractElementInst
            elem[i] = llvm::ExtractElementInst::Create(inst, llvm::ConstantInt::get(int32Ty, i), "", insert_before);
        }
    }
}

llvm::Value* ElementToVector(llvm::Value *elem[], llvm::Type *int32Ty, llvm::Instruction *insert_before, int vsize)
{
    llvm::VectorType *vt = llvm::VectorType::get(elem[0]->getType(), vsize);
    llvm::Value *vecValue = llvm::UndefValue::get(vt);

    for (int i = 0; i < vsize; ++i)
    {
        vecValue = llvm::InsertElementInst::Create(vecValue, elem[i], llvm::ConstantInt::get(int32Ty, i), "", insert_before);
    }
    return vecValue;
}

bool IsUnsignedCmp(const llvm::CmpInst::Predicate Pred)
{
    switch (Pred) {
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_UGE:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_ULE:
        return true;
    default:
        break;
    }
    return false;
}

bool IsSignedCmp(const llvm::CmpInst::Predicate Pred)
{
    switch (Pred)
    {
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_SLE:
        return true;
    default:
        break;
    }
    return false;
}

// isA64Ptr - Queries whether given pointer type requires 64-bit representation in vISA
bool isA64Ptr(llvm::PointerType *PT, CodeGenContext* pContext)
{
    return pContext->getRegisterPointerSizeInBits(PT->getAddressSpace()) == 64;
}

bool IsBitCastForLifetimeMark(const llvm::Value *V)
{
    if (!V || !llvm::isa<llvm::BitCastInst>(V))
    {
        return false;
    }
    for (llvm::Value::const_user_iterator it = V->user_begin(), e = V->user_end(); it != e; ++it)
    {
        const llvm::IntrinsicInst *inst = llvm::dyn_cast<const llvm::IntrinsicInst>(*it);
        if (!inst)
        {
            return false;
        }
        llvm::Intrinsic::ID  IID = inst->getIntrinsicID();
        if (IID != llvm::Intrinsic::lifetime_start &&
            IID != llvm::Intrinsic::lifetime_end)
        {
            return false;
        }
    }
    return true;
}

Value* mutatePtrType(Value* ptrv, PointerType* newType,
    IRBuilder<>& builder, const Twine&)
{
    if (isa<ConstantPointerNull>(ptrv))
    {
        return ConstantPointerNull::get(newType);
    }
    else
    {
        if (ConstantExpr* cexpr = dyn_cast<ConstantExpr>(ptrv))
        {
            assert(cexpr->getOpcode() == Instruction::IntToPtr);
            Value* offset = cexpr->getOperand(0);
            ptrv = builder.CreateIntToPtr(offset, newType);
        }
        else
        {
            ptrv->mutateType(newType);
        }
    }
    return ptrv;
}

/*
cmp.l.f0.0 (8) null:d       r0.0<0;1,0>:w    0x0000:w         { Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi Test
(-f0.0) sendc (8) null:ud      r120.0<0;1,0>:f  0x00000025  0x08031400:ud    { Align1, N1, EOT, NoCompact }
nop
Test :
nop

*/

static const unsigned int CRastHeader_SIMD8[] =
{
    0x05600010,0x20001a24,0x1e000000,0x00000000,
    0x00110020,0x34000004,0x0e001400,0x00000020,
    0x05710032,0x20003a00,0x06000f00,0x88031400,
    0x00000000,0x00000000,0x00000000,0x00000000,
};

/*
cmp.l.f0.0 (16) null:d       r0.0 < 0; 1, 0 > : w    0x0000 : w{ Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi(1) Test { Align1, N1, NoMask, NoCompact }
(-f0.0) sendc(16) null : ud      r120.0 < 0; 1, 0 > : f  0x00000025 0x90031000 : ud{ Align1, N1, EOT, NoCompact }
nop
Test :
nop

*/
static const unsigned int CRastHeader_SIMD16[] =
{
    0x05800010, 0x20001A24, 0x1E000000, 0x00000000,
    0x00110020, 0x34000004, 0x0E001400, 0x00000020,
    0x05910032, 0x20003A00, 0x06000F00, 0x90031000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

/*
cmp.l.f0.0 (16) null:d       r0.0 < 0; 1, 0 > : w    0x0000 : w{ Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi Test
(-f0.0) sendc(16) null : w r120.0 < 0; 1, 0 > : ud  0x00000005 0x10031000 : ud{ Align1, N1, NoCompact }
(-f0.0) sendc(16) null : w r120.0 < 0; 1, 0 > : f  0x00000025  0x10031800 : ud{ Align1, N5, EOT, NoCompact }
nop
Test :
nop

*/

static const unsigned int CRastHeader_SIMD32[] =
{
    0x05800010,0x20001a24,0x1e000000,0x00000000,
    0x00110020,0x34000004,0x0e001400,0x00000020,
    0x05910032,0x20000260,0x06000f00,0x10031000,
    0x05912032,0x20003a60,0x06000f00,0x90031800,
};


unsigned int AppendConservativeRastWAHeader(IGC::SProgramOutput* program, SIMDMode simdmode)
{
     unsigned int headerSize = 0;
     const unsigned int* pHeader = nullptr;

    if (program && (program->m_programSize > 0 ))
    {
        switch (simdmode)
        {
        case SIMDMode::SIMD8: 
            headerSize = sizeof(CRastHeader_SIMD8);
            pHeader = CRastHeader_SIMD8;
            break;

        case SIMDMode::SIMD16: 
            headerSize = sizeof(CRastHeader_SIMD16);
            pHeader = CRastHeader_SIMD16;
            break;

        case SIMDMode::SIMD32: 
            headerSize = sizeof(CRastHeader_SIMD32);
            pHeader = CRastHeader_SIMD32;
            break;

        default: assert("Invalid SIMD Mode for Conservative Raster WA");
                    break;
        }

        unsigned int newSize = program->m_programSize + headerSize;
        void* newBinary = IGC::aligned_malloc(newSize, 16);
        memcpy_s(newBinary, newSize, pHeader, headerSize);
        memcpy_s((char*)newBinary + headerSize, newSize, program->m_programBin, program->m_programSize);
        IGC::aligned_free(program->m_programBin);
        program->m_programBin = newBinary;
        program->m_programSize = newSize;
    }
    return headerSize;
}

bool DSDualPatchEnabled(class CodeGenContext* ctx)
{
    return ctx->platform.supportDSDualPatchDispatch() &&
        ctx->platform.WaDisableDSDualPatchMode() &&
        !(ctx->m_DriverInfo.APIDisableDSDualPatchDispatch()) &&
        IGC_IS_FLAG_DISABLED(DisableDSDualPatch);
}

Function* getUniqueEntryFunc(const IGCMD::MetaDataUtils *pM)
{
	Function *entryFunc = nullptr;
	for (auto i = pM->begin_FunctionsInfo(), e = pM->end_FunctionsInfo(); i != e; ++i)
	{
		IGCMD::FunctionInfoMetaDataHandle Info = i->second;
		if (Info->getType() != IGCMD::FunctionTypeEnum::EntryFunctionType)
		{
			continue;
		}

		const Function *F = i->first;
		if (!entryFunc)
		{
			entryFunc = const_cast<Function*>(F);
		}
		else
		{
			assert(false && "Not a single entry func!");
		}
	}
	assert(entryFunc && "No entry func!");
	return entryFunc;
}

// If true, the codegen will likely not emit instruction for this instruction.
bool isNoOpInst(Instruction* I, CodeGenContext* Ctx)
{
    if (isa<BitCastInst>(I) ||
        isa<IntToPtrInst>(I) ||
        isa<PtrToIntInst>(I))
    {
        // Don't bother with constant operands
        if (isa<Constant>(I->getOperand(0))) {
            return false;
        }

        Type* dTy = I->getType();
        Type* sTy = I->getOperand(0)->getType();
        PointerType *dPTy = dyn_cast<PointerType>(dTy);
        PointerType *sPTy = dyn_cast<PointerType>(sTy);
        uint32_t dBits = dPTy ? Ctx->getRegisterPointerSizeInBits(dPTy->getAddressSpace())
                              : dTy->getPrimitiveSizeInBits();
        uint32_t sBits = sPTy ? Ctx->getRegisterPointerSizeInBits(sPTy->getAddressSpace())
                              : sTy->getPrimitiveSizeInBits();
        if (dBits == 0 || sBits == 0 || dBits != sBits) {
            // Not primitive type or not equal in size (inttoptr, etc)
            return false;
        }

        VectorType* dVTy = dyn_cast<VectorType>(dTy);
        VectorType* sVTy = dyn_cast<VectorType>(sTy);
        int d_nelts = dVTy ? (int)dVTy->getNumElements() : 1;
        int s_nelts = sVTy ? (int)sVTy->getNumElements() : 1;
        if (d_nelts != s_nelts) {
            // Vector relayout bitcast.
            return false;
        }
        return true;
    }
    return false;
}

//
// Given a value, check if it is likely a positive number.
//
// This function works best if llvm.assume() is used in the bif libraries to
// give ValueTracking hints.  ex:
//
// size_t get_local_id(uint dim)
// {
//    size_t ret = __builtin_IB_get_local_id()
//    __builtin_assume(ret >= 0);
//    __builtin_assume(ret <= 0x0000ffff)
//    return ret;
// }
// 
// This implementation relies completly on native llvm functions
//
//
//
bool valueIsPositive(
	Value* V,
	const DataLayout *DL,
	llvm::AssumptionCache *AC,
	llvm::Instruction *CxtI)
{
#if LLVM_VERSION_MAJOR == 4
	bool isKnownNegative = false;
	bool isKnownPositive = false;
	llvm::ComputeSignBit(
		V,
		isKnownPositive,
		isKnownNegative,
		*DL,
		0,
		AC,
		CxtI);
	return isKnownPositive;
#elif LLVM_VERSION_MAJOR >= 7
	return computeKnownBits(
		V,
		*DL,
		0,
		AC,
		CxtI).isNonNegative();
#endif
}

void appendToUsed(llvm::Module &M, ArrayRef<GlobalValue *> Values)
{
    std::string Name = "llvm.used";
    GlobalVariable *GV = M.getGlobalVariable(Name);
    SmallPtrSet<Constant *, 16> InitAsSet;
    SmallVector<Constant *, 16> Init;
    if (GV) {
        ConstantArray *CA = dyn_cast<ConstantArray>(GV->getInitializer());
        for (auto &Op : CA->operands()) {
            Constant *C = cast_or_null<Constant>(Op);
            if (InitAsSet.insert(C).second)
                Init.push_back(C);
        }
        GV->eraseFromParent();
    }

    Type *Int8PtrTy = llvm::Type::getInt8PtrTy(M.getContext());
    for (auto *V : Values) {
        Constant *C = V;
        if(V->getType()->getAddressSpace() != 0)
            C = ConstantExpr::getAddrSpaceCast(V, Int8PtrTy);
        else
            C = ConstantExpr::getBitCast(V, Int8PtrTy);
        if (InitAsSet.insert(C).second)
            Init.push_back(C);
    }

    if (Init.empty())
        return;

    ArrayType *ATy = ArrayType::get(Int8PtrTy, Init.size());
    GV = new llvm::GlobalVariable(M, ATy, false, GlobalValue::AppendingLinkage,
                                  ConstantArray::get(ATy, Init), Name);
    GV->setSection("llvm.metadata");
}

} // namespace IGC

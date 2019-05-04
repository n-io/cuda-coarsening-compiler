// ============================================================================
// Copyright (c) Richard Rohac, 2019, All rights reserved.
// ============================================================================
// CUDA Coarsening Transformation pass
// -> Based on Alberto's Magni OpenCL coarsening pass algorithm
//    available at https://github.com/HariSeldon/coarsening_pass
// ============================================================================

#include "llvm/Pass.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/IRBuilder.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"

#include "Common.h"
#include "CUDACoarsening.h"
#include "Util.h"
#include "DivergenceAnalysisPass.h"
#include "GridAnalysisPass.h"

#include <cxxabi.h>
#include <stdlib.h>

// https://gcc.gnu.org/onlinedocs/libstdc++/manual/ext_demangling.html
inline std::string demangle(std::string mangledName)
{
    int status = -1;

    std::unique_ptr<char, decltype(std::free) *> result{
        abi::__cxa_demangle(mangledName.c_str(), NULL, NULL, &status),
        std::free
    };

    return (status == 0) ? result.get() : mangledName;
}

// Command line parameters
cl::opt<std::string> CLKernelName("coarsened-kernel",
                                  cl::init(""),
                                  cl::Hidden,
                                  cl::desc("Name of the kernel to coarsen"));

cl::opt<unsigned int> CLCoarseningFactor("coarsening-factor",
                                         cl::init(1),
                                         cl::Hidden,
                                         cl::desc("Coarsening factor"));

cl::opt<unsigned int> CLCoarseningStride("coarsening-stride",
                                         cl::init(1),
                                         cl::Hidden,
                                         cl::desc("Coarsening stride"));

cl::opt<std::string> CLCoarseningDimension("coarsening-dimension",
                                           cl::init("y"),
                                           cl::Hidden,
                                           cl::desc("Coarsening dimension"));

cl::opt<std::string> CLCoarseningMode("coarsening-mode",
                                      cl::init("block"),
                                      cl::Hidden,
                                      cl::desc("Coarsening mode (thread/block)"));

using namespace llvm;

char CUDACoarseningPass::ID = 0;

// CREATORS
CUDACoarseningPass::CUDACoarseningPass()
: ModulePass(ID)
{

}

bool CUDACoarseningPass::runOnModule(Module& M)
{
    // Parse command line configuration
    m_kernelName = CLKernelName;
    assert(m_kernelName != "");
    assert((CLCoarseningMode == "block" ||
           CLCoarseningMode == "thread" ||
           CLCoarseningMode == "dynamic") &&
           "Wrong mode specified!");
    m_blockLevel = CLCoarseningMode == "block";
    m_dynamicLevel = CLCoarseningMode == "dynamic";
    m_factor = CLCoarseningFactor;
    m_stride = CLCoarseningStride;
    m_dimX = CLCoarseningDimension.find('x') != std::string::npos;
    m_dimY = CLCoarseningDimension.find('y') != std::string::npos;
    m_dimZ = CLCoarseningDimension.find('z') != std::string::npos;

    errs() << "\nInvoked CUDA COARSENING PASS (MODULE LEVEL) "
           << "on module: " << M.getName()
           << " -- kernel: " << CLKernelName << " " << CLCoarseningFactor
           << "x " << CLCoarseningMode << " mode" 
           << " with stride " << CLCoarseningStride << "\n";

    bool result = false;

    if (M.getTargetTriple() == CUDA_TARGET_TRIPLE) {
        // -----------------------------------------------------------------
        // Device code gets extended with coarsened versions of the kernels.
        // For example:
        // -----------------------------------------------------------------
        // XYZ -> XYZ_1x_2x_<stride> XYZ_1x_4x_<stride> XYZ_1x_8x_<stride> ...
        //        XYZ_2x_1x_1x
        //        XYZ_4x_1x_1x
        //        XYZ_8x_1x_1x
        //        ...
        // -----------------------------------------------------------------
        // Where the numbering in the kernel names is defined as follows:
        // <block_factor>_<thread_factor>_<stride_factor>
        // -----------------------------------------------------------------
        result = handleDeviceCode(M);
    }
    else {
        // -----------------------------------------------------------------
        // Host code gets either extended with a dispatcher function
        // to support more versions of coarsened grids, or, for optimization
        // purposes, specific one can be selected as well.
        // -----------------------------------------------------------------
        result = handleHostCode(M);
    }
    errs() << "--  INFO  -- End of CUDA coarsening pass!" << "\n\n";

    return result;
}

void CUDACoarseningPass::getAnalysisUsage(AnalysisUsage& AU) const
{
    AU.addRequired<DivergenceAnchorPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DivergenceAnalysisPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<GridAnalysisPass>();
}

bool CUDACoarseningPass::handleDeviceCode(Module& M)
{
    errs() << "--  INFO  -- Running on device code" << "\n";

    const llvm::NamedMDNode *nvmmAnnot = M.getNamedMetadata("nvvm.annotations");
    if (!nvmmAnnot) {
        errs() << "--  STOP  -- Missing nvvm.annotations in this module.\n";
        return false;
    }

    bool foundKernel = false;
    for (auto& F : M) {
        if (Util::isKernelFunction(F) && !F.isDeclaration()) {
            foundKernel = true;

            std::string name = demangle(F.getName());
            name = name.substr(0, name.find_first_of('('));

            if (name != m_kernelName) {
                continue;
            }

            errs() << "--  INFO  -- Found CUDA kernel: " << name << "\n";

            if (m_dynamicLevel) {
                generateVersions(F);
                continue;
            }

            analyzeKernel(F);
            scaleKernelGrid();
            coarsenKernel();
            replacePlaceholders();
        }
    }

    return foundKernel;
}

bool CUDACoarseningPass::handleHostCode(Module& M)
{
    errs() << "--  INFO  -- Running on host code" << "\n";

    bool foundGrid = false;

    m_cudaConfigureCallScaled = nullptr;
    insertCudaConfigureCallScaled(M);

    //if (m_dynamicLevel) {
    //    Constant* envFunc = M.getOrInsertFunction(
    //        "rpcReadEnv", Type::getVoidTy(M.getContext()), Type::getInt32Ty(Ctx), NULL
    //        );
    //}

    std::vector<CallInst*> toRemove;

    for (Function& F : M) {
        for (BasicBlock& B: F) {
            for (Instruction& I : B) {
                Instruction *pI = &I;
                if (CallInst *callInst = dyn_cast<CallInst>(pI)) {
                    Function *calledF = callInst->getCalledFunction();

                    if (calledF->getName() == CUDA_RUNTIME_LAUNCH) {
                        // cudaLaunch receives function pointer as an argument.
                        Constant *castPtr = 
                                        cast<Constant>(callInst->getOperand(0));
                        Function *kernelF =
                                         cast<Function>(castPtr->getOperand(0));
                        std::string kernel = kernelF->getName();
                        
                        kernel = demangle(kernel);
                        kernel = kernel.substr(0, kernel.find_first_of('('));

                        if (kernel != m_kernelName) {
                            continue;
                        }

                        errs() << "--  INFO  -- Found cudaLaunch of " << kernel << "\n";
                        foundGrid = true;

                        BasicBlock *configOKBlock = &B;

                        #ifndef CUDA_USES_NEW_LAUNCH
                        // Call to cudaLaunch is preceded by "numArgs()" of
                        // blocks, where the very first one is referenced by
                        // the unconditional branch instruction that checks
                        // for valid configuration (call to cudaConfigureCall).
                        for (unsigned int i = 0;
                             i < kernelF->arg_size();
                             ++i) {
                                 configOKBlock = configOKBlock->getPrevNode();
                        }
                        #endif

                        // FIXED!
                        // Depending on the optimization level, we might be
                        // in a _kernelname_() function call.
                        std::string pn = demangle(
                                         configOKBlock->getParent()->getName());
                        pn = pn.substr(0, pn.find_first_of('('));

                        if (pn == kernel) {
                            for (Function& xF : M) {
                                for (BasicBlock& xB: xF) {
                                    for (Instruction& xI : xB) {
                                        Instruction *pxI = &xI;
                                        if (CallInst *callInst = dyn_cast<CallInst>(pxI)) {
                                            Function *calledxF = callInst->getCalledFunction();
                                            if (calledxF == configOKBlock->getParent()) {
                                                CallInst *rem = amendConfiguration(M, callInst->getParent());
                                                assert(rem != nullptr);
                                                toRemove.push_back(rem);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            CallInst *rem = amendConfiguration(M, configOKBlock);
                            assert(rem != nullptr);
                            toRemove.push_back(rem);
                        }
                    }
                }
            }
        }
    }

    for(CallInst *rem : toRemove) {
        rem->eraseFromParent();
    }

    if (!foundGrid && m_cudaConfigureCallScaled != nullptr) {
        m_cudaConfigureCallScaled->eraseFromParent();
    }

    return foundGrid;
}

void CUDACoarseningPass::generateVersions(Function& F)
{
    std::vector<unsigned int> factors = {2, 4, 8, 16};
    std::vector<unsigned int> strides = {1, 2, 4};

    for (auto factor : factors) {
        for (auto stride : strides) {
            llvm::ValueToValueMapTy vMap;
            Function *cloned = llvm::CloneFunction(&F, vMap);
            cloned->setName(namedKernelVersion(F.getName(), 1, factor, stride));

            unsigned int savedFactor = m_factor;
            unsigned int savedStride = m_stride;
            bool savedBlockLevel = m_blockLevel;

            m_factor = factor;
            m_stride = stride;
            m_blockLevel = false;

            analyzeKernel(*cloned);
            scaleKernelGrid();
            coarsenKernel();
            replacePlaceholders();

            m_factor = savedFactor;
            m_stride = savedStride;
            m_blockLevel = savedBlockLevel;
            //F.getMetadata("nvvm.annotations")->getOperand(0)->dump();
            //cloned->setMetadata("nvvm.annotations", F.getMetadata("nvvm.annotations"));
            
        }
    }
                //errs() << "--  INFO  -- Found CUDA kernel: " << name << "\n";

           // analyzeKernel(F);
          //  scaleKernelGrid();
           // coarsenKernel();
          //  replacePlaceholders();
}

std::string CUDACoarseningPass::namedKernelVersion(std::string kernel,
                                                   int b, int t, int s)
{
    // Generate <kernel>_<blockfactor>_<threadfactor>_<stride> name
    std::string name = kernel;
    name.append("_");
    name.append(std::to_string(b));
    name.append("_");
    name.append(std::to_string(t));
    name.append("_");
    name.append(std::to_string(s));
    return name;
}

void CUDACoarseningPass::analyzeKernel(Function& F)
{
    m_coarseningMap.clear();
    m_phMap.clear();
    m_phReplacementMap.clear();

    // Perform initial analysis.
    m_loopInfo = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    m_postDomT = &getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
    m_domT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    m_divergenceAnalysis = &getAnalysis<DivergenceAnalysisPass>(F);
    m_gridAnalysis = &getAnalysis<GridAnalysisPass>(F);
}

void CUDACoarseningPass::scaleGrid(BasicBlock *configBlock,
                                   CallInst   *configCall)
{
    uint8_t coarseningGrid[CUDA_MAX_DIM];
    uint8_t coarseningBlock[CUDA_MAX_DIM];

    coarseningGrid[0] = (m_dimX && m_blockLevel) ? m_factor : 1;
    coarseningGrid[1] = (m_dimY && m_blockLevel) ? m_factor : 1;
    coarseningGrid[2] = (m_dimZ && m_blockLevel) ? m_factor : 1;

    coarseningBlock[0] = (m_dimX && !m_blockLevel) ? m_factor : 1;
    coarseningBlock[1] = (m_dimY && !m_blockLevel) ? m_factor : 1;
    coarseningBlock[2] = (m_dimZ && !m_blockLevel) ? m_factor : 1;

    IRBuilder<> builder(configCall);
    SmallVector<Value *, 12> args(configCall->arg_begin(),
                                  configCall->arg_end());

    args.push_back(builder.getInt8(coarseningGrid[0])); // scale grid X
    args.push_back(builder.getInt8(coarseningGrid[1])); // scale grid Y
    args.push_back(builder.getInt8(coarseningGrid[2])); // scale grid Z
    args.push_back(builder.getInt8(coarseningBlock[0])); // scale block X
    args.push_back(builder.getInt8(coarseningBlock[1])); // scale block Y
    args.push_back(builder.getInt8(coarseningBlock[2])); // scale block Z

    CallInst *newCall = builder.CreateCall(m_cudaConfigureCallScaled, args);
    newCall->setCallingConv(m_cudaConfigureCallScaled->getCallingConv());
    if (!configCall->use_empty()) {
        configCall->replaceAllUsesWith(newCall);
    }
}

CallInst *CUDACoarseningPass::amendConfiguration(Module&     M,
                                                 BasicBlock *configOKBlock)
{
    CallInst *ret = nullptr;

    if (configOKBlock == nullptr) {
        assert(0 && "Found cudaLaunch without corresponding config block!");
    }

    // Find branch instruction jumping to the "configOK" block.
    // This instruction is located within a block that handles
    // cudaConfigureCall().
    BasicBlock *configBlock = nullptr;
    for (Function& F : M) {
        for (BasicBlock& B: F) {
            for (Instruction& I : B) {
                if (isa<BranchInst>(&I)) {
                    BranchInst *bI = cast<BranchInst>(&I);
                    if (bI->getNumOperands() == 3) {
                        if(isa<BasicBlock>(bI->getOperand(1))) {
                            BasicBlock *targetBlock =
                                            cast<BasicBlock>(bI->getOperand(1));

                            if (targetBlock == configOKBlock) {
                                configBlock = bI->getParent();
                            }
                        }
                        
                        if(isa<BasicBlock>(bI->getOperand(2))) {
                            BasicBlock *targetBlock =
                                            cast<BasicBlock>(bI->getOperand(2));

                            if (targetBlock == configOKBlock) {
                                configBlock = bI->getParent();
                            }
                        }
                    }
                }
            }
        }
    }

    assert(configBlock != nullptr);

    for (Instruction& I : *configBlock) {
        Instruction *pI = &I;
        if (CallInst *callInst = dyn_cast<CallInst>(pI)) {
            Function *calledF = callInst->getCalledFunction();

            if (calledF->getName() == CUDA_RUNTIME_CONFIGURECALL) {
                scaleGrid(configBlock, callInst);
                ret = callInst;
            }
        } 
    }

    return ret;
}

AllocaInst *CreateAllocaA(IRBuilder<> *b, Type *Ty, Value *ArraySize = nullptr,
                         const Twine &Name = "", unsigned int align = 8) {
        const DataLayout &DL = b->GetInsertBlock()->getParent()->getParent()->getDataLayout();
return b->Insert(new AllocaInst(Ty, DL.getAllocaAddrSpace(), ArraySize, align), Name);
}

void CUDACoarseningPass::insertCudaConfigureCallScaled(Module& M)
{
    LLVMContext& ctx = M.getContext();
    Function *ptrF;

    Function *original = M.getFunction(CUDA_RUNTIME_CONFIGURECALL);
    FunctionType *origFT = original->getFunctionType();
    assert(original != nullptr);

    SmallVector<Type *, 16> scaledArgs;
    for (auto& arg : origFT->params()) {
        scaledArgs.push_back(arg);
    }

    assert(original->arg_size() == 6 && "This ABI is not supported yet!");

    scaledArgs.push_back(Type::getInt8Ty(ctx));
    scaledArgs.push_back(Type::getInt8Ty(ctx));
    scaledArgs.push_back(Type::getInt8Ty(ctx));
    scaledArgs.push_back(Type::getInt8Ty(ctx));
    scaledArgs.push_back(Type::getInt8Ty(ctx));
    scaledArgs.push_back(Type::getInt8Ty(ctx));

    FunctionCallee scaled = M.getOrInsertFunction(
        "cudaConfigureCallScaled",
        FunctionType::get(original->getReturnType(), scaledArgs, false)
    );

    ptrF = cast<Function>(scaled.getCallee());
    ptrF->setCallingConv(original->getCallingConv());

    Function::arg_iterator argIt = ptrF->arg_begin();
    Value *gridXY = argIt++; gridXY->setName("gridXY");
    Value *gridZ = argIt++; gridZ->setName("gridZ");
    Value *blockXY = argIt++; blockXY->setName("blockXY");
    Value *blockZ = argIt++; blockZ->setName("blockZ");
    Value *sharedMem = argIt++; sharedMem->setName("sharedMem");
    Value *cudaStream = argIt++; cudaStream->setName("cudaStream");
    Value *scaleGridX = argIt++; scaleGridX->setName("sgX");
    Value *scaleGridY = argIt++; scaleGridY->setName("sgY");
    Value *scaleGridZ = argIt++; scaleGridZ->setName("sgZ");
    Value *scaleBlockX = argIt++; scaleBlockX->setName("sbX");
    Value *scaleBlockY = argIt++; scaleBlockY->setName("sbY");
    Value *scaleBlockZ = argIt++; scaleBlockZ->setName("sbZ");

    BasicBlock* block = BasicBlock::Create(ctx, "entry", ptrF);
    IRBuilder<> builder(block);

    // Allocate space for amended parameters
    AllocaInst *sgXY =
              CreateAllocaA(&builder, builder.getInt64Ty(), nullptr, "sgXY", 8);
    AllocaInst *sgZ =
               CreateAllocaA(&builder, builder.getInt32Ty(), nullptr, "sgZ", 8);
    AllocaInst *sbXY =
              CreateAllocaA(&builder, builder.getInt64Ty(), nullptr, "sbXY", 8);
    AllocaInst *sbZ =
               CreateAllocaA(&builder, builder.getInt32Ty(), nullptr, "sbZ", 8);
    AllocaInst *ssm =
            CreateAllocaA(&builder, origFT->getParamType(4), nullptr, "ssm", 8);
    AllocaInst *scs =
            CreateAllocaA(&builder, origFT->getParamType(5), nullptr, "scs", 8);

    builder.CreateAlignedStore(gridXY, sgXY, 8, false);
    builder.CreateAlignedStore(gridZ, sgZ, 8, false);
    builder.CreateAlignedStore(blockXY, sbXY, 8, false);
    builder.CreateAlignedStore(blockZ, sbZ, 8, false);
    builder.CreateAlignedStore(sharedMem, ssm, 8, false);
    builder.CreateAlignedStore(cudaStream, scs, 8, false);

    // Scale grid X
    Value *ptrGridX = builder.CreatePointerCast(sgXY, Type::getInt32PtrTy(ctx));
    ptrGridX = builder.CreateInBoundsGEP(ptrGridX,
                                         ConstantInt::get(builder.getInt64Ty(),
                                                          0));
    Value *valGridX = builder.CreateAlignedLoad(ptrGridX, 4);
    Value *valScaledGridX = 
        builder.CreateUDiv(valGridX,
                           builder.CreateIntCast(scaleGridX,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledGridX, ptrGridX, 4, false);

    // Scale grid Y
    Value *ptrGridY = builder.CreatePointerCast(sgXY, Type::getInt32PtrTy(ctx));
    ptrGridY = builder.CreateInBoundsGEP(ptrGridY,
                                         ConstantInt::get(builder.getInt64Ty(),
                                                          1));
    Value *valGridY = builder.CreateAlignedLoad(ptrGridY, 4);
    Value *valScaledGridY = 
        builder.CreateUDiv(valGridY,
                           builder.CreateIntCast(scaleGridY,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledGridY, ptrGridY, 4, false);

    // Scale grid Z
    Value *valGridZ = builder.CreateAlignedLoad(sgZ, 8);
    Value *valScaledGridZ = 
        builder.CreateUDiv(valGridZ,
                           builder.CreateIntCast(scaleGridZ,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledGridZ, sgZ, 8, false);

    // Scale BLOCK X
    Value *ptrBlockX = builder.CreatePointerCast(sbXY, Type::getInt32PtrTy(ctx));
    ptrBlockX = builder.CreateInBoundsGEP(ptrBlockX,
                                          ConstantInt::get(builder.getInt64Ty(),
                                                           0));
    Value *valBlockX = builder.CreateAlignedLoad(ptrBlockX, 4);
    Value *valScaledBlockX = 
        builder.CreateUDiv(valBlockX,
                           builder.CreateIntCast(scaleBlockX,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledBlockX, ptrBlockX, 4, false);

    // Scale BLOCK Y
    Value *ptrBlockY = builder.CreatePointerCast(sbXY, Type::getInt32PtrTy(ctx));
    ptrBlockY = builder.CreateInBoundsGEP(ptrBlockY,
                                          ConstantInt::get(builder.getInt64Ty(),
                                                           1));
    Value *valBlockY = builder.CreateAlignedLoad(ptrBlockY, 4);
    Value *valScaledBlockY = 
        builder.CreateUDiv(valBlockY,
                           builder.CreateIntCast(scaleBlockY,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledBlockY, ptrBlockY, 4, false);

    // Scale BLOCK Z
    Value *valBlockZ = builder.CreateAlignedLoad(sbZ, 8);
    Value *valScaledBlockZ = 
        builder.CreateUDiv(valBlockZ,
                           builder.CreateIntCast(scaleBlockZ,
                                                 builder.getInt32Ty(),
                                                 false));
    builder.CreateAlignedStore(valScaledBlockZ, sbZ, 8, false);

    Value *c_sgXY = builder.CreateAlignedLoad(sgXY, 8, "c_sgXY");
    Value *c_sgZ = builder.CreateAlignedLoad(sgZ, 8, "c_sgZ");
    Value *c_sbXY = builder.CreateAlignedLoad(sbXY, 8, "c_sbXY");
    Value *c_sbZ = builder.CreateAlignedLoad(sbZ, 8, "c_sbZ");
    Value *c_ssm = builder.CreateAlignedLoad(ssm, 8, "c_ssm");
    Value *c_scs = builder.CreateAlignedLoad(scs, 8, "c_scs"); 

    SmallVector<Value *, 6> callArgs;
    callArgs.push_back(c_sgXY); callArgs.push_back(c_sgZ);
    callArgs.push_back(c_sbXY); callArgs.push_back(c_sbZ);
    callArgs.push_back(c_ssm); callArgs.push_back(c_scs);

    CallInst *cudaCall = builder.CreateCall(original, callArgs);

    builder.CreateRet(cudaCall);

    m_cudaConfigureCallScaled = ptrF;
}

static RegisterPass<CUDACoarseningPass> X("cuda-coarsening-pass",
                                          "CUDA Coarsening Pass",
                                          false, // Only looks at CFG,
                                          false // Analysis pass
                                          );
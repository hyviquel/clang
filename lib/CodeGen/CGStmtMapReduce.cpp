//===--- CGStmtOpenMP.cpp - Emit LLVM Code for declarations ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Decl nodes as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CGDebugInfo.h"
#include "CGOpenCLRuntime.h"
#include "CGOpenMPRuntimeTypes.h"
#include "CGOpenMPRuntime.h"
#include "CodeGenModule.h"
#include "TargetInfo.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclOpenMP.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/Basic/OpenMPKinds.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/CallSite.h"

#include "clang/AST/RecursiveASTVisitor.h"

#define VERBOSE 1

using namespace clang;
using namespace CodeGen;


void CodeGenFunction::GenArgumentElementSize(const VarDecl *VD) {
  llvm::Module *mod = &(CGM.getModule());

  // Find the bit size of one element
  QualType varType = VD->getType();
  while(varType->isAnyPointerType()) {
    varType = varType->getPointeeType();
  }
  int64_t size = getContext().getTypeSize(varType);

  // Type Definitions
  llvm::StructType *StructTy_JNINativeInterface = mod->getTypeByName("struct.JNINativeInterface_");
  llvm::PointerType* PointerTy_JNINativeInterface = llvm::PointerType::get(StructTy_JNINativeInterface, 0);
  llvm::PointerType* PointerTy_1 = llvm::PointerType::get(PointerTy_JNINativeInterface, 0);

  llvm::StructType *StructTy_jobject = mod->getTypeByName("struct._jobject");
  llvm::PointerType* PointerTy_jobject = llvm::PointerType::get(StructTy_jobject, 0);

  std::vector<llvm::Type*> FuncTy_sizeMethod_args;
  FuncTy_sizeMethod_args.push_back(PointerTy_1);
  FuncTy_sizeMethod_args.push_back(PointerTy_jobject);
  llvm::FunctionType* FuncTy_sizeMethod = llvm::FunctionType::get(
        /*Result=*/llvm::IntegerType::get(mod->getContext(), 32),
        /*Params=*/FuncTy_sizeMethod_args,
        /*isVarArg=*/false);

  // Function Declarations
  llvm::StringRef FnName_sizeMethod = llvm::StringRef((".GetSizeOf" + VD->getName()).str());
  llvm::Function* sizeMethod = mod->getFunction(FnName_sizeMethod);
  if (!sizeMethod) {
    sizeMethod = llvm::Function::Create(
          /*Type=*/FuncTy_sizeMethod,
          /*Linkage=*/llvm::GlobalValue::ExternalLinkage,
          /*Name=*/FnName_sizeMethod, mod);
    sizeMethod->setCallingConv(llvm::CallingConv::C);
  }

  llvm::AttributeSet sizeMethod_PAL;
  {
    SmallVector<llvm::AttributeSet, 4> Attrs;
    llvm::AttributeSet PAS;
    {
      llvm::AttrBuilder B;
      B.addAttribute(llvm::Attribute::NoUnwind);
      B.addAttribute(llvm::Attribute::StackProtect);
      B.addAttribute(llvm::Attribute::UWTable);
      PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
    }

    Attrs.push_back(PAS);
    sizeMethod_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

  }
  sizeMethod->setAttributes(sizeMethod_PAL);

  // Constant Definitions
  llvm::ConstantInt* const_size = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, size, false));

  // Function: .GetSizeOf
  {
    llvm::Function::arg_iterator args = sizeMethod->arg_begin();
    llvm::Value* ptr_env = args++;
    ptr_env->setName("env");
    llvm::Value* ptr_obj = args++;
    ptr_obj->setName("obj");

    llvm::BasicBlock* block = llvm::BasicBlock::Create(mod->getContext(), "", sizeMethod, 0);
    CGBuilderTy LBuilder(block);

    // Block  (label_252)
    llvm::AllocaInst* alloca_env = LBuilder.CreateAlloca(PointerTy_1);
    llvm::AllocaInst* alloca_obj = LBuilder.CreateAlloca(PointerTy_jobject);
    llvm::StoreInst* store_env = LBuilder.CreateStore(ptr_env, alloca_env);
    llvm::StoreInst* store_obj = LBuilder.CreateStore(ptr_obj, alloca_obj);
    LBuilder.CreateRet(const_size);

  }
}


/// A StmtVisitor that propagates the raw counts through the AST and
/// records the count at statements where the value may change.
struct FindIndexingArguments : public RecursiveASTVisitor<FindIndexingArguments> {

  CodeGenFunction &CGF;
  CodeGenModule &CGM;
  bool verbose;

  llvm::DenseMap<const VarDecl*, llvm::SmallVector<const Expr*,8>> InputsMap;

  ArraySubscriptExpr *CurrArrayExpr;
  Expr *CurrArrayIndexExpr;

  FindIndexingArguments(CodeGenFunction &CGF)
    : CGF(CGF), CGM(CGF.CGM) {
    verbose = VERBOSE;
    CurrArrayExpr = NULL;
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {

    if(const VarDecl *VD = dyn_cast<VarDecl>(D->getDecl())) {
      if(verbose) llvm::errs() << "Indexing use the variable " << VD->getName();
      const Expr *RefExpr;

      if(CurrArrayExpr != nullptr) {
        RefExpr = CurrArrayExpr;
        if(verbose) llvm::errs() << "Require more advanced analysis\n";
        exit(0);
      } else {
        RefExpr = D;
      }

      InputsMap[VD].push_back(RefExpr);

      if(verbose) llvm::errs() << "\n";
    }

    return true;
  }

  bool TraverseArraySubscriptExpr(ArraySubscriptExpr *A) {
    CurrArrayExpr = A;
    CurrArrayIndexExpr = A->getIdx();

    // Skip array indexes since the pointer will index directly the right element
    TraverseStmt(A->getBase());
    CurrArrayExpr = nullptr;
    CurrArrayIndexExpr = nullptr;
    return true;
  }

};


/// A StmtVisitor that propagates the raw counts through the AST and
/// records the count at statements where the value may change.
struct FindKernelArguments : public RecursiveASTVisitor<FindKernelArguments> {

  CodeGenFunction &CGF;
  CodeGenModule &CGM;
  bool verbose ;

  ArraySubscriptExpr *CurrArrayExpr;
  Expr *CurrArrayIndexExpr;

  FindKernelArguments(CodeGenFunction &CGF)
    : CGF(CGF), CGM(CGF.CGM) {
    verbose = VERBOSE;
    CurrArrayExpr = NULL;
  }

  bool VisitDeclRefExpr(DeclRefExpr *D) {

    if(const VarDecl *VD = dyn_cast<VarDecl>(D->getDecl())) {
      if(verbose) llvm::errs() << ">>> Found use of Var = " << VD->getName();

      unsigned MapType = CGM.OpenMPSupport.getMapType(VD);

      if (verbose) llvm::errs() << " --> That's an argument";

      const Expr *RefExpr;

      if(CurrArrayExpr != nullptr) {
        RefExpr = CurrArrayExpr;
      } else {
        RefExpr = D;
      }

      if(MapType == OMP_TGT_MAPTYPE_TO) {
        CGM.OpenMPSupport.getOffloadingInputVarUse()[VD].push_back(RefExpr);
        if (verbose) llvm::errs() << " --> input";
      }
      else if (MapType == OMP_TGT_MAPTYPE_FROM) {
        CGM.OpenMPSupport.getOffloadingOutputVarDef()[VD].push_back(RefExpr);
        if (verbose) llvm::errs() << " --> output";
      }
      else {
        if (verbose) llvm::errs() << " --> euuh something";
      }

      if(verbose) llvm::errs() << "\n";

      if(CurrArrayExpr != nullptr && CurrArrayIndexExpr->IgnoreCasts()->isRValue()) {
        if(verbose) llvm::errs() << "Require reordering\n";
        CGM.OpenMPSupport.getReorderMap()[RefExpr] = CurrArrayIndexExpr->IgnoreCasts();

        //CurrArrayIndexExpr->Profile();
        //Finder.inputs;
      }

    }

    return true;
  }

  bool TraverseArraySubscriptExpr(ArraySubscriptExpr *A) {
    CurrArrayExpr = A;
    CurrArrayIndexExpr = A->getIdx();

    // Skip array indexes since the pointer will index directly the right element
    TraverseStmt(A->getBase());
    CurrArrayExpr = nullptr;
    CurrArrayIndexExpr = nullptr;
    return true;
  }

};

void CodeGenFunction::GenerateReductionKernel(const OMPReductionClause &C, const OMPExecutableDirective &S) {
  bool verbose = VERBOSE;

  DefineJNITypes();

  // Create the mapping function
  llvm::Module *mod = &(CGM.getModule());

  // Get JNI type
  llvm::StructType *StructTy_JNINativeInterface = mod->getTypeByName("struct.JNINativeInterface_");
  llvm::PointerType* PointerTy_JNINativeInterface = llvm::PointerType::get(StructTy_JNINativeInterface, 0);
  llvm::PointerType* PointerTy_1 = llvm::PointerType::get(PointerTy_JNINativeInterface, 0);

  llvm::StructType *StructTy_jobject = mod->getTypeByName("struct._jobject");
  llvm::PointerType* PointerTy_jobject = llvm::PointerType::get(StructTy_jobject, 0);

  for (OMPReductionClause::varlist_const_iterator I = C.varlist_begin(), E = C.varlist_end(); I != E; ++I) {

    const VarDecl *VD = cast<VarDecl>(cast<DeclRefExpr>(*I)->getDecl());
    QualType QTy = (*I)->getType();

    // Initialize arguments
    std::vector<llvm::Type*> FuncTy_args;

    // Add compulsary arguments
    FuncTy_args.push_back(PointerTy_1);
    FuncTy_args.push_back(PointerTy_jobject);

    FuncTy_args.push_back(PointerTy_jobject);
    FuncTy_args.push_back(PointerTy_jobject);

    llvm::FunctionType* FnTy = llvm::FunctionType::get(
          /*Result=*/PointerTy_jobject,
          /*Params=*/FuncTy_args,
          /*isVarArg=*/false);

    llvm::StringRef RedFnName = llvm::StringRef("Java_org_llvm_openmp_OmpKernel_reduceMethod" + VD->getNameAsString());

    llvm::Function *RedFn =
        llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage, RedFnName, mod);

    llvm::AttributeSet red_PAL;
    {
      SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        B.addAttribute(llvm::Attribute::StackProtect);
        B.addAttribute(llvm::Attribute::UWTable);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      red_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    RedFn->setAttributes(red_PAL);

    // Initialize a new CodeGenFunction used to generate the reduction
    CodeGenFunction CGF(CGM, true);
    CGF.CurFn = RedFn;
    CGF.EnsureInsertPoint();

    // Generate useful type and constant
    llvm::PointerType* PointerTy_4 = llvm::PointerType::get(CGF.Builder.getInt8Ty(), 0);
    llvm::PointerType* PointerTy_190 = llvm::PointerType::get(CGF.Builder.getInt32Ty(), 0);

    llvm::ArrayType* ArrayTy_0 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 13);
    llvm::ArrayType* ArrayTy_2 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 7);
    llvm::ArrayType* ArrayTy_4 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 40);
    llvm::ArrayType* ArrayTy_42 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 58);

    // Global variable
    llvm::GlobalVariable* gvar_array__str = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                     /*Type=*/ArrayTy_0,
                                                                     /*isConstant=*/true,
                                                                     /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                     /*Initializer=*/0,
                                                                     /*Name=*/".str");

    llvm::GlobalVariable* gvar_array__str_1 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                       /*Type=*/ArrayTy_2,
                                                                       /*isConstant=*/true,
                                                                       /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                       /*Initializer=*/0,
                                                                       /*Name=*/".str.1");

    llvm::GlobalVariable* gvar_array__str_2 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                       /*Type=*/ArrayTy_4,
                                                                       /*isConstant=*/true,
                                                                       /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                       /*Initializer=*/0,
                                                                       /*Name=*/".str.2");

    llvm::GlobalVariable* gvar_array__str_22 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                        /*Type=*/ArrayTy_42,
                                                                        /*isConstant=*/true,
                                                                        /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                        /*Initializer=*/0,
                                                                        /*Name=*/".str.22");


    // Generate useful type and constant

    llvm::ConstantInt* const_int32_254 = llvm::ConstantInt::get(getLLVMContext(), llvm::APInt(32, llvm::StringRef("0"), 10));
    llvm::ConstantInt* const_int64_252 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(64, llvm::StringRef("0"), 10));
    llvm::ConstantInt* const_int32_258 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, llvm::StringRef("4"), 10));
    llvm::ConstantInt* const_int32_255 = llvm::ConstantInt::get(getLLVMContext(), llvm::APInt(32, llvm::StringRef("184"), 10));
    llvm::ConstantInt* const_int64_266 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(64, llvm::StringRef("0"), 10));

    llvm::Constant *const_array_262 = llvm::ConstantDataArray::getString(mod->getContext(), "scala/Tuple2", true);
    llvm::Constant *const_array_263 = llvm::ConstantDataArray::getString(mod->getContext(), "<init>", true);
    llvm::Constant *const_array_264 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;)V", true);
    llvm::Constant *const_array_264_2 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V", true);

    llvm::ConstantPointerNull* const_ptr_256 = llvm::ConstantPointerNull::get(PointerTy_4);

    std::vector<llvm::Constant*> const_ptr_277_indices;
    const_ptr_277_indices.push_back(const_int64_266);
    const_ptr_277_indices.push_back(const_int64_266);

    // Init global variables
    gvar_array__str->setInitializer(const_array_262);
    gvar_array__str_1->setInitializer(const_array_263);
    gvar_array__str_2->setInitializer(const_array_264);
    gvar_array__str_22->setInitializer(const_array_264_2);

    // Allocate and load compulsry JNI arguments
    llvm::Function::arg_iterator args = RedFn->arg_begin();
    args->setName("env");
    llvm::AllocaInst* alloca_env = CGF.Builder.CreateAlloca(PointerTy_1);
    llvm::StoreInst* store_env = CGF.Builder.CreateStore(args, alloca_env);
    args++;
    args->setName("obj");
    llvm::AllocaInst* alloca_obj = CGF.Builder.CreateAlloca(PointerTy_jobject);
    llvm::StoreInst* store_obj = CGF.Builder.CreateStore(args, alloca_obj);
    args++;

    llvm::LoadInst* ptr_env = CGF.Builder.CreateLoad(alloca_env, "");
    llvm::LoadInst* ptr_270 = CGF.Builder.CreateLoad(ptr_env, "");

    std::vector<llvm::Value*> ptr_271_indices;
    ptr_271_indices.push_back(const_int32_254);
    ptr_271_indices.push_back(const_int32_255);

    llvm::Value* ptr_271 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_270, 0, 184);
    llvm::LoadInst* ptr_272 = CGF.Builder.CreateLoad(ptr_271, "");
    llvm::LoadInst* ptr_273 = CGF.Builder.CreateLoad(alloca_env, "");

    // Allocate, load and cast the first operand
    llvm::AllocaInst* alloca_arg1 = CGF.Builder.CreateAlloca(PointerTy_jobject);
    llvm::StoreInst* store_arg1 = CGF.Builder.CreateStore(args, alloca_arg1);

    llvm::LoadInst* ptr_274 = CGF.Builder.CreateLoad(alloca_arg1, "");
    std::vector<llvm::Value*> ptr_275_params;
    ptr_275_params.push_back(ptr_273);
    ptr_275_params.push_back(ptr_274);
    ptr_275_params.push_back(const_ptr_256);
    llvm::CallInst* ptr_275 = CGF.Builder.CreateCall(ptr_272, ptr_275_params);
    ptr_275->setCallingConv(llvm::CallingConv::C);
    ptr_275->setTailCall(false);
    llvm::AttributeSet ptr_275_PAL;
    ptr_275->setAttributes(ptr_275_PAL);
    llvm::Value* ptr_265 =  CGF.Builder.CreateBitCast(ptr_275, PointerTy_190);
    llvm::Value* ptr_265_3 = CGF.Builder.CreateLoad(ptr_265);
    llvm::Value* ptr_265_3_cast =  CGF.Builder.CreateBitCast(ptr_265_3, CGF.Builder.getInt32Ty());
    args++;

    // Allocate, load and cast the second operand
    llvm::LoadInst* ptr_env_2 = CGF.Builder.CreateLoad(alloca_env, "");
    llvm::LoadInst* ptr_270_2 = CGF.Builder.CreateLoad(ptr_env_2, "");

    std::vector<llvm::Value*> ptr_271_2_indices;
    ptr_271_2_indices.push_back(const_int32_254);
    ptr_271_2_indices.push_back(const_int32_255);

    llvm::Value* ptr_271_2 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_270_2, 0, 184);
    llvm::LoadInst* ptr_272_2 = CGF.Builder.CreateLoad(ptr_271_2, "");
    llvm::LoadInst* ptr_273_2 = CGF.Builder.CreateLoad(alloca_env, "");

    llvm::AllocaInst* alloca_arg2 = CGF.Builder.CreateAlloca(PointerTy_jobject);
    llvm::StoreInst* store_arg2 = CGF.Builder.CreateStore(args, alloca_arg2);

    llvm::LoadInst* ptr_274_1 = CGF.Builder.CreateLoad(alloca_arg2, "");
    std::vector<llvm::Value*> ptr_275_1_params;
    ptr_275_1_params.push_back(ptr_273_2);
    ptr_275_1_params.push_back(ptr_274_1);
    ptr_275_1_params.push_back(const_ptr_256);
    llvm::CallInst* ptr_275_1 = CGF.Builder.CreateCall(ptr_272_2, ptr_275_1_params);
    ptr_275_1->setCallingConv(llvm::CallingConv::C);
    ptr_275_1->setTailCall(false);
    llvm::AttributeSet ptr_275_1_PAL;
    ptr_275_1->setAttributes(ptr_275_1_PAL);
    llvm::Value* ptr_265_1 =  CGF.Builder.CreateBitCast(ptr_275_1, PointerTy_190);
    llvm::Value* ptr_265_2 = CGF.Builder.CreateLoad(ptr_265_1);
    llvm::Value* ptr_265_2_cast =  CGF.Builder.CreateBitCast(ptr_265_2, CGF.Builder.getInt32Ty());

    // Compute the reduction
    llvm::Value* res = nullptr;

    switch (C.getOperator()) {
      case OMPC_REDUCTION_or:
      case OMPC_REDUCTION_bitor:{
        res = CGF.Builder.CreateOr(ptr_265_3_cast, ptr_265_2_cast);
        break;
      }
      case OMPC_REDUCTION_bitxor:{
        res = CGF.Builder.CreateXor(ptr_265_3_cast, ptr_265_2_cast);
        break;
      }
      case OMPC_REDUCTION_sub:{
        res = CGF.Builder.CreateSub(ptr_265_3_cast, ptr_265_2_cast);
        break;
      }
      case OMPC_REDUCTION_add: {
        res = CGF.Builder.CreateAdd(ptr_265_3_cast, ptr_265_2_cast, "", false, true);
        break;
      }
      case OMPC_REDUCTION_and:
      case OMPC_REDUCTION_bitand: {
        res = CGF.Builder.CreateAnd(ptr_265_3_cast, ptr_265_2_cast);
        break;
      }
      case OMPC_REDUCTION_mult: {
        res = CGF.Builder.CreateMul(ptr_265_3_cast, ptr_265_2_cast);
        break;
      }
      case OMPC_REDUCTION_min: {
        break;
      }
      case OMPC_REDUCTION_max: {
        // TODO
        break;
      }
      case OMPC_REDUCTION_custom:
        llvm_unreachable("Custom initialization cannot be NULLed.");
      case OMPC_REDUCTION_unknown:
      case NUM_OPENMP_REDUCTION_OPERATORS:
        llvm_unreachable("Unkonwn operator kind.");
    }

    // Allocate and store the result
    llvm::AllocaInst* alloca_res = CGF.Builder.CreateAlloca(CGF.Builder.getInt32Ty());
    CGF.Builder.CreateStore(res,alloca_res);

    // Protect arg 1

    {
    llvm::LoadInst* ptr_xx = CGF.Builder.CreateLoad(ptr_env, "");
    std::vector<llvm::Value*> ptr_270_indices;
    ptr_270_indices.push_back(const_int64_252);
    ptr_270_indices.push_back(const_int32_255);
    llvm::Value* ptr_270 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_xx, 0, 192);
    llvm::LoadInst* ptr_271 = CGF.Builder.CreateLoad(ptr_270, "");

    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(ptr_274);
    void_272_params.push_back(ptr_275);
    void_272_params.push_back(const_int32_254);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_271, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);
    llvm::AttributeSet void_272_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      void_272_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    void_272->setAttributes(void_272_PAL);
    }

    // Protect arg 2

    {
    llvm::LoadInst* ptr_xx = CGF.Builder.CreateLoad(ptr_env, "");
    std::vector<llvm::Value*> ptr_270_indices;
    ptr_270_indices.push_back(const_int64_252);
    ptr_270_indices.push_back(const_int32_255);
    llvm::Value* ptr_270 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_xx, 0, 192);
    llvm::LoadInst* ptr_271 = CGF.Builder.CreateLoad(ptr_270, "");

    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(ptr_274_1);
    void_272_params.push_back(ptr_275_1);
    void_272_params.push_back(const_int32_254);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_271, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);
    llvm::AttributeSet void_272_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      void_272_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    void_272->setAttributes(void_272_PAL);
    }

    // Cast back the result to bit array
    llvm::LoadInst* ptr_27422 = CGF.Builder.CreateLoad(ptr_env, "");
    llvm::Value* ptr_275_2 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_27422, 0, 176);
    llvm::LoadInst* ptr_276 = CGF.Builder.CreateLoad(ptr_275_2, "");
    std::vector<llvm::Value*> ptr_277_params;
    ptr_277_params.push_back(ptr_env);
    ptr_277_params.push_back(const_int32_258); // TOFIX: That should the size in byte of the element
    llvm::CallInst* ptr_277 = CGF.Builder.CreateCall(ptr_276, ptr_277_params);
    ptr_277->setCallingConv(llvm::CallingConv::C);
    ptr_277->setTailCall(true);
    llvm::AttributeSet ptr_277_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_277_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_277->setAttributes(ptr_277_PAL);

    llvm::LoadInst* ptr_278 = CGF.Builder.CreateLoad(ptr_env, "");
    llvm::Value* ptr_279 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_278, 0, 208);
    llvm::LoadInst* ptr_280 = CGF.Builder.CreateLoad(ptr_279, "");
    llvm::Value* ptr_res_cast = CGF.Builder.CreateBitCast(alloca_res, PointerTy_4, "");
    std::vector<llvm::Value*> void_281_params;
    void_281_params.push_back(ptr_env);
    void_281_params.push_back(ptr_277);
    void_281_params.push_back(const_int32_254);
    void_281_params.push_back(const_int32_258); // TOFIX: That should the size in byte of the element
    void_281_params.push_back(ptr_res_cast);
    llvm::CallInst* void_281 = CGF.Builder.CreateCall(ptr_280, void_281_params);
    void_281->setCallingConv(llvm::CallingConv::C);
    void_281->setTailCall(false);
    llvm::AttributeSet void_281_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      void_281_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    void_281->setAttributes(void_281_PAL);

    llvm::ReturnInst *ret = CGF.Builder.CreateRet(ptr_277);

  }
}


void CodeGenFunction::GenerateMappingKernel(const OMPExecutableDirective &S) {
  bool verbose = VERBOSE;

  DefineJNITypes();

  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& typeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();
  auto& indexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();

  if(verbose) llvm::errs() << "Offloaded variables \n";
  for(auto iter = typeMap.begin(); iter!= typeMap.end(); ++iter) {
    if(verbose) llvm::errs() << iter->first->getName() << " - " << iter->second << " - " << indexMap[iter->first] << "\n";
  }

  const Stmt *Body = S.getAssociatedStmt();

  if (const CapturedStmt *CS = dyn_cast_or_null<CapturedStmt>(Body))
    Body = CS->getCapturedStmt();
  //for (unsigned I = 0; I < getCollapsedNumberFromLoopDirective(&S); ++I) {
  bool SkippedContainers = false;
  while (!SkippedContainers) {
    if (const AttributedStmt *AS = dyn_cast_or_null<AttributedStmt>(Body))
      Body = AS->getSubStmt();
    else if (const CompoundStmt *CS =
             dyn_cast_or_null<CompoundStmt>(Body)) {
      if (CS->size() != 1) {
        SkippedContainers = true;
      } else {
        Body = CS->body_back();
      }
    } else
      SkippedContainers = true;
  }
  const ForStmt *For = dyn_cast_or_null<ForStmt>(Body);
  Body = For->getBody();



  // Detect input/output expression from the loop body
  Stmt *Body2 = const_cast<Stmt*>(Body);
  FindKernelArguments Finder(*this);
  Finder.TraverseStmt(Body2);

  EmitSparkJob();


  GenerateReorderingKernels();

  // Create the mapping function
  llvm::Module *mod = &(CGM.getModule());

  // Get JNI type
  llvm::StructType *StructTy_JNINativeInterface = mod->getTypeByName("struct.JNINativeInterface_");
  llvm::PointerType* PointerTy_JNINativeInterface = llvm::PointerType::get(StructTy_JNINativeInterface, 0);
  llvm::PointerType* PointerTy_1 = llvm::PointerType::get(PointerTy_JNINativeInterface, 0);

  llvm::StructType *StructTy_jobject = mod->getTypeByName("struct._jobject");
  llvm::PointerType* PointerTy_jobject = llvm::PointerType::get(StructTy_jobject, 0);

  // Initialize arguments
  std::vector<llvm::Type*> FuncTy_args;

  // Add compulsary arguments
  FuncTy_args.push_back(PointerTy_1);
  FuncTy_args.push_back(PointerTy_jobject);

  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it){
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      FuncTy_args.push_back(PointerTy_jobject);
    }
  }

  llvm::FunctionType* FnTy = llvm::FunctionType::get(
        /*Result=*/PointerTy_jobject,
        /*Params=*/FuncTy_args,
        /*isVarArg=*/false);

  llvm::Function *MapFn =
      llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                             "Java_org_llvm_openmp_OmpKernel_mappingMethod", &CGM.getModule());

  llvm::AttributeSet map_PAL;
  {
    SmallVector<llvm::AttributeSet, 4> Attrs;
    llvm::AttributeSet PAS;
    {
      llvm::AttrBuilder B;
      B.addAttribute(llvm::Attribute::NoUnwind);
      B.addAttribute(llvm::Attribute::StackProtect);
      B.addAttribute(llvm::Attribute::UWTable);
      PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
    }

    Attrs.push_back(PAS);
    map_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

  }
  MapFn->setAttributes(map_PAL);


  // Initialize a new CodeGenFunction used to generate the mapping
  CodeGenFunction CGF(CGM, true);
  CGF.CurFn = MapFn;
  CGF.EnsureInsertPoint();

  // Generate useful type and constant
  llvm::PointerType* PointerTy_4 = llvm::PointerType::get(CGF.Builder.getInt8Ty(), 0);
  llvm::PointerType* PointerTy_190 = llvm::PointerType::get(CGF.Builder.getInt32Ty(), 0);

  llvm::ArrayType* ArrayTy_0 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 13);
  llvm::ArrayType* ArrayTy_2 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 7);
  llvm::ArrayType* ArrayTy_4 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 40);
  llvm::ArrayType* ArrayTy_42 = llvm::ArrayType::get(llvm::IntegerType::get(mod->getContext(), 8), 58);

  // Global variable
  llvm::GlobalVariable* gvar_array__str = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                   /*Type=*/ArrayTy_0,
                                                                   /*isConstant=*/true,
                                                                   /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                   /*Initializer=*/0,
                                                                   /*Name=*/".str");

  llvm::GlobalVariable* gvar_array__str2 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                    /*Type=*/ArrayTy_0,
                                                                    /*isConstant=*/true,
                                                                    /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                    /*Initializer=*/0,
                                                                    /*Name=*/".str");

  llvm::GlobalVariable* gvar_array__str_1 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                     /*Type=*/ArrayTy_2,
                                                                     /*isConstant=*/true,
                                                                     /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                     /*Initializer=*/0,
                                                                     /*Name=*/".str.1");

  llvm::GlobalVariable* gvar_array__str_2 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                     /*Type=*/ArrayTy_4,
                                                                     /*isConstant=*/true,
                                                                     /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                     /*Initializer=*/0,
                                                                     /*Name=*/".str.2");

  llvm::GlobalVariable* gvar_array__str_22 = new llvm::GlobalVariable(/*Module=*/*mod,
                                                                      /*Type=*/ArrayTy_42,
                                                                      /*isConstant=*/true,
                                                                      /*Linkage=*/llvm::GlobalValue::PrivateLinkage,
                                                                      /*Initializer=*/0,
                                                                      /*Name=*/".str.22");


  // Generate useful type and constant

  llvm::ConstantInt* const_int32_254 = llvm::ConstantInt::get(getLLVMContext(), llvm::APInt(32, llvm::StringRef("0"), 10));
  llvm::ConstantInt* const_int64_252 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(64, llvm::StringRef("0"), 10));
  llvm::ConstantInt* const_int32_258 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, llvm::StringRef("4"), 10));
  llvm::ConstantInt* const_int32_255 = llvm::ConstantInt::get(getLLVMContext(), llvm::APInt(32, llvm::StringRef("184"), 10));
  llvm::ConstantInt* const_int64_266 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(64, llvm::StringRef("0"), 10));

  llvm::Constant *const_array_262 = llvm::ConstantDataArray::getString(mod->getContext(), "scala/Tuple2", true);
  llvm::Constant *const_array_262_2 = llvm::ConstantDataArray::getString(mod->getContext(), "scala/Tuple3", true);
  llvm::Constant *const_array_263 = llvm::ConstantDataArray::getString(mod->getContext(), "<init>", true);
  llvm::Constant *const_array_264 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;)V", true);
  llvm::Constant *const_array_264_2 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V", true);

  std::vector<llvm::Constant*> const_ptr_277_indices;
  const_ptr_277_indices.push_back(const_int64_266);
  const_ptr_277_indices.push_back(const_int64_266);
  llvm::Constant* const_ptr_277 = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar_array__str, const_ptr_277_indices);

  std::vector<llvm::Constant*> const_ptr_277_2_indices;
  const_ptr_277_2_indices.push_back(const_int64_266);
  const_ptr_277_2_indices.push_back(const_int64_266);
  llvm::Constant* const_ptr_277_2 = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar_array__str2, const_ptr_277_2_indices);

  std::vector<llvm::Constant*> const_ptr_279_indices;
  const_ptr_279_indices.push_back(const_int64_266);
  const_ptr_279_indices.push_back(const_int64_266);
  llvm::Constant* const_ptr_279 = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar_array__str_1, const_ptr_279_indices);

  std::vector<llvm::Constant*> const_ptr_280_indices;
  const_ptr_280_indices.push_back(const_int64_266);
  const_ptr_280_indices.push_back(const_int64_266);
  llvm::Constant* const_ptr_280 = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar_array__str_2, const_ptr_280_indices);

  std::vector<llvm::Constant*> const_ptr_280_2_indices;
  const_ptr_280_2_indices.push_back(const_int64_266);
  const_ptr_280_2_indices.push_back(const_int64_266);
  llvm::Constant* const_ptr_280_2 = llvm::ConstantExpr::getGetElementPtr(nullptr, gvar_array__str_22, const_ptr_280_2_indices);

  // Init global variables
  gvar_array__str->setInitializer(const_array_262);
  gvar_array__str2->setInitializer(const_array_262_2);
  gvar_array__str_1->setInitializer(const_array_263);
  gvar_array__str_2->setInitializer(const_array_264);
  gvar_array__str_22->setInitializer(const_array_264_2);

  // Allocate and load compulsry JNI arguments
  llvm::Function::arg_iterator args = MapFn->arg_begin();
  args->setName("env");
  llvm::AllocaInst* alloca_env = CGF.Builder.CreateAlloca(PointerTy_1);
  llvm::StoreInst* store_env = CGF.Builder.CreateStore(args, alloca_env);
  args++;
  args->setName("obj");
  llvm::AllocaInst* alloca_obj = CGF.Builder.CreateAlloca(PointerTy_jobject);
  llvm::StoreInst* store_obj = CGF.Builder.CreateStore(args, alloca_obj);
  args++;

  llvm::LoadInst* ptr_env = CGF.Builder.CreateLoad(alloca_env, "");
  llvm::LoadInst* ptr_270 = CGF.Builder.CreateLoad(ptr_env, "");

  std::vector<llvm::Value*> ptr_271_indices;
  ptr_271_indices.push_back(const_int32_254);
  ptr_271_indices.push_back(const_int32_255);

  llvm::Value* ptr_271 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_270, 0, 184);
  llvm::LoadInst* ptr_272 = CGF.Builder.CreateLoad(ptr_271, "");
  llvm::LoadInst* ptr_273 = CGF.Builder.CreateLoad(alloca_env, "");

  // Keep values that have to be used for releasing.
  llvm::SmallVector<llvm::Value*, 8> VecPtrBarrays;
  llvm::SmallVector<llvm::Value*, 8> VecPtrValues;

  // Allocate, load and cast input variables (i.e. the arguments)
  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it){
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      const VarDecl *VD = it->first;
      llvm::SmallVector<const Expr*, 8> DefExprs = it->second;

      args->setName(VD->getName());
      llvm::AllocaInst* alloca_arg = CGF.Builder.CreateAlloca(PointerTy_jobject);
      llvm::StoreInst* store_arg = CGF.Builder.CreateStore(args, alloca_arg);

      llvm::ConstantPointerNull* const_ptr_256 = llvm::ConstantPointerNull::get(PointerTy_4);

      llvm::LoadInst* ptr_274 = CGF.Builder.CreateLoad(alloca_arg, "");
      std::vector<llvm::Value*> ptr_275_params;
      ptr_275_params.push_back(ptr_273);
      ptr_275_params.push_back(ptr_274);
      ptr_275_params.push_back(const_ptr_256);
      llvm::CallInst* ptr_275 = CGF.Builder.CreateCall(ptr_272, ptr_275_params);
      ptr_275->setCallingConv(llvm::CallingConv::C);
      ptr_275->setTailCall(false);
      llvm::AttributeSet ptr_275_PAL;
      ptr_275->setAttributes(ptr_275_PAL);
      llvm::Value* ptr_265 =  CGF.Builder.CreateBitCast(ptr_275, PointerTy_190);

      VecPtrBarrays.push_back(ptr_274);
      VecPtrValues.push_back(ptr_275);


      CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, ptr_265);
      args++;
    }
  }

  // Allocate output variables
  for (auto it = CGM.OpenMPSupport.getOffloadingOutputVarDef().begin(); it != CGM.OpenMPSupport.getOffloadingOutputVarDef().end(); ++it)
  {
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      const VarDecl *VD = it->first;

      // Find the type of one element
      QualType varType = VD->getType();
      while(varType->isAnyPointerType()) {
        varType = varType->getPointeeType();
      }

      llvm::Type *TyObject = ConvertType(varType);
      llvm::AllocaInst* alloca_res = CGF.Builder.CreateAlloca(TyObject);

      CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, alloca_res);
    }
  }

  CGF.EmitStmt(Body);
  if(verbose) Body->dump();

  auto ptrBarray = VecPtrBarrays.begin();
  auto ptrValue = VecPtrValues.begin();

  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it){
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      const VarDecl *VD = it->first;
      llvm::SmallVector<const Expr*, 8> DefExprs = it->second;

      llvm::LoadInst* ptr_xx = CGF.Builder.CreateLoad(ptr_env, "");
      std::vector<llvm::Value*> ptr_270_indices;
      ptr_270_indices.push_back(const_int64_252);
      ptr_270_indices.push_back(const_int32_255);
      llvm::Value* ptr_270 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_xx, 0, 192);
      llvm::LoadInst* ptr_271 = CGF.Builder.CreateLoad(ptr_270, "");

      std::vector<llvm::Value*> void_272_params;
      void_272_params.push_back(ptr_env);
      void_272_params.push_back(*ptrBarray);
      void_272_params.push_back(*ptrValue);
      void_272_params.push_back(const_int32_254);
      llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_271, void_272_params);
      void_272->setCallingConv(llvm::CallingConv::C);
      void_272->setTailCall(true);
      llvm::AttributeSet void_272_PAL;
      {
        llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
        llvm::AttributeSet PAS;
        {
          llvm::AttrBuilder B;
          B.addAttribute(llvm::Attribute::NoUnwind);
          PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
        }

        Attrs.push_back(PAS);
        void_272_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

      }
      void_272->setAttributes(void_272_PAL);

      ptrBarray++;
      ptrValue++;
    }
  }

  llvm::SmallVector<llvm::Value*, 8> results;

  for (auto it = CGM.OpenMPSupport.getOffloadingOutputVarDef().begin(); it != CGM.OpenMPSupport.getOffloadingOutputVarDef().end(); ++it)
  {
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      const VarDecl *VD = it->first;
      llvm::Type *TyObject = ConvertType(VD->getType());
      llvm::Value *ptr_result = CGM.OpenMPSupport.getOpenMPKernelArgVar(*it2);

      llvm::Value* ptr_273 = CGF.Builder.CreateBitCast(ptr_result, PointerTy_4, "");
      llvm::LoadInst* ptr_274 = CGF.Builder.CreateLoad(ptr_env, "");
      llvm::Value* ptr_275 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_274, 0, 176);
      llvm::LoadInst* ptr_276 = CGF.Builder.CreateLoad(ptr_275, "");
      std::vector<llvm::Value*> ptr_277_params;
      ptr_277_params.push_back(ptr_env);
      ptr_277_params.push_back(const_int32_258); // TOFIX: That should the size in byte of the element
      llvm::CallInst* ptr_277 = CGF.Builder.CreateCall(ptr_276, ptr_277_params);
      ptr_277->setCallingConv(llvm::CallingConv::C);
      ptr_277->setTailCall(true);
      llvm::AttributeSet ptr_277_PAL;
      {
        llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
        llvm::AttributeSet PAS;
        {
          llvm::AttrBuilder B;
          B.addAttribute(llvm::Attribute::NoUnwind);
          PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
        }

        Attrs.push_back(PAS);
        ptr_277_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

      }
      ptr_277->setAttributes(ptr_277_PAL);

      llvm::LoadInst* ptr_278 = CGF.Builder.CreateLoad(ptr_env, "");
      llvm::Value* ptr_279 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_278, 0, 208);
      llvm::LoadInst* ptr_280 = CGF.Builder.CreateLoad(ptr_279, "");
      std::vector<llvm::Value*> void_281_params;
      void_281_params.push_back(ptr_env);
      void_281_params.push_back(ptr_277);
      void_281_params.push_back(const_int32_254);
      void_281_params.push_back(const_int32_258); // TOFIX: That should the size in byte of the element
      void_281_params.push_back(ptr_273);
      llvm::CallInst* void_281 = CGF.Builder.CreateCall(ptr_280, void_281_params);
      void_281->setCallingConv(llvm::CallingConv::C);
      void_281->setTailCall(false);
      llvm::AttributeSet void_281_PAL;
      {
        llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
        llvm::AttributeSet PAS;
        {
          llvm::AttrBuilder B;
          B.addAttribute(llvm::Attribute::NoUnwind);
          PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
        }

        Attrs.push_back(PAS);
        void_281_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

      }
      void_281->setAttributes(void_281_PAL);

      results.push_back(ptr_277);
      //llvm::ReturnInst *ret = CGF.Builder.CreateRet(ptr_277);

      //for(auto def = DefExprs.begin(); def != DefExprs.end(); def++)
      //CGM.OpenMPSupport.addOpenMPKernelArgVar(*def, alloca_res);
    }
  }

  unsigned NbOutputs = 0;
  for(auto it = CGM.OpenMPSupport.getOffloadingOutputVarDef().begin(); it != CGM.OpenMPSupport.getOffloadingOutputVarDef().end(); ++it)
    NbOutputs += it->second.size();

  if(NbOutputs == 1) {
    // Just return the value
    llvm::ReturnInst *ret = CGF.Builder.CreateRet(results.front());
  } else if (NbOutputs == 2) {
    // Construct and return a Tuple2

    llvm::LoadInst* ptr_327 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_328 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_327, 0, 6);
    llvm::LoadInst* ptr_329 = CGF.Builder.CreateLoad(ptr_328, false);
    std::vector<llvm::Value*> ptr_330_params;
    ptr_330_params.push_back(ptr_env);
    ptr_330_params.push_back(const_ptr_277);
    llvm::CallInst* ptr_330 = CGF.Builder.CreateCall(ptr_329, ptr_330_params);
    ptr_330->setCallingConv(llvm::CallingConv::C);
    ptr_330->setTailCall(false);
    llvm::AttributeSet ptr_330_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_330_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_330->setAttributes(ptr_330_PAL);

    llvm::LoadInst* ptr_331 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_332 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_331, 0, 33);
    llvm::LoadInst* ptr_333 = CGF.Builder.CreateLoad(ptr_332, false);
    std::vector<llvm::Value*> ptr_334_params;
    ptr_334_params.push_back(ptr_env);
    ptr_334_params.push_back(ptr_330);
    ptr_334_params.push_back(const_ptr_279);
    ptr_334_params.push_back(const_ptr_280);
    llvm::CallInst* ptr_334 = CGF.Builder.CreateCall(ptr_333, ptr_334_params);
    ptr_334->setCallingConv(llvm::CallingConv::C);
    ptr_334->setTailCall(false);
    llvm::AttributeSet ptr_334_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_334_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_334->setAttributes(ptr_334_PAL);

    llvm::LoadInst* ptr_335 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_336 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_335, 0, 28);
    llvm::LoadInst* ptr_337 = CGF.Builder.CreateLoad(ptr_336, false);
    std::vector<llvm::Value*> ptr_338_params;
    ptr_338_params.push_back(ptr_env);
    ptr_338_params.push_back(ptr_330);
    ptr_338_params.push_back(ptr_334);
    ptr_338_params.push_back(results[0]);
    ptr_338_params.push_back(results[1]);
    llvm::CallInst* ptr_338 = CGF.Builder.CreateCall(ptr_337, ptr_338_params);
    ptr_338->setCallingConv(llvm::CallingConv::C);
    ptr_338->setTailCall(false);
    llvm::AttributeSet ptr_338_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_338_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_338->setAttributes(ptr_338_PAL);

    llvm::ReturnInst *ret = CGF.Builder.CreateRet(ptr_338);
  } else if (NbOutputs == 3) {
    // Construct and return a Tuple3

    llvm::LoadInst* ptr_327 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_328 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_327, 0, 6);
    llvm::LoadInst* ptr_329 = CGF.Builder.CreateLoad(ptr_328, false);
    std::vector<llvm::Value*> ptr_330_params;
    ptr_330_params.push_back(ptr_env);
    ptr_330_params.push_back(const_ptr_277_2);
    llvm::CallInst* ptr_330 = CGF.Builder.CreateCall(ptr_329, ptr_330_params);
    ptr_330->setCallingConv(llvm::CallingConv::C);
    ptr_330->setTailCall(false);
    llvm::AttributeSet ptr_330_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_330_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_330->setAttributes(ptr_330_PAL);

    llvm::LoadInst* ptr_331 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_332 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_331, 0, 33);
    llvm::LoadInst* ptr_333 = CGF.Builder.CreateLoad(ptr_332, false);
    std::vector<llvm::Value*> ptr_334_params;
    ptr_334_params.push_back(ptr_env);
    ptr_334_params.push_back(ptr_330);
    ptr_334_params.push_back(const_ptr_279);
    ptr_334_params.push_back(const_ptr_280_2);
    llvm::CallInst* ptr_334 = CGF.Builder.CreateCall(ptr_333, ptr_334_params);
    ptr_334->setCallingConv(llvm::CallingConv::C);
    ptr_334->setTailCall(false);
    llvm::AttributeSet ptr_334_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_334_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_334->setAttributes(ptr_334_PAL);

    llvm::LoadInst* ptr_335 = CGF.Builder.CreateLoad(ptr_env, false);
    llvm::Value* ptr_336 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_335, 0, 28);
    llvm::LoadInst* ptr_337 = CGF.Builder.CreateLoad(ptr_336, false);
    std::vector<llvm::Value*> ptr_338_params;
    ptr_338_params.push_back(ptr_env);
    ptr_338_params.push_back(ptr_330);
    ptr_338_params.push_back(ptr_334);
    ptr_338_params.push_back(results[0]);
    ptr_338_params.push_back(results[1]);
    ptr_338_params.push_back(results[2]);
    llvm::CallInst* ptr_338 = CGF.Builder.CreateCall(ptr_337, ptr_338_params);
    ptr_338->setCallingConv(llvm::CallingConv::C);
    ptr_338->setTailCall(false);
    llvm::AttributeSet ptr_338_PAL;
    {
      llvm::SmallVector<llvm::AttributeSet, 4> Attrs;
      llvm::AttributeSet PAS;
      {
        llvm::AttrBuilder B;
        B.addAttribute(llvm::Attribute::NoUnwind);
        PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
      }

      Attrs.push_back(PAS);
      ptr_338_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

    }
    ptr_338->setAttributes(ptr_338_PAL);

    llvm::ReturnInst *ret = CGF.Builder.CreateRet(ptr_338);
  } else if (NbOutputs == 4) {
    // Construct and return a Collection
  }
}

void CodeGenFunction::GenerateReorderingKernels() {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();

  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        FindIndexingArguments Finder(*this);
        Expr *expr = const_cast<Expr*>(reorderExpr);
        Finder.TraverseStmt(expr);
        GenerateReorderingKernel(it->first, Finder.InputsMap, reorderExpr);
      }
    }
  }

  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        FindIndexingArguments Finder(*this);
        Expr *expr = const_cast<Expr*>(reorderExpr);
        Finder.TraverseStmt(expr);
        GenerateReorderingKernel(it->first, Finder.InputsMap, reorderExpr);
      }
    }
  }
}

void CodeGenFunction::GenerateReorderingKernel(const VarDecl* VD, llvm::DenseMap<const VarDecl*, llvm::SmallVector<const Expr*, 8>> InputsMap, const Expr* expression) {
  bool verbose = VERBOSE;

  CodeGenFunction CGF(CGM, true);
  DefineJNITypes();

  // Create the mapping function
  llvm::Module *mod = &(CGM.getModule());

  // Get JNI type
  llvm::StructType *StructTy_JNINativeInterface = mod->getTypeByName("struct.JNINativeInterface_");
  llvm::PointerType* PointerTy_JNINativeInterface = llvm::PointerType::get(StructTy_JNINativeInterface, 0);
  llvm::PointerType* PointerTy_1 = llvm::PointerType::get(PointerTy_JNINativeInterface, 0);

  llvm::StructType *StructTy_jobject = mod->getTypeByName("struct._jobject");
  llvm::PointerType* PointerTy_jobject = llvm::PointerType::get(StructTy_jobject, 0);

  llvm::IntegerType *IntTy_jlong = CGF.Builder.getInt64Ty();
  llvm::IntegerType *IntTy_jint = CGF.Builder.getInt32Ty();


  // Initialize arguments
  std::vector<llvm::Type*> FuncTy_args;

  // Add compulsary arguments
  FuncTy_args.push_back(PointerTy_1);
  FuncTy_args.push_back(PointerTy_jobject);

  for(auto it = InputsMap.begin(); it != InputsMap.end(); ++it) {
    FuncTy_args.push_back(IntTy_jlong);
  }

  llvm::FunctionType* FnTy = llvm::FunctionType::get(
        /*Result=*/IntTy_jlong,
        /*Params=*/FuncTy_args,
        /*isVarArg=*/false);

  llvm::FoldingSetNodeID ExprID;
  expression->Profile(ExprID, getContext(), true);

  llvm::StringRef ReordFnName = llvm::StringRef("Java_org_llvm_openmp_OmpKernel_reorderMethod" + std::to_string(ExprID.ComputeHash()));

  if(verbose) llvm::errs() << ReordFnName.str() << "\n";

  llvm::Function *ReordFn =
      llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage, ReordFnName, mod);

  llvm::AttributeSet reord_PAL;
  {
    SmallVector<llvm::AttributeSet, 4> Attrs;
    llvm::AttributeSet PAS;
    {
      llvm::AttrBuilder B;
      B.addAttribute(llvm::Attribute::NoUnwind);
      B.addAttribute(llvm::Attribute::StackProtect);
      B.addAttribute(llvm::Attribute::UWTable);
      PAS = llvm::AttributeSet::get(mod->getContext(), ~0U, B);
    }

    Attrs.push_back(PAS);
    reord_PAL = llvm::AttributeSet::get(mod->getContext(), Attrs);

  }
  ReordFn->setAttributes(reord_PAL);

  // Initialize a new CodeGenFunction used to generate the reduction

  CGF.CurFn = ReordFn;
  CGF.EnsureInsertPoint();

  // Allocate, load and cast input variables (i.e. the arguments)
  llvm::Function::arg_iterator args = ReordFn->arg_begin();
  args++;
  args++;

  for (auto it = InputsMap.begin(); it != InputsMap.end(); ++it)
  {
    const VarDecl *VD = it->first;
    llvm::SmallVector<const Expr*, 8> DefExprs = it->second;

    //args->setName(VD->getName());
    //llvm::AllocaInst* alloca_arg = CGF.Builder.CreateAlloca(IntTy_jlong);
    //llvm::StoreInst* store_arg = CGF.Builder.CreateStore(args, alloca_arg);
    //llvm::LoadInst* load_arg = CGF.Builder.CreateLoad(alloca_arg, "");
    llvm::AllocaInst* alloca_cast = CGF.Builder.CreateAlloca(IntTy_jint);
    llvm::Value* cast = CGF.Builder.CreateTruncOrBitCast(args, IntTy_jint);
    llvm::StoreInst* store_cast = CGF.Builder.CreateStore(cast, alloca_cast);

    for(auto use = DefExprs.begin(); use != DefExprs.end(); use++)
      CGM.OpenMPSupport.addOpenMPKernelArgVar(*use, alloca_cast);
    args++;

  }

  llvm::Value* index = CGF.EmitScalarExpr(expression);
  llvm::Value* res = CGF.Builder.CreateSExtOrBitCast(index, IntTy_jlong);
  CGF.Builder.CreateRet(res);
}


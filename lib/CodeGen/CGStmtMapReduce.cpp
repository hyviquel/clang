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
#include "clang/AST/StmtVisitor.h"

#define VERBOSE 1

using namespace clang;
using namespace CodeGen;

Expr *CodeGenFunction::ActOnIntegerConstant(SourceLocation Loc, uint64_t Val) {
  unsigned IntSize = getContext().getTargetInfo().getIntWidth();
  return IntegerLiteral::Create(getContext(), llvm::APInt(IntSize, Val),
                                getContext().IntTy, Loc);
}


namespace {
  class ForInitChecker : public StmtVisitor<ForInitChecker, Decl *> {
    class ForInitVarChecker : public StmtVisitor<ForInitVarChecker, Decl *> {
    public:
      VarDecl *VisitDeclRefExpr(DeclRefExpr *E) {
        return dyn_cast_or_null<VarDecl>(E->getDecl());
      }
      Decl *VisitStmt(Stmt *S) { return 0; }
      ForInitVarChecker() {}
    } VarChecker;
    Expr *InitValue;

  public:
    Decl *VisitBinaryOperator(BinaryOperator *BO) {
      if (BO->getOpcode() != BO_Assign)
        return 0;

      InitValue = BO->getRHS();
      return VarChecker.Visit(BO->getLHS());
    }
    Decl *VisitDeclStmt(DeclStmt *S) {
      if (S->isSingleDecl()) {
        VarDecl *Var = dyn_cast_or_null<VarDecl>(S->getSingleDecl());
        if (Var && Var->hasInit()) {
          if (CXXConstructExpr *Init =
              dyn_cast<CXXConstructExpr>(Var->getInit())) {
            if (Init->getNumArgs() != 1)
              return 0;
            InitValue = Init->getArg(0);
          } else {
            InitValue = Var->getInit();
          }
          return Var;
        }
      }
      return 0;
    }
    Decl *VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
      switch (E->getOperator()) {
        case OO_Equal:
          InitValue = E->getArg(1);
          return VarChecker.Visit(E->getArg(0));
        default:
          break;
      }
      return 0;
    }
    Decl *VisitStmt(Stmt *S) { return 0; }
    ForInitChecker() : VarChecker(), InitValue(0) {}
    Expr *getInitValue() { return InitValue; }
  };

  class ForVarChecker : public StmtVisitor<ForVarChecker, bool> {
    Decl *InitVar;

  public:
    bool VisitDeclRefExpr(DeclRefExpr *E) { return E->getDecl() == InitVar; }
    bool VisitImplicitCastExpr(ImplicitCastExpr *E) {
      return Visit(E->getSubExpr());
    }
    bool VisitStmt(Stmt *S) { return false; }
    ForVarChecker(Decl *D) : InitVar(D) {}
  };

  class ForTestChecker : public StmtVisitor<ForTestChecker, bool> {
    ForVarChecker VarChecker;
    Expr *CheckValue;
    bool IsLessOp;
    bool IsStrictOp;

  public:
    bool VisitBinaryOperator(BinaryOperator *BO) {
      if (!BO->isRelationalOp())
        return false;
      if (VarChecker.Visit(BO->getLHS())) {
        CheckValue = BO->getRHS();
        IsLessOp = BO->getOpcode() == BO_LT || BO->getOpcode() == BO_LE;
        IsStrictOp = BO->getOpcode() == BO_LT || BO->getOpcode() == BO_GT;
      } else if (VarChecker.Visit(BO->getRHS())) {
        CheckValue = BO->getLHS();
        IsLessOp = BO->getOpcode() == BO_GT || BO->getOpcode() == BO_GE;
        IsStrictOp = BO->getOpcode() == BO_LT || BO->getOpcode() == BO_GT;
      }
      return CheckValue != 0;
    }
    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
      switch (E->getOperator()) {
        case OO_Greater:
        case OO_GreaterEqual:
        case OO_Less:
        case OO_LessEqual:
          break;
        default:
          return false;
      }
      if (E->getNumArgs() != 2)
        return false;

      if (VarChecker.Visit(E->getArg(0))) {
        CheckValue = E->getArg(1);
        IsLessOp =
            E->getOperator() == OO_Less || E->getOperator() == OO_LessEqual;
        IsStrictOp = E->getOperator() == OO_Less;
      } else if (VarChecker.Visit(E->getArg(1))) {
        CheckValue = E->getArg(0);
        IsLessOp =
            E->getOperator() == OO_Greater || E->getOperator() == OO_GreaterEqual;
        IsStrictOp = E->getOperator() == OO_Greater;
      }

      return CheckValue != 0;
    }
    bool VisitStmt(Stmt *S) { return false; }
    ForTestChecker(Decl *D)
      : VarChecker(D), CheckValue(0), IsLessOp(false), IsStrictOp(false) {}
    Expr *getCheckValue() { return CheckValue; }
    bool isLessOp() const { return IsLessOp; }
    bool isStrictOp() const { return IsStrictOp; }
  };

  class ForIncrChecker : public StmtVisitor<ForIncrChecker, bool> {
    ForVarChecker VarChecker;
    class ForIncrExprChecker : public StmtVisitor<ForIncrExprChecker, bool> {
      ForVarChecker VarChecker;
      Expr *StepValue;
      bool IsIncrement;

    public:
      bool VisitBinaryOperator(BinaryOperator *BO) {
        if (!BO->isAdditiveOp())
          return false;
        if (BO->getOpcode() == BO_Add) {
          IsIncrement = true;
          if (VarChecker.Visit(BO->getLHS()))
            StepValue = BO->getRHS();
          else if (VarChecker.Visit(BO->getRHS()))
            StepValue = BO->getLHS();
          return StepValue != 0;
        }
        // BO_Sub
        if (VarChecker.Visit(BO->getLHS()))
          StepValue = BO->getRHS();
        return StepValue != 0;
      }
      bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
        switch (E->getOperator()) {
          case OO_Plus:
            IsIncrement = true;
            if (VarChecker.Visit(E->getArg(0)))
              StepValue = E->getArg(1);
            else if (VarChecker.Visit(E->getArg(1)))
              StepValue = E->getArg(0);
            return StepValue != 0;
          case OO_Minus:
            if (VarChecker.Visit(E->getArg(0)))
              StepValue = E->getArg(1);
            return StepValue != 0;
          default:
            return false;
        }
      }
      bool VisitStmt(Stmt *S) { return false; }
      ForIncrExprChecker(ForVarChecker &C)
        : VarChecker(C), StepValue(0), IsIncrement(false) {}
      Expr *getStepValue() { return StepValue; }
      bool isIncrement() const { return IsIncrement; }
    } ExprChecker;
    Expr *StepValue;
    CodeGenFunction &Actions;
    bool IsLessOp, IsCompatibleWithTest;

  public:
    bool VisitUnaryOperator(UnaryOperator *UO) {
      if (!UO->isIncrementDecrementOp())
        return false;
      if (VarChecker.Visit(UO->getSubExpr())) {
        IsCompatibleWithTest = (IsLessOp && UO->isIncrementOp()) ||
            (!IsLessOp && UO->isDecrementOp());
        if (!IsCompatibleWithTest && IsLessOp)
          StepValue = Actions.ActOnIntegerConstant(SourceLocation(), -1);
        else
          StepValue = Actions.ActOnIntegerConstant(SourceLocation(), 1);
      }
      return StepValue != 0;
    }
    bool VisitBinaryOperator(BinaryOperator *BO) {
      IsCompatibleWithTest = (IsLessOp && BO->getOpcode() == BO_AddAssign) ||
          (!IsLessOp && BO->getOpcode() == BO_SubAssign);
      switch (BO->getOpcode()) {
        case BO_AddAssign:
        case BO_SubAssign:
          if (VarChecker.Visit(BO->getLHS())) {
            StepValue = BO->getRHS();
            IsCompatibleWithTest = (IsLessOp && BO->getOpcode() == BO_AddAssign) ||
                (!IsLessOp && BO->getOpcode() == BO_SubAssign);
          }
          return StepValue != 0;
        case BO_Assign:
          if (VarChecker.Visit(BO->getLHS()) && ExprChecker.Visit(BO->getRHS())) {
            StepValue = ExprChecker.getStepValue();
            IsCompatibleWithTest = IsLessOp == ExprChecker.isIncrement();
          }
          return StepValue != 0;
        default:
          break;
      }
      return false;
    }
    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *E) {
      switch (E->getOperator()) {
        case OO_PlusPlus:
        case OO_MinusMinus:
          if (VarChecker.Visit(E->getArg(0))) {
            IsCompatibleWithTest = (IsLessOp && E->getOperator() == OO_PlusPlus) ||
                (!IsLessOp && E->getOperator() == OO_MinusMinus);
            if (!IsCompatibleWithTest && IsLessOp)
              StepValue = Actions.ActOnIntegerConstant(SourceLocation(), -1);
            else
              StepValue = Actions.ActOnIntegerConstant(SourceLocation(), 1);
          }
          return StepValue != 0;
        case OO_PlusEqual:
        case OO_MinusEqual:
          if (VarChecker.Visit(E->getArg(0))) {
            StepValue = E->getArg(1);
            IsCompatibleWithTest = (IsLessOp && E->getOperator() == OO_PlusEqual) ||
                (!IsLessOp && E->getOperator() == OO_MinusEqual);
          }
          return StepValue != 0;
        case OO_Equal:
          if (VarChecker.Visit(E->getArg(0)) && ExprChecker.Visit(E->getArg(1))) {
            StepValue = ExprChecker.getStepValue();
            IsCompatibleWithTest = IsLessOp == ExprChecker.isIncrement();
          }
          return StepValue != 0;
        default:
          break;
      }
      return false;
    }
    bool VisitStmt(Stmt *S) { return false; }
    ForIncrChecker(Decl *D, CodeGenFunction &S, bool LessOp)
      : VarChecker(D), ExprChecker(VarChecker), StepValue(0), Actions(S),
        IsLessOp(LessOp), IsCompatibleWithTest(false) {}
    Expr *getStepValue() { return StepValue; }
    bool isCompatibleWithTest() const { return IsCompatibleWithTest; }
  };
}

bool CodeGenFunction::isNotSupportedLoopForm(Stmt *S, OpenMPDirectiveKind Kind,
                                             Expr *&InitVal, Expr *&StepVal, Expr *&CheckVal, VarDecl *&VarCnt,
                                             Expr *&CheckOp, BinaryOperatorKind &OpKind) {
  // assert(S && "non-null statement must be specified");
  // OpenMP [2.9.5, Canonical Loop Form]
  //  for (init-expr; test-expr; incr-expr) structured-block
  OpKind = BO_Assign;
  ForStmt *For = dyn_cast_or_null<ForStmt>(S);
  if (!For) {
    //    Diag(S->getLocStart(), diag::err_omp_not_for)
    //        << getOpenMPDirectiveName(Kind);
    return true;
  }
  Stmt *Body = For->getBody();
  if (!Body) {
    //    Diag(S->getLocStart(), diag::err_omp_directive_nonblock)
    //        << getOpenMPDirectiveName(Kind);
    return true;
  }

  // OpenMP [2.9.5, Canonical Loop Form]
  //  init-expr One of the following:
  //  var = lb
  //  integer-type var = lb
  //  random-access-iterator-type var = lb
  //  pointer-type var = lb
  ForInitChecker InitChecker;
  Stmt *Init = For->getInit();
  VarDecl *Var;
  if (!Init || !(Var = dyn_cast_or_null<VarDecl>(InitChecker.Visit(Init)))) {
    //    Diag(Init ? Init->getLocStart() : For->getForLoc(),
    //         diag::err_omp_not_canonical_for)
    //        << 0;
    return true;
  }
  SourceLocation InitLoc = Init->getLocStart();

  // OpenMP [2.11.1.1, Data-sharing Attribute Rules for Variables Referenced
  // in a Construct, C/C++]
  // The loop iteration variable(s) in the associated for-loop(s) of a for or
  // parallel for construct may be listed in a private or lastprivate clause.
  bool HasErrors = false;

  // OpenMP [2.9.5, Canonical Loop Form]
  // Var One of the following
  // A variable of signed or unsigned integer type
  // For C++, a variable of a random access iterator type.
  // For C, a variable of a pointer type.
  QualType Type = Var->getType()
      .getNonReferenceType()
      .getCanonicalType()
      .getUnqualifiedType();
  if (!Type->isIntegerType() && !Type->isPointerType() &&
      (!getLangOpts().CPlusPlus || !Type->isOverloadableType())) {
    //    Diag(Init->getLocStart(), diag::err_omp_for_variable)
    //        << getLangOpts().CPlusPlus;
    HasErrors = true;
  }

  // OpenMP [2.9.5, Canonical Loop Form]
  //  test-expr One of the following:
  //  var relational-op b
  //  b relational-op var
  ForTestChecker TestChecker(Var);
  Stmt *Cond = For->getCond();
  CheckOp = cast<Expr>(Cond);
  bool TestCheckCorrect = false;
  if (!Cond || !(TestCheckCorrect = TestChecker.Visit(Cond))) {
    //    Diag(Cond ? Cond->getLocStart() : For->getForLoc(),
    //         diag::err_omp_not_canonical_for)
    //        << 1;
    HasErrors = true;
  }

  // OpenMP [2.9.5, Canonical Loop Form]
  //  incr-expr One of the following:
  //  ++var
  //  var++
  //  --var
  //  var--
  //  var += incr
  //  var -= incr
  //  var = var + incr
  //  var = incr + var
  //  var = var - incr
  ForIncrChecker IncrChecker(Var, *this, TestChecker.isLessOp());
  Stmt *Incr = For->getInc();
  bool IncrCheckCorrect = false;
  if (!Incr || !(IncrCheckCorrect = IncrChecker.Visit(Incr))) {
    //    Diag(Incr ? Incr->getLocStart() : For->getForLoc(),
    //         diag::err_omp_not_canonical_for)
    //        << 2;
    HasErrors = true;
  }

  // OpenMP [2.9.5, Canonical Loop Form]
  //  lb and b Loop invariant expressions of a type compatible with the type
  //  of var.
  Expr *InitValue = InitChecker.getInitValue();
  //  QualType InitTy =
  //    InitValue ? InitValue->getType().getNonReferenceType().
  //                                  getCanonicalType().getUnqualifiedType() :
  //                QualType();
  //  if (InitValue &&
  //      Context.mergeTypes(Type, InitTy, false, true).isNull()) {
  //    Diag(InitValue->getExprLoc(), diag::err_omp_for_type_not_compatible)
  //      << InitValue->getType()
  //      << Var << Var->getType();
  //    HasErrors = true;
  //  }
  Expr *CheckValue = TestChecker.getCheckValue();
  //  QualType CheckTy =
  //    CheckValue ? CheckValue->getType().getNonReferenceType().
  //                                  getCanonicalType().getUnqualifiedType() :
  //                 QualType();
  //  if (CheckValue &&
  //      Context.mergeTypes(Type, CheckTy, false, true).isNull()) {
  //    Diag(CheckValue->getExprLoc(), diag::err_omp_for_type_not_compatible)
  //      << CheckValue->getType()
  //      << Var << Var->getType();
  //    HasErrors = true;
  //  }

  // OpenMP [2.9.5, Canonical Loop Form]
  //  incr A loop invariant integer expression.
  Expr *Step = IncrChecker.getStepValue();
  if (Step && !Step->getType()->isIntegralOrEnumerationType()) {
    //    Diag(Step->getExprLoc(), diag::err_omp_for_incr_not_integer);
    HasErrors = true;
  }

  // OpenMP [2.9.5, Canonical Loop Form, Restrictions]
  //  If test-expr is of form var relational-op b and relational-op is < or
  //  <= then incr-expr must cause var to increase on each iteration of the
  //  loop. If test-expr is of form var relational-op b and relational-op is
  //  > or >= then incr-expr must cause var to decrease on each iteration of the
  //  loop.
  //  If test-expr is of form b relational-op var and relational-op is < or
  //  <= then incr-expr must cause var to decrease on each iteration of the
  //  loop. If test-expr is of form b relational-op var and relational-op is
  //  > or >= then incr-expr must cause var to increase on each iteration of the
  //  loop.
  if (Incr && TestCheckCorrect && IncrCheckCorrect &&
      !IncrChecker.isCompatibleWithTest()) {
    // Additional type checking.
    llvm::APSInt Result;
    bool IsConst = Step->isIntegerConstantExpr(Result, getContext());
    bool IsConstNeg = IsConst && Result.isSigned() && Result.isNegative();
    bool IsSigned = Step->getType()->hasSignedIntegerRepresentation();
    if ((TestChecker.isLessOp() && IsConst && IsConstNeg) ||
        (!TestChecker.isLessOp() &&
         ((IsConst && !IsConstNeg) || (!IsConst && !IsSigned)))) {
      //      Diag(Incr->getLocStart(), diag::err_omp_for_incr_not_compatible)
      //          << Var << TestChecker.isLessOp();
      HasErrors = true;
    } else {
      // TODO: Negative increment
      //Step = CreateBuiltinUnaryOp(Step->getExprLoc(), UO_Minus, Step);
    }
  }
  if (HasErrors)
    return true;

  assert(Step && "Null expr in Step in OMP FOR");
  Step = Step->IgnoreParenImpCasts();
  CheckValue = CheckValue->IgnoreParenImpCasts();
  InitValue = InitValue->IgnoreParenImpCasts();

  //  if (TestChecker.isStrictOp()) {
  //    Diff = BuildBinOp(DSAStack->getCurScope(), InitLoc, BO_Sub, CheckValue,
  //                      ActOnIntegerConstant(SourceLocation(), 1));
  //  }

  InitVal = InitValue;
  CheckVal = CheckValue;
  StepVal = Step;
  VarCnt = Var;

}


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

      const Expr *RefExpr;
      if(CurrArrayExpr != nullptr) {
        RefExpr = CurrArrayExpr;
      } else {
        RefExpr = D;
      }

      int MapType = CGM.OpenMPSupport.getMapType(VD);
      if (MapType == -1) {
        // FIXME: That should be detected before
        if (verbose) llvm::errs() << " --> assume input (not in clause)";
        CGM.OpenMPSupport.addOffloadingMapVariable(VD, OMP_TGT_MAPTYPE_TO);
        MapType = CGM.OpenMPSupport.getMapType(VD);
      }

      if(MapType == OMP_TGT_MAPTYPE_TO) {
        CGM.OpenMPSupport.getCurrentSparkMappingInfo()->InputVarUse[VD].push_back(D);
        CGM.OpenMPSupport.getCurrentSparkMappingInfo()->Inputs.insert(VD);
        CGM.OpenMPSupport.getCurrentSparkMappingInfo()->InputStyle[VD] = 1;
        if (verbose) llvm::errs() << " --> input";
      }
      else if (MapType == OMP_TGT_MAPTYPE_FROM) {
        CGM.OpenMPSupport.getCurrentSparkMappingInfo()->OutputVarDef[VD].push_back(D);
        if (verbose) llvm::errs() << " --> output";
      }
      else if (MapType == (OMP_TGT_MAPTYPE_TO | OMP_TGT_MAPTYPE_FROM)) {
        CGM.OpenMPSupport.getCurrentSparkMappingInfo()->InputOutputVarUse[VD].push_back(D);
        if (verbose) llvm::errs() << " --> both input/output";
      } else {
        if (verbose) llvm::errs() << " --> euuh something not supported";
        exit(1);
      }

      if(verbose) llvm::errs() << "\n";



      /*FIXME: experiment without input reordering*/
      /*
      if(CurrArrayExpr != nullptr && CurrArrayIndexExpr->IgnoreCasts()->isRValue() && MapType == OMP_TGT_MAPTYPE_FROM) {
        if(verbose) llvm::errs() << "Require reordering\n";
        CGM.OpenMPSupport.getReorderMap()[RefExpr] = CurrArrayIndexExpr->IgnoreCasts();
      }*/

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

    // FIXME: experiment without reordering
    TraverseStmt(A->getIdx());
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
    llvm::ConstantInt* const_int32_0 = llvm::ConstantInt::get(getLLVMContext(), llvm::APInt(32, llvm::StringRef("0"), 10));
    llvm::ConstantInt* const_int64_0 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(64, llvm::StringRef("0"), 10));
    llvm::ConstantInt* const_int32_4 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, llvm::StringRef("4"), 10));

    llvm::Constant *const_array_262 = llvm::ConstantDataArray::getString(mod->getContext(), "scala/Tuple2", true);
    llvm::Constant *const_array_263 = llvm::ConstantDataArray::getString(mod->getContext(), "<init>", true);
    llvm::Constant *const_array_264 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;)V", true);
    llvm::Constant *const_array_264_2 = llvm::ConstantDataArray::getString(mod->getContext(), "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V", true);

    llvm::ConstantPointerNull* const_ptr_256 = llvm::ConstantPointerNull::get(PointerTy_4);

    std::vector<llvm::Constant*> const_ptr_277_indices;
    const_ptr_277_indices.push_back(const_int64_0);
    const_ptr_277_indices.push_back(const_int64_0);

    // Init global variables
    gvar_array__str->setInitializer(const_array_262);
    gvar_array__str_1->setInitializer(const_array_263);
    gvar_array__str_2->setInitializer(const_array_264);
    gvar_array__str_22->setInitializer(const_array_264_2);

    // Allocate and load compulsry JNI arguments
    llvm::Function::arg_iterator args = RedFn->arg_begin();
    args->setName("env");
    llvm::AllocaInst* alloca_env = CGF.Builder.CreateAlloca(PointerTy_1);
    CGF.Builder.CreateStore(args, alloca_env);
    args++;
    args->setName("obj");
    llvm::AllocaInst* alloca_obj = CGF.Builder.CreateAlloca(PointerTy_jobject);
    CGF.Builder.CreateStore(args, alloca_obj);
    args++;

    llvm::LoadInst* ptr_env = CGF.Builder.CreateLoad(alloca_env, "");
    llvm::LoadInst* ptr_270 = CGF.Builder.CreateLoad(ptr_env, "");

    llvm::Value* ptr_271 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_270, 0, 184);
    llvm::LoadInst* ptr_272 = CGF.Builder.CreateLoad(ptr_271, "");
    llvm::LoadInst* ptr_273 = CGF.Builder.CreateLoad(alloca_env, "");

    // Allocate, load and cast the first operand
    llvm::AllocaInst* alloca_arg1 = CGF.Builder.CreateAlloca(PointerTy_jobject);
    CGF.Builder.CreateStore(args, alloca_arg1);

    llvm::LoadInst* ptr_274 = CGF.Builder.CreateLoad(alloca_arg1, "");
    std::vector<llvm::Value*> ptr_275_params;
    ptr_275_params.push_back(ptr_273);
    ptr_275_params.push_back(ptr_274);
    ptr_275_params.push_back(const_ptr_256);
    llvm::CallInst* ptr_275 = CGF.Builder.CreateCall(ptr_272, ptr_275_params);
    ptr_275->setCallingConv(llvm::CallingConv::C);
    ptr_275->setTailCall(false);

    llvm::Value* ptr_265 =  CGF.Builder.CreateBitCast(ptr_275, PointerTy_190);
    llvm::Value* ptr_265_3 = CGF.Builder.CreateLoad(ptr_265);
    llvm::Value* ptr_265_3_cast =  CGF.Builder.CreateBitCast(ptr_265_3, CGF.Builder.getInt32Ty());
    args++;

    // Allocate, load and cast the second operand
    llvm::LoadInst* ptr_env_2 = CGF.Builder.CreateLoad(alloca_env, "");
    llvm::LoadInst* ptr_270_2 = CGF.Builder.CreateLoad(ptr_env_2, "");

    llvm::Value* ptr_271_2 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_270_2, 0, 184);
    llvm::LoadInst* ptr_272_2 = CGF.Builder.CreateLoad(ptr_271_2, "");
    llvm::LoadInst* ptr_273_2 = CGF.Builder.CreateLoad(alloca_env, "");

    llvm::AllocaInst* alloca_arg2 = CGF.Builder.CreateAlloca(PointerTy_jobject);
    CGF.Builder.CreateStore(args, alloca_arg2);

    llvm::LoadInst* ptr_274_1 = CGF.Builder.CreateLoad(alloca_arg2, "");
    std::vector<llvm::Value*> ptr_275_1_params;
    ptr_275_1_params.push_back(ptr_273_2);
    ptr_275_1_params.push_back(ptr_274_1);
    ptr_275_1_params.push_back(const_ptr_256);
    llvm::CallInst* ptr_275_1 = CGF.Builder.CreateCall(ptr_272_2, ptr_275_1_params);
    ptr_275_1->setCallingConv(llvm::CallingConv::C);
    ptr_275_1->setTailCall(false);

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
        // TODO: What about min/max op ?
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
      llvm::Value* ptr_270 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_xx, 0, 192);
      llvm::LoadInst* ptr_271 = CGF.Builder.CreateLoad(ptr_270, "");

      std::vector<llvm::Value*> void_272_params;
      void_272_params.push_back(ptr_env);
      void_272_params.push_back(ptr_274);
      void_272_params.push_back(ptr_275);
      void_272_params.push_back(const_int32_0);
      llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_271, void_272_params);
      void_272->setCallingConv(llvm::CallingConv::C);
      void_272->setTailCall(true);
    }

    // Protect arg 2

    {
      llvm::LoadInst* ptr_xx = CGF.Builder.CreateLoad(ptr_env, "");
      llvm::Value* ptr_270 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_xx, 0, 192);
      llvm::LoadInst* ptr_271 = CGF.Builder.CreateLoad(ptr_270, "");

      std::vector<llvm::Value*> void_272_params;
      void_272_params.push_back(ptr_env);
      void_272_params.push_back(ptr_274_1);
      void_272_params.push_back(ptr_275_1);
      void_272_params.push_back(const_int32_0);
      llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_271, void_272_params);
      void_272->setCallingConv(llvm::CallingConv::C);
      void_272->setTailCall(true);
    }

    // Cast back the result to bit array
    llvm::LoadInst* ptr_27422 = CGF.Builder.CreateLoad(ptr_env, "");
    llvm::Value* ptr_275_2 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_27422, 0, 176);
    llvm::LoadInst* ptr_276 = CGF.Builder.CreateLoad(ptr_275_2, "");
    std::vector<llvm::Value*> ptr_277_params;
    ptr_277_params.push_back(ptr_env);
    ptr_277_params.push_back(const_int32_4); // FIXME: That should the size in byte of the element
    llvm::CallInst* ptr_277 = CGF.Builder.CreateCall(ptr_276, ptr_277_params);
    ptr_277->setCallingConv(llvm::CallingConv::C);
    ptr_277->setTailCall(true);

    llvm::LoadInst* ptr_278 = CGF.Builder.CreateLoad(ptr_env, "");
    llvm::Value* ptr_279 = CGF.Builder.CreateConstInBoundsGEP2_32(nullptr, ptr_278, 0, 208);
    llvm::LoadInst* ptr_280 = CGF.Builder.CreateLoad(ptr_279, "");
    llvm::Value* ptr_res_cast = CGF.Builder.CreateBitCast(alloca_res, PointerTy_4, "");
    std::vector<llvm::Value*> void_281_params;
    void_281_params.push_back(ptr_env);
    void_281_params.push_back(ptr_277);
    void_281_params.push_back(const_int32_0);
    void_281_params.push_back(const_int32_4); // FIXME: That should the size in byte of the element
    void_281_params.push_back(ptr_res_cast);
    llvm::CallInst* void_281 = CGF.Builder.CreateCall(ptr_280, void_281_params);
    void_281->setCallingConv(llvm::CallingConv::C);
    void_281->setTailCall(false);

    CGF.Builder.CreateRet(ptr_277);

  }
}


void CodeGenFunction::GenerateMappingKernel(const OMPExecutableDirective &S) {
  bool verbose = VERBOSE;

  const OMPParallelForDirective &ForDirective = cast<OMPParallelForDirective>(S);

  DefineJNITypes();

  auto& typeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();
  auto& indexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();

  auto& info = *CGM.OpenMPSupport.getCurrentSparkMappingInfo();

  if(verbose) llvm::errs() << "Offloaded variables \n";
  for(auto iter = typeMap.begin(); iter!= typeMap.end(); ++iter) {
    if(verbose) llvm::errs() << iter->first->getName() << " - " << iter->second << " - " << indexMap[iter->first] << "\n";
  }

  const Stmt *Body = S.getAssociatedStmt();
  Stmt *LoopStmt ;

  if (const CapturedStmt *CS = dyn_cast_or_null<CapturedStmt>(Body))
    Body = CS->getCapturedStmt();

  for (unsigned I = 0; I < ForDirective.getCollapsedNumber(); ++I) {
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

    LoopStmt = const_cast<Stmt*>(Body);

    // Detect info of the loop counter

    Expr *Step;
    Expr *Check;
    Expr *Init;
    VarDecl *VarCnt;
    Expr *CheckOp;
    BinaryOperatorKind OpKind;

    isNotSupportedLoopForm(LoopStmt, S.getDirectiveKind(), Init, Step, Check, VarCnt, CheckOp, OpKind);

    if(verbose) llvm::errs() << "Find counter " << VarCnt->getName() << "\n";

    auto& CntInfo = info.CounterInfo[VarCnt];
    CntInfo.push_back(Init);
    CntInfo.push_back(Check);
    CntInfo.push_back(Step);
    CntInfo.push_back(CheckOp);

    Body = For->getBody();
  }

  // Detect input/output expression from the loop body
  FindKernelArguments Finder(*this);
  Finder.TraverseStmt(LoopStmt);

  // Create the mapping function
  llvm::Module *mod = &(CGM.getModule());

  // Initialize a new CodeGenFunction used to generate the mapping
  CodeGenFunction CGF(CGM, true);

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

  for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it) {
    bool isCnt = info.CounterInfo.find(it->first) != info.CounterInfo.end();
    if(isCnt) {
      FuncTy_args.push_back(IntTy_jlong);
    } else {
      FuncTy_args.push_back(PointerTy_jobject);
    }
  }

  for(auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it) {
    bool isCnt = info.CounterInfo.find(it->first) != info.CounterInfo.end();
    if(isCnt) {
      FuncTy_args.push_back(IntTy_jlong);
    } else {
      FuncTy_args.push_back(PointerTy_jobject);
    }
  }

  // Size of the outputs
  for(auto it = info.OutputVarDef.begin(); it != info.OutputVarDef.end(); ++it) {
    FuncTy_args.push_back(IntTy_jint);
  }

  llvm::FunctionType* FnTy = llvm::FunctionType::get(
        /*Result=*/PointerTy_jobject,
        /*Params=*/FuncTy_args,
        /*isVarArg=*/false);


  std::string FnName = "Java_org_llvm_openmp_OmpKernel_mappingMethod" + std::to_string(info.Identifier);

  llvm::Function *MapFn =
      llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                             FnName, &CGM.getModule());

  CGF.CurFn = MapFn;
  CGF.EnsureInsertPoint();

  // Generate useful type and constant
  llvm::PointerType* PointerTy_Int8 = llvm::PointerType::get(CGF.Builder.getInt8Ty(), 0);

  llvm::ConstantInt* const_int32_0 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, llvm::StringRef("0"), 10));
  llvm::ConstantInt* const_int32_2 = llvm::ConstantInt::get(mod->getContext(), llvm::APInt(32, llvm::StringRef("2"), 10));

  llvm::ConstantPointerNull* const_ptr_null = llvm::ConstantPointerNull::get(PointerTy_Int8);

  // Global variable
  llvm::Value* const_ptr_init = CGF.Builder.CreateGlobalStringPtr("<init>", ".str.init");
  llvm::Value* const_ptr_tuple2 = CGF.Builder.CreateGlobalStringPtr("scala/Tuple2", ".str.tuple2");
  llvm::Value* const_ptr_tuple3 = CGF.Builder.CreateGlobalStringPtr("scala/Tuple3", ".str.tuple3");
  llvm::Value* const_ptr_tuple2_args = CGF.Builder.CreateGlobalStringPtr("(Ljava/lang/Object;Ljava/lang/Object;)V", ".str.tuple2.args");
  llvm::Value* const_ptr_tuple3_args = CGF.Builder.CreateGlobalStringPtr("(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)V", ".str.tuple3.args");

  // Allocate and load compulsory JNI arguments
  llvm::Function::arg_iterator args = MapFn->arg_begin();
  args->setName("env");
  llvm::AllocaInst* alloca_env = CGF.Builder.CreateAlloca(PointerTy_1);
  CGF.Builder.CreateStore(args, alloca_env);
  args++;
  args->setName("obj");
  llvm::AllocaInst* alloca_obj = CGF.Builder.CreateAlloca(PointerTy_jobject);
  CGF.Builder.CreateStore(args, alloca_obj);
  args++;

  llvm::LoadInst* ptr_env = CGF.Builder.CreateLoad(alloca_env, "");
  llvm::LoadInst* ptr_ptr_env = CGF.Builder.CreateLoad(ptr_env, "");

  // Load pointer to JNI functions
  llvm::Value* ptr_gep_getelement = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 184);
  llvm::LoadInst* ptr_fn_getelement = CGF.Builder.CreateLoad(ptr_gep_getelement, "");

  llvm::Value* ptr_gep_getcritical = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 222);
  llvm::LoadInst* ptr_fn_getcritical = CGF.Builder.CreateLoad(ptr_gep_getcritical, "");

  llvm::Value* ptr_gep_releaseelement = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 192);
  llvm::LoadInst* ptr_fn_releaseelement = CGF.Builder.CreateLoad(ptr_gep_releaseelement, "");

  llvm::Value* ptr_gep_releasecritical = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 223);
  llvm::LoadInst* ptr_fn_releasecritical = CGF.Builder.CreateLoad(ptr_gep_releasecritical, "");

  // Keep values that have to be used for releasing.
  llvm::SmallVector<llvm::Value*, 8> VecPtrBarrays;
  llvm::SmallVector<llvm::Value*, 8> VecPtrValues;
  llvm::SmallVector<llvm::Value*, 8> VecPtrBarraysElem;
  llvm::SmallVector<llvm::Value*, 8> VecPtrValuesElem;

  // Allocate, load and cast input variables (i.e. the arguments)
  for (auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it){
    const VarDecl *VD = it->first;

    bool isCnt = info.CounterInfo.find(VD) != info.CounterInfo.end();
    if(isCnt) {
      // FIXME: What about long ??
      llvm::AllocaInst* alloca_cast = CGF.Builder.CreateAlloca(IntTy_jint);
      llvm::Value* cast = CGF.Builder.CreateTruncOrBitCast(args, IntTy_jint);
      CGF.Builder.CreateStore(cast, alloca_cast);

      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, alloca_cast);
      }
    } else {

      QualType varType = VD->getType();
      llvm::Value *valuePtr;

      if(!varType->isAnyPointerType()) {

        // GetPrimitiveArrayCritical
        std::vector<llvm::Value*> ptr_load_arg_params;
        ptr_load_arg_params.push_back(ptr_env);
        ptr_load_arg_params.push_back(args);
        ptr_load_arg_params.push_back(const_ptr_null);
        llvm::CallInst* ptr_load_arg = CGF.Builder.CreateCall(ptr_fn_getelement, ptr_load_arg_params);
        ptr_load_arg->setCallingConv(llvm::CallingConv::C);
        ptr_load_arg->setTailCall(false);

        args->setName(VD->getName());

        VecPtrBarraysElem.push_back(args);
        VecPtrValuesElem.push_back(ptr_load_arg);


        llvm::Type *TyObject_arg = ConvertType(varType);

        llvm::PointerType* PointerTy_arg = llvm::PointerType::get(TyObject_arg, 0);
        valuePtr =  CGF.Builder.CreateBitCast(ptr_load_arg, PointerTy_arg);

      } else {
        // GetElement
        std::vector<llvm::Value*> ptr_load_arg_params;
        ptr_load_arg_params.push_back(ptr_env);
        ptr_load_arg_params.push_back(args);
        ptr_load_arg_params.push_back(const_ptr_null);
        llvm::CallInst* ptr_load_arg = CGF.Builder.CreateCall(ptr_fn_getcritical, ptr_load_arg_params);
        ptr_load_arg->setCallingConv(llvm::CallingConv::C);
        ptr_load_arg->setTailCall(false);

        args->setName(VD->getName());

        VecPtrBarrays.push_back(args);
        VecPtrValues.push_back(ptr_load_arg);

        llvm::Type *TyObject_arg = ConvertType(varType);

        llvm::Value* ptr_265 =  CGF.Builder.CreateBitCast(ptr_load_arg, TyObject_arg);

        valuePtr = CGF.Builder.CreateAlloca(TyObject_arg);
        CGF.Builder.CreateStore(ptr_265, valuePtr);
      }

      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, valuePtr);
      }
    }

    args++;
  }

  // Keep values that have to be used for releasing.
  llvm::SmallVector<llvm::Value*, 8> VecInOutBarrays;
  llvm::SmallVector<llvm::Value*, 8> VecInOutValues;

  // Allocate, load and cast input/output variables (i.e. the arguments)
  for (auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it){
    const VarDecl *VD = it->first;

    bool isCnt = info.CounterInfo.find(VD) != info.CounterInfo.end();
    if(isCnt) {
      // FIXME: What about long ??
      llvm::AllocaInst* alloca_cast = CGF.Builder.CreateAlloca(IntTy_jint);
      llvm::Value* cast = CGF.Builder.CreateTruncOrBitCast(args, IntTy_jint);
      CGF.Builder.CreateStore(cast, alloca_cast);

      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, alloca_cast);
      }
    } else {

      // GetByteArrayElements
      std::vector<llvm::Value*> ptr_load_arg_params;
      ptr_load_arg_params.push_back(ptr_env);
      ptr_load_arg_params.push_back(args);
      ptr_load_arg_params.push_back(const_ptr_null);
      llvm::CallInst* ptr_load_arg = CGF.Builder.CreateCall(ptr_fn_getcritical, ptr_load_arg_params);
      ptr_load_arg->setCallingConv(llvm::CallingConv::C);
      ptr_load_arg->setTailCall(false);

      args->setName(VD->getName());

      VecInOutBarrays.push_back(args);
      VecInOutValues.push_back(ptr_load_arg);

      QualType varType = VD->getType();
      llvm::Type *TyObject_arg = ConvertType(varType);

      llvm::Value *valuePtr;

      if(!varType->isAnyPointerType()) {
        if(verbose) llvm::errs() << ">Test< " << VD->getName() << " is scalar\n";

        llvm::PointerType* PointerTy_arg = llvm::PointerType::get(TyObject_arg, 0);
        valuePtr =  CGF.Builder.CreateBitCast(ptr_load_arg, PointerTy_arg);

      } else {
        llvm::Value* ptr_265 =  CGF.Builder.CreateBitCast(ptr_load_arg, TyObject_arg);

        valuePtr = CGF.Builder.CreateAlloca(TyObject_arg);
        CGF.Builder.CreateStore(ptr_265, valuePtr);
      }

      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, valuePtr);
      }
    }

    args++;
  }

  // Keep values that have to be used for releasing.
  llvm::SmallVector<llvm::Value*, 8> VecOutBarrays;
  llvm::SmallVector<llvm::Value*, 8> VecOutValues;

  // Allocate output variables
  for (auto it = info.OutputVarDef.begin(); it != info.OutputVarDef.end(); ++it) {
    const VarDecl *VD = it->first;

    QualType varType = VD->getType();
    llvm::Type *TyObject_res = ConvertType(varType);
    //llvm::AllocaInst *alloca_res = CGF.Builder.CreateAlloca(CGF.Builder.getInt8Ty(), args);

    // NewByteArray
    llvm::Value* ptr_275 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 176);
    llvm::LoadInst* ptr_276 = CGF.Builder.CreateLoad(ptr_275, "");
    std::vector<llvm::Value*> ptr_277_params;
    ptr_277_params.push_back(ptr_env);
    ptr_277_params.push_back(args);
    llvm::CallInst* ptr_277 = CGF.Builder.CreateCall(ptr_276, ptr_277_params);
    ptr_277->setCallingConv(llvm::CallingConv::C);
    ptr_277->setTailCall(true);

    // GetByteArrayElements
    std::vector<llvm::Value*> ptr_load_arg_params;
    ptr_load_arg_params.push_back(ptr_env);
    ptr_load_arg_params.push_back(ptr_277);
    ptr_load_arg_params.push_back(const_ptr_null);
    llvm::CallInst* alloca_res = CGF.Builder.CreateCall(ptr_fn_getelement, ptr_load_arg_params);
    alloca_res->setCallingConv(llvm::CallingConv::C);
    alloca_res->setTailCall(false);

    CGF.Builder.CreateMemSet(alloca_res, CGF.Builder.getInt8(0), args, 1);

    llvm::Value *cast_addr = CGF.Builder.CreateBitOrPointerCast(alloca_res, TyObject_res);
    llvm::AllocaInst *alloca_addr = CGF.Builder.CreateAlloca(TyObject_res);
    CGF.Builder.CreateStore(cast_addr, alloca_addr);

    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      CGM.OpenMPSupport.addOpenMPKernelArgVar(*it2, alloca_addr);
    }

    args->setName("sizeOf" + VD->getName());

    VecOutBarrays.push_back(ptr_277);
    VecOutValues.push_back(alloca_res);
    args++;
  }

  // Generate kernel code
  if (const CompoundStmt *S = dyn_cast<CompoundStmt>(Body))
    CGF.EmitCompoundStmtWithoutScope(*S);
  else
    CGF.EmitStmt(Body);

  auto ptrValue = VecPtrValues.begin();

  for (auto ptrBarray = VecPtrBarrays.begin(); ptrBarray != VecPtrBarrays.end(); ++ptrBarray){
    // ReleaseCritical
    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(*ptrBarray);
    void_272_params.push_back(*ptrValue);
    void_272_params.push_back(const_int32_2);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_fn_releasecritical, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);

    ptrValue++;
  }

  ptrValue = VecPtrValuesElem.begin();

  for (auto ptrBarray = VecPtrBarraysElem.begin(); ptrBarray != VecPtrBarraysElem.end(); ++ptrBarray){
    // ReleaseByteArrayElements
    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(*ptrBarray);
    void_272_params.push_back(*ptrValue);
    void_272_params.push_back(const_int32_2);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_fn_releaseelement, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);

    ptrValue++;
  }

  llvm::SmallVector<llvm::Value*, 8> results;

  auto InOutValues = VecInOutValues.begin();

  for (auto inOutBarrays = VecInOutBarrays.begin(); inOutBarrays != VecInOutBarrays.end(); ++inOutBarrays)
  {
    // ReleaseByteArrayElements
    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(*inOutBarrays);
    void_272_params.push_back(*InOutValues);
    void_272_params.push_back(const_int32_0);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_fn_releasecritical, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);

    results.push_back(*inOutBarrays);

    InOutValues++;
  }


  auto OutValues = VecOutValues.begin();

  for (auto outBarrays = VecOutBarrays.begin(); outBarrays != VecOutBarrays.end(); ++outBarrays)
  {
    // ReleaseByteArrayElements
    std::vector<llvm::Value*> void_272_params;
    void_272_params.push_back(ptr_env);
    void_272_params.push_back(*outBarrays);
    void_272_params.push_back(*OutValues);
    void_272_params.push_back(const_int32_0);
    llvm::CallInst* void_272 = CGF.Builder.CreateCall(ptr_fn_releaseelement, void_272_params);
    void_272->setCallingConv(llvm::CallingConv::C);
    void_272->setTailCall(true);

    results.push_back(*outBarrays);

    OutValues++;
  }

  unsigned NbOutputs = info.OutputVarDef.size() + info.InputOutputVarUse.size();

  if(NbOutputs == 1) {
    // Just return the value
    CGF.Builder.CreateRet(results.front());
  } else if (NbOutputs == 2) {
    // Construct and return a Tuple2

    llvm::Value* ptr_328 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 6);
    llvm::LoadInst* ptr_329 = CGF.Builder.CreateLoad(ptr_328, false);
    std::vector<llvm::Value*> ptr_330_params;
    ptr_330_params.push_back(ptr_env);
    ptr_330_params.push_back(const_ptr_tuple2);
    llvm::CallInst* ptr_330 = CGF.Builder.CreateCall(ptr_329, ptr_330_params);
    ptr_330->setCallingConv(llvm::CallingConv::C);
    ptr_330->setTailCall(false);

    llvm::Value* ptr_332 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 33);
    llvm::LoadInst* ptr_333 = CGF.Builder.CreateLoad(ptr_332, false);
    std::vector<llvm::Value*> ptr_334_params;
    ptr_334_params.push_back(ptr_env);
    ptr_334_params.push_back(ptr_330);
    ptr_334_params.push_back(const_ptr_init);
    ptr_334_params.push_back(const_ptr_tuple2_args);
    llvm::CallInst* ptr_334 = CGF.Builder.CreateCall(ptr_333, ptr_334_params);
    ptr_334->setCallingConv(llvm::CallingConv::C);
    ptr_334->setTailCall(false);

    llvm::Value* ptr_336 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 28);
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

    CGF.Builder.CreateRet(ptr_338);
  } else if (NbOutputs == 3) {
    // Construct and return a Tuple3

    llvm::Value* ptr_328 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 6);
    llvm::LoadInst* ptr_329 = CGF.Builder.CreateLoad(ptr_328, false);
    std::vector<llvm::Value*> ptr_330_params;
    ptr_330_params.push_back(ptr_env);
    ptr_330_params.push_back(const_ptr_tuple3);
    llvm::CallInst* ptr_330 = CGF.Builder.CreateCall(ptr_329, ptr_330_params);
    ptr_330->setCallingConv(llvm::CallingConv::C);
    ptr_330->setTailCall(false);

    llvm::Value* ptr_332 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 33);
    llvm::LoadInst* ptr_333 = CGF.Builder.CreateLoad(ptr_332, false);
    std::vector<llvm::Value*> ptr_334_params;
    ptr_334_params.push_back(ptr_env);
    ptr_334_params.push_back(ptr_330);
    ptr_334_params.push_back(const_ptr_init);
    ptr_334_params.push_back(const_ptr_tuple3_args);
    llvm::CallInst* ptr_334 = CGF.Builder.CreateCall(ptr_333, ptr_334_params);
    ptr_334->setCallingConv(llvm::CallingConv::C);
    ptr_334->setTailCall(false);

    llvm::Value* ptr_336 = CGF.Builder.CreateConstGEP2_32(nullptr, ptr_ptr_env, 0, 28);
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

    CGF.Builder.CreateRet(ptr_338);
  } else {
    // TODO: Construct and return Tuples in generic way
    if (verbose) llvm::errs() << "Need support for more than 3 outputs\n";
    exit(1);
  }
}


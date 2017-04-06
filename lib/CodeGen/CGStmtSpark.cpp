//===----------------- CGStmtSpark.cpp - Spark CodeGen --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Statements as Spark code.
//
//===----------------------------------------------------------------------===//

#include "CGDebugInfo.h"
#include "CGOpenCLRuntime.h"
#include "CGOpenMPRuntime.h"
#include "CGOpenMPRuntimeTypes.h"
#include "CodeGenFunction.h"
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
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/TypeBuilder.h"

#include "clang/AST/StmtVisitor.h"

#include "clang/AST/RecursiveASTVisitor.h"

#define VERBOSE 1

using namespace clang;
using namespace CodeGen;

void CodeGenFunction::EmitSparkJob() {
  std::error_code EC;

  // char *tmpName = strdup("_kernel_spark_XXXXXX");
  llvm::raw_fd_ostream SPARK_FILE("_kernel_spark.scala", EC,
                                  llvm::sys::fs::F_Text);
  if (EC) {
    llvm::errs() << "Couldn't open kernel_spark file for dumping.\nError:"
                 << EC.message() << "\n";
    exit(1);
  }

  // Header
  SPARK_FILE << "package org.llvm.openmp\n\n"
             << "import java.nio.ByteBuffer\n\n";

  EmitSparkNativeKernel(SPARK_FILE);

  // Core
  SPARK_FILE << "object OmpKernel {"
             << "\n";

  SPARK_FILE << "  def main(args: Array[String]) {\n"
             << "    var i = 0 // used for loop index\n"
             << "    \n"
             << "    val info = new CloudInfo(args)\n"
             << "    val fs = new CloudFileSystem(info.fs, args(3), args(4))\n"
             << "    val at = AddressTable.create(fs)\n"
             << "    info.init(fs)\n"
             << "    \n"
             << "    import info.sqlContext.implicits._\n"
             << "    \n";

  EmitSparkInput(SPARK_FILE);

  auto &mappingFunctions = CGM.OpenMPSupport.getSparkMappingFunctions();

  for (auto it = mappingFunctions.begin(); it != mappingFunctions.end(); it++) {
    EmitSparkMapping(SPARK_FILE, **it, (it + 1) == mappingFunctions.end());
  }

  EmitSparkOutput(SPARK_FILE);

  SPARK_FILE << "  }\n"
             << "\n"
             << "}\n";
}

void CodeGenFunction::EmitSparkNativeKernel(llvm::raw_fd_ostream &SPARK_FILE) {
  bool verbose = VERBOSE;

  auto &mappingFunctions = CGM.OpenMPSupport.getSparkMappingFunctions();
  auto &ReductionMap = CGM.OpenMPSupport.getReductionMap();

  int i;

  SPARK_FILE << "\n";
  SPARK_FILE << "import org.apache.spark.SparkFiles\n";
  SPARK_FILE << "class OmpKernel {\n";

  for (auto it = mappingFunctions.begin(); it != mappingFunctions.end(); it++) {
    CodeGenModule::OMPSparkMappingInfo &info = **it;

    unsigned NbOutputs = info.OutVarDef.size() + info.InOutVarUse.size();

    SPARK_FILE << "  @native def mappingMethod" << info.Identifier << "(";
    i = 0;
    for (auto it = info.CounterUse.begin(); it != info.CounterUse.end();
         ++it, i++) {
      // Separator
      if (it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ": Long, bound" << i << ": Long";
    }
    i = 0;
    for (auto it = info.InVarUse.begin(); it != info.InVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    SPARK_FILE << ") : ";
    if (NbOutputs == 1)
      SPARK_FILE << "Array[Byte]";
    else {
      SPARK_FILE << "Tuple" << NbOutputs << "[Array[Byte]";
      for (unsigned i = 1; i < NbOutputs; i++)
        SPARK_FILE << ", Array[Byte]";
      SPARK_FILE << "]";
    }
    SPARK_FILE << "\n";
    SPARK_FILE << "  def mapping" << info.Identifier << "(";
    i = 0;
    for (auto it = info.CounterUse.begin(); it != info.CounterUse.end();
         ++it, i++) {
      // Separator
      if (it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ": Long, bound" << i << ": Long";
    }
    i = 0;
    for (auto it = info.InVarUse.begin(); it != info.InVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    SPARK_FILE << ") : ";
    if (NbOutputs == 1)
      SPARK_FILE << "Array[Byte]";
    else {
      SPARK_FILE << "Tuple" << NbOutputs << "[Array[Byte]";
      for (unsigned i = 1; i < NbOutputs; i++)
        SPARK_FILE << ", Array[Byte]";
      SPARK_FILE << "]";
    }
    SPARK_FILE << " = {\n";
    SPARK_FILE << "    NativeKernels.loadOnce()\n";
    SPARK_FILE << "    return mappingMethod" << info.Identifier << "(";
    i = 0;
    for (auto it = info.CounterUse.begin(); it != info.CounterUse.end();
         ++it, i++) {
      // Separator
      if (it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ", bound" << i;
    }
    i = 0;
    for (auto it = info.InVarUse.begin(); it != info.InVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i;
    }
    for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i;
    }
    for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end();
         ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i;
    }
    SPARK_FILE << ")\n";
    SPARK_FILE << "  }\n\n";

    for (auto it = ReductionMap.begin(); it != ReductionMap.end(); ++it) {
      SPARK_FILE << "  @native def reduceMethod" << it->first->getName() << "_"
                 << info.Identifier
                 << "(n0 : Array[Byte], n1 : Array[Byte]) : Array[Byte]\n\n";
    }
  }
  SPARK_FILE << "}\n\n";
}

class SparkExprPrinter : public ConstStmtVisitor<SparkExprPrinter> {

  llvm::raw_fd_ostream &SPARK_FILE;
  ASTContext &Context;
  CodeGenModule::OMPSparkMappingInfo &Info;
  std::string CntStr;

public:
  SparkExprPrinter(llvm::raw_fd_ostream &SPARK_FILE, ASTContext &Context,
                   CodeGenModule::OMPSparkMappingInfo &Info, std::string CntStr)
      : SPARK_FILE(SPARK_FILE), Context(Context), Info(Info), CntStr(CntStr) {}

  void PrintExpr(const Expr *E) {
    if (E) {
      llvm::APSInt Value;
      bool isEvaluable = E->EvaluateAsInt(Value, Context);
      if (isEvaluable)
        SPARK_FILE << std::to_string(Value.getSExtValue());
      else
        Visit(E);
    } else
      SPARK_FILE << "<null expr>";
  }

  void VisitImplicitCastExpr(const ImplicitCastExpr *Node) {
    // No need to print anything, simply forward to the subexpression.
    PrintExpr(Node->getSubExpr());
  }

  void VisitParenExpr(const ParenExpr *Node) {
    SPARK_FILE << "(";
    PrintExpr(Node->getSubExpr());
    SPARK_FILE << ")";
  }

  void VisitBinaryOperator(const BinaryOperator *Node) {
    PrintExpr(Node->getLHS());
    SPARK_FILE << " " << BinaryOperator::getOpcodeStr(Node->getOpcode()) << " ";
    PrintExpr(Node->getRHS());
  }

  void VisitDeclRefExpr(const DeclRefExpr *Node) {
    const VarDecl *VD = dyn_cast<VarDecl>(Node->getDecl());
    if (Info.CounterInfo.find(VD) != Info.CounterInfo.end()) {
      SPARK_FILE << CntStr;
    } else {
      SPARK_FILE << "ByteBuffer.wrap(";
      SPARK_FILE << "__ompcloud_offload_" + VD->getName().str();
      // FIXME: How about long ?
      SPARK_FILE << ").order(java.nio.ByteOrder.LITTLE_ENDIAN).getInt";
    }
  }
};

std::string CodeGenFunction::getSparkVarName(const ValueDecl *VD) {
  return "__ompcloud_offload_" + VD->getName().str();
}

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {
  bool verbose = VERBOSE;
  auto &IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto &TypeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();

  SPARK_FILE << "    // Read each input from cloud-based filesystem\n";
  for (auto it = IndexMap.begin(); it != IndexMap.end(); ++it) {
    const ValueDecl *VD = it->first;
    int OffloadId = IndexMap[VD];
    unsigned OffloadType = TypeMap[VD];
    bool NeedBcast = VD->getType()->isAnyPointerType();

    // Find the bit size of one element
    QualType VarType = VD->getType();

    while (VarType->isAnyPointerType()) {
      VarType = VarType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(VarType) / 8;

    SPARK_FILE << "    val sizeOf_" << getSparkVarName(VD) << " = at.get("
               << OffloadId << ")\n";
    SPARK_FILE << "    val eltSizeOf_" << getSparkVarName(VD) << " = "
               << SizeInByte << "\n";

    if (OffloadType == OMP_TGT_MAPTYPE_TO ||
        OffloadType == (OMP_TGT_MAPTYPE_TO | OMP_TGT_MAPTYPE_FROM)) {

      SPARK_FILE << "    var " << getSparkVarName(VD) << " = fs.read("
                 << OffloadId << ", sizeOf_" << getSparkVarName(VD) << ")\n";

    } else if (OffloadType == OMP_TGT_MAPTYPE_FROM ||
               OffloadType == OMP_TGT_MAPTYPE_ALLOC) {
      SPARK_FILE << "    var " << getSparkVarName(VD)
                 << " = new Array[Byte](sizeOf_" << getSparkVarName(VD)
                 << ")\n";
    }

    if (verbose)
      SPARK_FILE << "    println(\"XXXX DEBUG XXXX SizeOf "
                 << getSparkVarName(VD) << "= \" + sizeOf_"
                 << getSparkVarName(VD) << ")\n";

    if (NeedBcast)
      SPARK_FILE << "    var " << getSparkVarName(VD)
                 << "_bcast = info.sc.broadcast(" << getSparkVarName(VD)
                 << ")\n";
  }

  SPARK_FILE << "    val _parallelism = info.getParallelism\n";

  SPARK_FILE << "\n";
}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE,
                                       CodeGenModule::OMPSparkMappingInfo &info,
                                       bool isLast) {
  bool verbose = VERBOSE;
  auto &IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto &TypeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();
  unsigned MappingId = info.Identifier;
  SparkExprPrinter MappingPrinter(SPARK_FILE, getContext(), info, "x.toInt");

  SPARK_FILE << "    // omp parallel for\n";

  SPARK_FILE << "    // 1 - Generate RDDs of index\n";
  int NbIndex = 0;

  for (auto it = info.CounterInfo.begin(); it != info.CounterInfo.end(); ++it) {
    const VarDecl *VarCnt = it->first;
    const Expr *Init = it->second[0];
    const Expr *Check = it->second[1];
    const Expr *Step = it->second[2];
    const Expr *CheckOp = it->second[3];

    const BinaryOperator *BO = cast<BinaryOperator>(CheckOp);

    SPARK_FILE << "    val bound_" << MappingId << "_" << NbIndex << " = ";
    MappingPrinter.PrintExpr(Check);
    SPARK_FILE << ".toLong\n";
    SPARK_FILE << "    val blockSize_" << MappingId << "_" << NbIndex
               << " = ((bound_" << MappingId << "_" << NbIndex
               << ").toFloat/_parallelism).floor.toLong\n";

    SPARK_FILE << "    val index_" << MappingId << "_" << NbIndex << " = (";
    MappingPrinter.PrintExpr(Init);
    SPARK_FILE << ".toLong to bound_" << MappingId << "_" << NbIndex;
    if (BO->getOpcode() == BO_LT || BO->getOpcode() == BO_GT) {
      SPARK_FILE << "-1";
    }
    SPARK_FILE << " by blockSize_" << MappingId << "_" << NbIndex << ")";
    SPARK_FILE << " // Index " << VarCnt->getName() << "\n";

    if (verbose) {
      SPARK_FILE << "    println(\"XXXX DEBUG XXXX blockSize = "
                    "\" + blockSize_"
                 << MappingId << "_" << NbIndex << ")\n";
      SPARK_FILE << "    println(\"XXXX DEBUG XXXX bound = \" + bound_"
                 << MappingId << "_" << NbIndex << ")\n";
    }
    NbIndex++;
  }

  // We need to explicitly create Tuple1 if there is no ranged input.
  int NumberOfRangedInput = 0;
  for (auto it = info.InVarUse.begin(); it != info.InVarUse.end(); ++it)
    if (const CEANIndexExpr *Range = info.RangedVar[it->first])
      NumberOfRangedInput++;
  for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end(); ++it)
    if (const CEANIndexExpr *Range = info.RangedVar[it->first])
      NumberOfRangedInput++;
  for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end(); ++it)
    if (const CEANIndexExpr *Range = info.RangedVar[it->first])
      NumberOfRangedInput++;

  SparkExprPrinter InputStartRangePrinter(SPARK_FILE, getContext(), info,
                                          "x.toInt");
  SparkExprPrinter InputEndRangePrinter(
      SPARK_FILE, getContext(), info,
      "x.toInt + blockSize_" + std::to_string(MappingId) + "_0.toInt");

  SPARK_FILE << "    val index_" << MappingId << " = index_" << MappingId
             << "_0";
  for (int i = 1; i < NbIndex; i++) {
    SPARK_FILE << ".cartesian(index_" << MappingId << "_" << i << ")";
  }
  SPARK_FILE << ".map{ x => ";

  if (NumberOfRangedInput == 0) {
    SPARK_FILE << "Tuple1(x)";
  } else {
    SPARK_FILE << "(x";
    for (auto it = info.InVarUse.begin(); it != info.InVarUse.end(); ++it) {
      const VarDecl *VD = it->first;
      if (const CEANIndexExpr *Range = info.RangedVar[VD]) {
        // Separator
        SPARK_FILE << ", ";
        SPARK_FILE << getSparkVarName(VD);
        SPARK_FILE << ".slice((";
        InputStartRangePrinter.PrintExpr(Range->getLowerBound());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", Math.min((";
        InputEndRangePrinter.PrintExpr(Range->getLength());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", sizeOf_"
                   << getSparkVarName(VD) << "))";
      }
    }
    for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end();
         ++it) {
      const VarDecl *VD = it->first;
      if (const CEANIndexExpr *Range = info.RangedVar[VD]) {
        // Separator
        SPARK_FILE << ", ";
        SPARK_FILE << getSparkVarName(VD);
        SPARK_FILE << ".slice((";
        InputStartRangePrinter.PrintExpr(Range->getLowerBound());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", Math.min((";
        InputEndRangePrinter.PrintExpr(Range->getLength());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", sizeOf_"
                   << getSparkVarName(VD) << "))";
      }
    }
    for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end(); ++it) {
      const VarDecl *VD = it->first;
      if (const CEANIndexExpr *Range = info.RangedVar[VD]) {
        // Separator
        SPARK_FILE << ", ";
        SPARK_FILE << getSparkVarName(VD);
        SPARK_FILE << ".slice((";
        InputStartRangePrinter.PrintExpr(Range->getLowerBound());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", Math.min((";
        InputEndRangePrinter.PrintExpr(Range->getLength());
        SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ", sizeOf_"
                   << getSparkVarName(VD) << "))";
      }
    }
    SPARK_FILE << ")";
  }
  SPARK_FILE << "}.toDS()\n"; // FIXME: Inverse with more indexes

  SPARK_FILE << "    // 2 - Perform Map operations\n";
  SPARK_FILE << "    val mapres_" << MappingId << " = index_" << MappingId
             << ".map{ x => (x._1, new OmpKernel().mapping" << MappingId << "(";

  // Assign each argument according to its type
  // x = (index, sliceOfInput1, sliceOfInput2, ...)
  int i = 1;
  NbIndex = 0;
  for (auto it = info.CounterUse.begin(); it != info.CounterUse.end(); ++it) {
    // Separator
    if (it != info.CounterUse.begin())
      SPARK_FILE << ", ";
    SPARK_FILE << "x._" << i << ", Math.min(x._" << i << "+blockSize_"
               << MappingId << "_" << NbIndex << "-1, bound_" << MappingId
               << "_" << NbIndex << "-1)";
    i++;
  }

  for (auto it = info.InVarUse.begin(); it != info.InVarUse.end(); ++it) {
    const VarDecl *VD = it->first;
    bool NeedBcast = VD->getType()->isAnyPointerType();
    // Separator
    SPARK_FILE << ", ";
    if (const CEANIndexExpr *Range = info.RangedVar[VD])
      SPARK_FILE << "x._" << i++;
    else if (NeedBcast)
      SPARK_FILE << getSparkVarName(VD) << "_bcast.value";
    else
      SPARK_FILE << getSparkVarName(VD) << ".clone";
  }
  for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end(); ++it) {
    const VarDecl *VD = it->first;
    bool NeedBcast = VD->getType()->isAnyPointerType();
    // Separator
    SPARK_FILE << ", ";
    if (const CEANIndexExpr *Range = info.RangedVar[VD])
      SPARK_FILE << "x._" << i++;
    else if (NeedBcast)
      // FIXME: Additional copy but avoid error when using multiple thread on
      // the same worker node
      SPARK_FILE << getSparkVarName(VD) << "_bcast.value.clone";
    else
      SPARK_FILE << getSparkVarName(VD) << ".clone";
  }
  for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end(); ++it) {
    const VarDecl *VD = it->first;
    bool NeedBcast = VD->getType()->isAnyPointerType();
    // Separator
    SPARK_FILE << ", ";
    if (const CEANIndexExpr *Range = info.RangedVar[VD])
      SPARK_FILE << "x._" << i++;
    else if (NeedBcast)
      // FIXME: Additional copy but avoid error when using multiple thread on
      // the same worker node
      SPARK_FILE << getSparkVarName(VD) << "_bcast.value.clone";
    else
      SPARK_FILE << getSparkVarName(VD) << ".clone";
  }

  SPARK_FILE << ")) }\n";

  unsigned NbOutputs = info.OutVarDef.size() + info.InOutVarUse.size();
  if (NbOutputs > 1) {
    SPARK_FILE << "    // cache not to perform the mapping for each output\n";
    SPARK_FILE << "    mapres_" << MappingId << ".cache\n";
  }

  SPARK_FILE << "    // 3 - Merge back the results\n";

  i = 0;

  for (auto it = info.OutVarDef.begin(); it != info.OutVarDef.end(); ++it) {
    const VarDecl *VD = it->first;
    bool NeedBcast = VD->getType()->isAnyPointerType();
    const CEANIndexExpr *Range = info.RangedVar[VD];

    SPARK_FILE << "    ";
    if (Range)
      SPARK_FILE << "val " << getSparkVarName(VD) << "_tmp_" << MappingId;
    else
      SPARK_FILE << getSparkVarName(VD);
    SPARK_FILE << " = ";

    SPARK_FILE << "mapres_" << MappingId;

    if (NbOutputs == 1) {
      // 1 output -> return the result directly
    } else if (NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << ".map{ x => (x._1, x._2._" << i + 1 << ") }";
    } else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << ".map{ x => (x._1, x._2(" << i << ")) }";
    }
    if (CGM.OpenMPSupport.isReduced(VD))
      SPARK_FILE << ".map{ x => x._2 }.reduce{(x, y) => new "
                    "OmpKernel().reduceMethod"
                 << VD->getName() << "(x, y)}";
    else if (Range)
      SPARK_FILE << ".collect()";
    else
      SPARK_FILE << ".map{ x => x._2 "
                    "}.repartition(info.getExecutorNumber.toInt).reduce{(x, y) "
                    "=> Util.bitor(x, y)}";
    SPARK_FILE << "\n";

    if (Range) {
      SparkExprPrinter RangePrinter(SPARK_FILE, getContext(), info,
                                    getSparkVarName(VD) + std::string("_tmp_") +
                                        std::to_string(MappingId) +
                                        std::string("(i)._1.toInt"));

      SPARK_FILE << "    " << getSparkVarName(VD)
                 << " = new Array[Byte](sizeOf_" << getSparkVarName(VD)
                 << ")\n";
      SPARK_FILE << "    i = 0\n";
      SPARK_FILE << "    while (i < " << getSparkVarName(VD) << "_tmp_"
                 << MappingId << ".length) {\n";
      SPARK_FILE << "      " << getSparkVarName(VD) << "_tmp_" << MappingId
                 << "(i)._2.copyToArray(" << getSparkVarName(VD) << ", (";
      RangePrinter.PrintExpr(Range->getLowerBound());
      SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ")\n"
                 << "      i += 1\n"
                 << "    }\n";
    }

    if (NeedBcast && !isLast)
      SPARK_FILE << "    " << getSparkVarName(VD) << "_bcast.destroy\n"
                 << "    " << getSparkVarName(VD)
                 << "_bcast = info.sc.broadcast(" << getSparkVarName(VD)
                 << ")\n";

    i++;
  }

  for (auto it = info.InOutVarUse.begin(); it != info.InOutVarUse.end(); ++it) {
    const VarDecl *VD = it->first;
    bool NeedBcast = VD->getType()->isAnyPointerType();
    const CEANIndexExpr *Range = info.RangedVar[VD];
    unsigned OffloadType = TypeMap[VD];

    if ((OffloadType == OMP_TGT_MAPTYPE_ALLOC) && isLast)
      continue;

    SPARK_FILE << "    ";
    if (Range)
      SPARK_FILE << "val " << getSparkVarName(VD) << "_tmp_" << MappingId;
    else
      SPARK_FILE << getSparkVarName(VD);
    SPARK_FILE << " = ";

    SPARK_FILE << "mapres_" << MappingId;

    if (NbOutputs == 1) {
      // 1 output -> return the result directly
    } else if (NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << ".map{ x => (x._1, x._2._" << i + 1 << ") }";
    } else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << ".map{ x => (x._1, x._2(" << i << ")) }";
    }
    if (CGM.OpenMPSupport.isReduced(VD))
      SPARK_FILE << ".map{ x => x._2 }.reduce{(x, y) => new "
                    "OmpKernel().reduceMethod"
                 << VD->getName() << "(x, y)}";
    if (Range)
      SPARK_FILE << ".collect()";
    else
      SPARK_FILE << ".map{ x => x._2 "
                    "}.repartition(info.getExecutorNumber.toInt).reduce{(x, y) "
                    "=> Util.bitor(x, y)}";
    SPARK_FILE << "\n";

    if (Range) {
      SparkExprPrinter RangePrinter(SPARK_FILE, getContext(), info,
                                    getSparkVarName(VD) + std::string("_tmp_") +
                                        std::to_string(MappingId) +
                                        std::string("(i)._1.toInt"));

      SPARK_FILE << "    " << getSparkVarName(VD)
                 << " = new Array[Byte](sizeOf_" << getSparkVarName(VD)
                 << ")\n";
      SPARK_FILE << "    i = 0\n";
      SPARK_FILE << "    while (i < " << getSparkVarName(VD) << "_tmp_"
                 << MappingId << ".length) {\n";
      SPARK_FILE << "      " << getSparkVarName(VD) << "_tmp_" << MappingId
                 << "(i)._2.copyToArray(" << getSparkVarName(VD) << ", (";
      RangePrinter.PrintExpr(Range->getLowerBound());
      SPARK_FILE << ") * eltSizeOf_" << getSparkVarName(VD) << ")\n"
                 << "      i += 1\n"
                 << "    }\n";
    }

    if (NeedBcast && !isLast)
      SPARK_FILE << "    " << getSparkVarName(VD) << "_bcast.destroy\n"
                 << "    " << getSparkVarName(VD)
                 << "_bcast = info.sc.broadcast(" << getSparkVarName(VD)
                 << ")\n";

    i++;
  }
  SPARK_FILE << "\n";
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto &IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto &TypeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";

  for (auto it = IndexMap.begin(); it != IndexMap.end(); ++it) {
    const ValueDecl *VD = it->first;
    int OffloadId = IndexMap[VD];
    unsigned OffloadType = TypeMap[VD];

    if (OffloadType == OMP_TGT_MAPTYPE_FROM ||
        OffloadType == (OMP_TGT_MAPTYPE_TO | OMP_TGT_MAPTYPE_FROM)) {
      SPARK_FILE << "    fs.write(" << OffloadId << ", sizeOf_"
                 << getSparkVarName(VD) << ", " << getSparkVarName(VD) << ")\n";
    }
  }
}

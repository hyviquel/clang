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

#define VERBOSE 0

using namespace clang;
using namespace CodeGen;


void CodeGenFunction::EmitSparkJob() {
  std::error_code EC;

  //char *tmpName = strdup("_kernel_spark_XXXXXX");
  llvm::raw_fd_ostream SPARK_FILE("_kernel_spark.scala", EC, llvm::sys::fs::F_Text);
  if (EC) {
    llvm::errs() << "Couldn't open kernel_spark file for dumping.\nError:" << EC.message() << "\n";
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
             << "    \n"
             << "    val info = new CloudInfo(args)\n"
             << "    val fs = new CloudFileSystem(info.fs, args(3))\n"
             << "    val at = AddressTable.create(fs)\n"
             << "    info.init(fs)\n"
             << "    \n"
             << "    import info.sqlContext.implicits._\n"
             << "    \n";

  EmitSparkInput(SPARK_FILE);

  auto& mappingFunctions = CGM.OpenMPSupport.getSparkMappingFunctions();

  for(auto it = mappingFunctions.begin(); it != mappingFunctions.end(); it++) {
    EmitSparkMapping(SPARK_FILE, **it);
  }

  EmitSparkOutput(SPARK_FILE);

  SPARK_FILE << "  }\n"
             << "\n"
             << "}\n";

}

void CodeGenFunction::EmitSparkNativeKernel(llvm::raw_fd_ostream &SPARK_FILE) {
  bool verbose = VERBOSE;

  auto& mappingFunctions = CGM.OpenMPSupport.getSparkMappingFunctions();
  auto& ReductionMap = CGM.OpenMPSupport.getReductionMap();

  int i;

  SPARK_FILE << "\n";
  SPARK_FILE << "import org.apache.spark.SparkFiles\n";
  SPARK_FILE << "class OmpKernel {\n";

  for(auto it = mappingFunctions.begin(); it != mappingFunctions.end(); it++) {
    auto& info = **it;

    unsigned NbInputs = info.InputVarUse.size() + info.InputOutputVarUse.size();
    unsigned NbOutputs = info.OutputVarDef.size() + info.InputOutputVarUse.size();
    unsigned NbOutputSize = info.OutputVarDef.size();

    if(verbose) llvm::errs() << "NbInput => " << NbInputs << "\n";
    if(verbose) llvm::errs() << "NbOutput => " << NbOutputs << "\n";

    SPARK_FILE << "  @native def mappingMethod" << info.Identifier << "(";
    i=0;
    for(auto it = info.CounterUse.begin(); it != info.CounterUse.end(); ++it, i++) {
      // Separator
      if(it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ": Long, bound" << i << ": Long";
    }
    i=0;
    for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for(auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for(unsigned j = 0; j<NbOutputSize; j++, i++)
      SPARK_FILE << ", n" << i << ": Int";
    SPARK_FILE << ") : ";
    if (NbOutputs == 1)
      SPARK_FILE << "Array[Byte]";
    else {
      SPARK_FILE << "Tuple" << NbOutputs << "[Array[Byte]";
      for(unsigned i = 1; i<NbOutputs; i++)
        SPARK_FILE << ", Array[Byte]";
      SPARK_FILE << "]";
    }
    SPARK_FILE << "\n";
    SPARK_FILE << "  def mapping" << info.Identifier << "(";
    i=0;
    for(auto it = info.CounterUse.begin(); it != info.CounterUse.end(); ++it, i++) {
      // Separator
      if(it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ": Long, bound" << i << ": Long";
    }
    i=0;
    for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for(auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
    for(unsigned j = 0; j<NbOutputSize; j++, i++)
      SPARK_FILE << ", n" << i << ": Int";
    SPARK_FILE << ") : ";
    if (NbOutputs == 1)
      SPARK_FILE << "Array[Byte]";
    else {
      SPARK_FILE << "Tuple" << NbOutputs << "[Array[Byte]";
      for(unsigned i = 1; i<NbOutputs; i++)
        SPARK_FILE << ", Array[Byte]";
      SPARK_FILE << "]";
    }
    SPARK_FILE << " = {\n";
    SPARK_FILE << "    NativeKernels.loadOnce()\n";
    SPARK_FILE << "    return mappingMethod" << info.Identifier << "(";
    i=0;
    for(auto it = info.CounterUse.begin(); it != info.CounterUse.end(); ++it, i++) {
      // Separator
      if(it != info.CounterUse.begin())
        SPARK_FILE << ", ";

      SPARK_FILE << "index" << i << ", bound" << i;
    }
    i=0;
    for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i;
    }
    for(auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it, i++) {
      // Separator
      SPARK_FILE << ", ";
      SPARK_FILE << "n" << i;
    }
    for(unsigned j = 0; j<NbOutputSize; j++, i++)
      SPARK_FILE << ", n" << i;
    SPARK_FILE << ")\n";
    SPARK_FILE << "  }\n\n";

    for(auto it = ReductionMap.begin(); it != ReductionMap.end(); ++it) {
      SPARK_FILE << "  @native def reduceMethod"<< it->first->getName() << "_" << info.Identifier << "(n0 : Array[Byte], n1 : Array[Byte]) : Array[Byte]\n\n" ;
    }

  }
  SPARK_FILE << "}\n\n";
}


std::string CodeGenFunction::getSparkExprOf(const Expr *ExprValue) {
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  std::string SparkExpr = "";
  llvm::APSInt Value;

  bool isEvaluable = ExprValue->EvaluateAsInt(Value, getContext());
  if(isEvaluable) {
    SparkExpr += std::to_string(Value.getSExtValue());

  } else if (const DeclRefExpr *D = dyn_cast<DeclRefExpr>(ExprValue)) {
    const VarDecl *VD = dyn_cast<VarDecl>(D->getDecl());
    int id = IndexMap[VD];
    SparkExpr += "ByteBuffer.wrap(";
    SparkExpr += VD->getName();
    // FIXME: How about long ?
    SparkExpr += ").order(java.nio.ByteOrder.LITTLE_ENDIAN).getInt";
  } else {
    llvm::errs()  << "Cannot fully detect its scope statically:\n"
                  << "Require the generation of native kernels to compute it during the execution.\n";
    exit(1);
  }

  return SparkExpr;
}

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& TypeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();

  SPARK_FILE << "    // Read each input from cloud-based filesystem\n";
  for (auto it = IndexMap.begin(); it != IndexMap.end(); ++it)
  {
    const ValueDecl *VD = it->first;
    int OffloadId = IndexMap[VD];
    unsigned OffloadType = TypeMap[VD];


    // Find the bit size of one element
    QualType VarType = VD->getType();

    while(VarType->isAnyPointerType()) {
      VarType = VarType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(VarType) / 8;

    if(OffloadType == OMP_TGT_MAPTYPE_TO
       || OffloadType == (OMP_TGT_MAPTYPE_TO | OMP_TGT_MAPTYPE_FROM)) {

      SPARK_FILE << "    var " << VD->getName() << " = ";
      if(VD->getType()->isAnyPointerType())
        SPARK_FILE << "info.sc.broadcast(";
      SPARK_FILE << "fs.read(" << OffloadId << ", " << SizeInByte << ")";
      if(VD->getType()->isAnyPointerType())
        SPARK_FILE << ")";
      SPARK_FILE << "\n";

    } else if (OffloadType == OMP_TGT_MAPTYPE_FROM) {
      SPARK_FILE << "    var " << VD->getName() << " = ";
      if(VD->getType()->isAnyPointerType())
        SPARK_FILE << "info.sc.broadcast(";
      SPARK_FILE << "new Array[Byte](0)";
      if(VD->getType()->isAnyPointerType())
        SPARK_FILE << ")";
      SPARK_FILE << "\n";
      SPARK_FILE << "    val sizeOf_" << VD->getName() << " = at.get(" << OffloadId << ")\n";

    }

  }

  SPARK_FILE << "\n";
}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE, CodeGenModule::OMPSparkMappingInfo &info) {
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  unsigned MappingId = info.Identifier;

  unsigned NbInputs = 0;
  for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it)
    NbInputs += it->second.size();

  SPARK_FILE << "    // omp parallel for\n";

  SPARK_FILE << "    // 1 - Generate RDDs of index\n";
  int NbIndex = 0;

  for (auto it = info.CounterInfo.begin(); it != info.CounterInfo.end(); ++it)
  {
    const VarDecl *VarCnt = it->first;
    const Expr *Init = it->second[0];
    const Expr *Check = it->second[1];
    const Expr *Step = it->second[2];
    const Expr *CheckOp = it->second[3];

    const BinaryOperator *BO = cast<BinaryOperator>(CheckOp);

    SPARK_FILE << "    val bound_" << MappingId << "_" << NbIndex << " = " << getSparkExprOf(Check) << ".toLong\n";
    SPARK_FILE << "    val blockSize_" << MappingId << "_" << NbIndex << " = ((bound_" << MappingId << "_" << NbIndex << ").toFloat/info.getParallelism).floor.toLong\n";

    SPARK_FILE << "    val index_" << MappingId << "_" << NbIndex << " = (" << getSparkExprOf(Init) << ".toLong to bound_" << MappingId << "_" << NbIndex;
    if(BO->getOpcode() == BO_LT || BO->getOpcode() == BO_GT) {
      SPARK_FILE << "-1";
    }
    SPARK_FILE << " by blockSize_" << MappingId << "_" << NbIndex << ").toDS()";
    SPARK_FILE << " // Index " << VarCnt->getName() << "\n";
    NbIndex++;
  }

  SPARK_FILE << "    val index_" << MappingId << " = index_" << MappingId << "_0";
  for(int i=1; i<NbIndex; i++) {
    SPARK_FILE << ".cartesian(index_" << MappingId << "_" << i << ")";
  }
  SPARK_FILE << "\n"; // FIXME: Inverse with more indexes


  SPARK_FILE << "    // 2 - Perform Map operations\n";
  SPARK_FILE << "    val mapres_" << MappingId << " = index_" << MappingId << ".map{ x => new OmpKernel().mapping" << MappingId << "(";

  // Assign each argument according to its type
  int i=1;
  NbIndex = 0;
  for(auto it = info.CounterUse.begin(); it != info.CounterUse.end(); ++it, i++) {
    // Separator
    if(it != info.CounterUse.begin())
      SPARK_FILE << ", ";
    if(info.CounterInfo.size() == 1) {
      SPARK_FILE << "x, Math.min(x+blockSize_" << MappingId << "_" << NbIndex << "-1, bound_" << MappingId << "_" << NbIndex << "-1)";
    } else {
      SPARK_FILE << "x._" << i << ", Math.min(x._" << i << "+blockSize_" << MappingId << "_" << NbIndex << "-1, bound_" << MappingId << "_" << NbIndex << "-1)";
      i++;
    }
    NbIndex++;
  }
  for(auto it = info.InputVarUse.begin(); it != info.InputVarUse.end(); ++it)
  {
    const VarDecl *VD = it->first;
    // Separator
    SPARK_FILE << ", ";
    SPARK_FILE << VD->getName();
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << ".value";
  }
  for(auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it)
  {
    const VarDecl *VD = it->first;
    // Separator
    SPARK_FILE << ", ";
    SPARK_FILE << VD->getName();
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << ".value";
  }
  for(auto it = info.OutputVarDef.begin(); it != info.OutputVarDef.end(); ++it)
  {
    const VarDecl *VD = it->first;
    int id = IndexMap[VD];
    SPARK_FILE << ", sizeOf_" << VD->getName();
  }

  SPARK_FILE << ") }\n";

  SPARK_FILE << "    // 3 - Merge back the results\n";
  SPARK_FILE << "    val mapres2_" << MappingId << " = mapres_" << MappingId << ".repartition(info.getExecutorNumber)\n";

  i=0;
  unsigned NbOutputs = info.OutputVarDef.size() + info.InputOutputVarUse.size();

  for (auto it = info.OutputVarDef.begin(); it != info.OutputVarDef.end(); ++it)
  {
    const VarDecl *VD = it->first;

    SPARK_FILE << "    " << VD->getName() << " = ";
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << "info.sc.broadcast(";
    if(NbOutputs == 1) {
      // 1 output -> return the result directly
      SPARK_FILE << "mapres2_" << MappingId;
    }
    else if(NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << "mapres2_" << MappingId << ".map{_._" << i+1 << "}";
    }
    else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << "mapres2_" << MappingId << ".map{ x => x(" << i << ") }";
    }
    if(CGM.OpenMPSupport.isReduced(VD))
      SPARK_FILE << ".reduce{(x, y) => new OmpKernel().reduceMethod"<< VD->getName() << "(x, y)}";
    else
      SPARK_FILE << ".reduce{(x, y) => Util.bitor(x, y)}";
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << ")";
    SPARK_FILE << "\n";

    i++;
  }

  for (auto it = info.InputOutputVarUse.begin(); it != info.InputOutputVarUse.end(); ++it)
  {
    const VarDecl *VD = it->first;

    SPARK_FILE << "    " << VD->getName() << " = ";
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << "info.sc.broadcast(";
    if(NbOutputs == 1) {
      // 1 output -> return the result directly
      SPARK_FILE << "mapres2_" << MappingId;
    }
    else if(NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << "mapres2_" << MappingId << ".map{_._" << i+1 << "}";
    }
    else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << "mapres2_" << MappingId << ".map{ x => x(" << i << ") }";
    }
    if(CGM.OpenMPSupport.isReduced(VD))
      SPARK_FILE << ".reduce{(x, y) => new OmpKernel().reduceMethod"<< VD->getName() << "(x, y)}";
    else
      SPARK_FILE << ".reduce{(x, y) => Util.bitor(x, y)}";
    if(VD->getType()->isAnyPointerType())
      SPARK_FILE << ")";
    SPARK_FILE << "\n";

    i++;
  }
  SPARK_FILE << "\n";
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& TypeMap = CGM.OpenMPSupport.getLastOffloadingMapVarsType();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";

  for (auto it = IndexMap.begin(); it != IndexMap.end(); ++it)
  {
    const ValueDecl *VD = it->first;
    int OffloadId = IndexMap[VD];
    unsigned OffloadType = TypeMap[VD];

    if (OffloadType == OMP_TGT_MAPTYPE_FROM
        || OffloadType == (OMP_TGT_MAPTYPE_TO | OMP_TGT_MAPTYPE_FROM)) {
      SPARK_FILE << "    fs.write(" << OffloadId << ", " << VD->getName();
      if(it->first->getType()->isAnyPointerType())
        SPARK_FILE << ".value";
      SPARK_FILE << ")\n";
    }
  }
}

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
             << "    val fs = CloudFileSystem.create(args(0), args(1), args(2), args(3))\n"
             << "    val at = AddressTable.create(fs)\n"
             << "    val info = new CloudInfo(fs, at)\n"
             << "    \n";

  EmitSparkInput(SPARK_FILE);

  EmitSparkMapping(SPARK_FILE);

  EmitSparkOutput(SPARK_FILE);

  SPARK_FILE << "  }\n"
             << "\n"
             << "}\n";

}

void CodeGenFunction::EmitSparkNativeKernel(llvm::raw_fd_ostream &SPARK_FILE) {
  bool verbose = VERBOSE;

  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& InputOutputVarUse = CGM.OpenMPSupport.getOffloadingInputOutputVarUse();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& reductionMap = CGM.OpenMPSupport.getReductionMap();
  auto& ReorderInputVarUse = CGM.OpenMPSupport.getReorderInputVarUse();
  auto& CntMap = CGM.OpenMPSupport.getOffloadingCounterInfo();

  int i;

  unsigned NbInputs = InputVarUse.size() + InputOutputVarUse.size();
  unsigned NbOutputs = OutputVarDef.size() + InputOutputVarUse.size();
  unsigned NbOutputSize = OutputVarDef.size();

  if(verbose) llvm::errs() << "NbInput => " << NbInputs << "\n";
  if(verbose) llvm::errs() << "NbOutput => " << NbOutputs << "\n";
  SPARK_FILE << "\n";
  SPARK_FILE << "import org.apache.spark.SparkFiles\n";
  SPARK_FILE << "class OmpKernel {\n";
  SPARK_FILE << "  @native def mappingMethod(";
  i=0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it, i++) {
    // Separator
    if(it != InputVarUse.begin())
      SPARK_FILE << ", ";

    bool isCnt = CntMap.find(it->first) != CntMap.end();
    if(isCnt) {
      SPARK_FILE << "n" << i << ": Long";
    } else {
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
  }
  for(auto it = InputOutputVarUse.begin(); it != InputOutputVarUse.end(); ++it, i++) {
    // Separator
    if(!InputVarUse.empty() || it != InputOutputVarUse.begin())
      SPARK_FILE << ", ";

    bool isCnt = CntMap.find(it->first) != CntMap.end();
    if(isCnt) {
      SPARK_FILE << "n" << i << ": Long";
    } else {
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
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
  SPARK_FILE << "  def mappingWrapper(";
  i=0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it, i++) {
    // Separator
    if(it != InputVarUse.begin())
      SPARK_FILE << ", ";

    bool isCnt = CntMap.find(it->first) != CntMap.end();
    if(isCnt) {
      SPARK_FILE << "n" << i << ": Long";
    } else {
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
  }
  for(auto it = InputOutputVarUse.begin(); it != InputOutputVarUse.end(); ++it, i++) {
    // Separator
    if(!InputVarUse.empty() || it != InputOutputVarUse.begin())
      SPARK_FILE << ", ";

    bool isCnt = CntMap.find(it->first) != CntMap.end();
    if(isCnt) {
      SPARK_FILE << "n" << i << ": Long";
    } else {
      SPARK_FILE << "n" << i << ": Array[Byte]";
    }
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
  SPARK_FILE << "    return mappingMethod(n0";
  for(unsigned i = 1; i<NbInputs+NbOutputSize; i++)
    SPARK_FILE << ", n" << i;
  SPARK_FILE << ")\n";
  SPARK_FILE << "  }\n\n";

  for(auto it = reductionMap.begin(); it != reductionMap.end(); ++it) {
    SPARK_FILE << "  @native def reduceMethod"<< it->first->getName() << "(n0 : Array[Byte], n1 : Array[Byte]) : Array[Byte]\n\n" ;
  }

  for(auto it = ReorderInputVarUse.begin(); it != ReorderInputVarUse.end(); ++it) {
    unsigned hash = it->first;
    int NbInputs = it->second.size();



    SPARK_FILE << "  @native def reorderMethod"<< hash << "(";
    int i=0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2, i++) {
      // Separator
      if(it2 != it->second.begin())
        SPARK_FILE << ", ";

      bool isCnt = CntMap.find(it2->first) != CntMap.end();
      if(isCnt) {
        SPARK_FILE << "n" << i << ": Long";
      } else {
        SPARK_FILE << "n" << i << ": Array[Byte]";
      }
    }
    SPARK_FILE << ") : Long\n";

    SPARK_FILE << "  def reorderMethodWrapper"<< hash << "(";
    i=0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2, i++) {
      // Separator
      if(it2 != it->second.begin())
        SPARK_FILE << ", ";

      bool isCnt = CntMap.find(it2->first) != CntMap.end();
      if(isCnt) {
        SPARK_FILE << "n" << i << ": Long";
      } else {
        SPARK_FILE << "n" << i << ": Array[Byte]";
      }
    }
    SPARK_FILE << ") : Long";
    SPARK_FILE << " = {\n";
    SPARK_FILE << "    NativeKernels.loadOnce()\n";
    SPARK_FILE << "    return reorderMethod"<< hash << "(n0";
    for(unsigned i = 1; i<NbInputs; i++)
      SPARK_FILE << ", n" << i;
    SPARK_FILE << ")\n";
    SPARK_FILE << "  }\n\n";
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
    SparkExpr += "ByteBuffer.wrap(arg";
    SparkExpr += std::to_string(id);
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
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& InputOutputVarUse = CGM.OpenMPSupport.getOffloadingInputOutputVarUse();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& CntMap = CGM.OpenMPSupport.getOffloadingCounterInfo();

  SPARK_FILE << "    // Read each input from cloud-based filesystem\n";
  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
  {
    int id = IndexMap[it->first];

    // Find the bit size of one element
    QualType varType = it->first->getType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(varType) / 8;

    SPARK_FILE << "    val arg" << id << " = ";
    if(it->first->getType()->isAnyPointerType())
      SPARK_FILE << "info.sc.broadcast(";
    SPARK_FILE << "fs.read(" << id << ", " << SizeInByte << ")";
    if(it->first->getType()->isAnyPointerType())
      SPARK_FILE << ")";
    SPARK_FILE << " // Variable " << it->first->getName() <<"\n";
  }

  for (auto it = InputOutputVarUse.begin(); it != InputOutputVarUse.end(); ++it)
  {
    int id = IndexMap[it->first];

    // Find the bit size of one element
    QualType varType = it->first->getType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(varType) / 8;

    SPARK_FILE << "    val arg" << id << " = ";
    if(it->first->getType()->isAnyPointerType())
      SPARK_FILE << "info.sc.broadcast(";
    SPARK_FILE << "fs.read(" << id << ", " << SizeInByte << ")";
    if(it->first->getType()->isAnyPointerType())
      SPARK_FILE << ")";
    SPARK_FILE << " // Variable " << it->first->getName() <<"\n";
  }

  for (auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
  {
    int id = IndexMap[it->first];

    SPARK_FILE << "    val size" << id << " = at.get(" << id << ")";
    SPARK_FILE << " // Variable " << it->first->getName() <<"\n";
  }

  SPARK_FILE << "\n";
  SPARK_FILE << "    // Generate RDDs of index\n";
  int NbIndex = 0;

  for (auto it = CntMap.begin(); it != CntMap.end(); ++it)
  {
    const VarDecl *VarCnt = it->first;
    const Expr *Init = it->second[0];
    const Expr *Check = it->second[1];
    const Expr *Step = it->second[2];
    const Expr *CheckOp = it->second[3];

    const BinaryOperator *BO = cast<BinaryOperator>(CheckOp);

    SPARK_FILE << "    val index" << NbIndex << " = info.sc.parallelize(" << getSparkExprOf(Init) << ".toLong to " << getSparkExprOf(Check);
    if(BO->getOpcode() == BO_LT || BO->getOpcode() == BO_GT) {
      SPARK_FILE << "-1";
    }
    SPARK_FILE << " by " << getSparkExprOf(Step) << ")";
    SPARK_FILE << " // Index " << VarCnt->getName() << "\n";
    NbIndex++;
  }

  SPARK_FILE << "    val index = index0";
  for(int i=1; i<NbIndex; i++) {
    SPARK_FILE << ".cartesian(index" << i << ")";
  }
  SPARK_FILE << "\n"; // FIXME: Inverse with more indexes

  SPARK_FILE << "\n\n";
}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& InputOutputVarUse = CGM.OpenMPSupport.getOffloadingInputOutputVarUse();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& CntMap = CGM.OpenMPSupport.getOffloadingCounterInfo();

  unsigned NbInputs = 0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    NbInputs += it->second.size();

  SPARK_FILE << "    // Perform Map operations\n";
  if (NbInputs == 1)
    SPARK_FILE << "    val mapres = index.map{ new OmpKernel().mappingWrapper(_) }.persist\n";
  else {
    SPARK_FILE << "    val mapres = index.map{ x => new OmpKernel().mappingWrapper(";

    // Assign each argument according to its type
    int i=1;
    for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    {
      bool isCnt = CntMap.find(it->first) != CntMap.end();

      // Separator
      if(it != InputVarUse.begin())
        SPARK_FILE << ", ";
      if(isCnt) {
        if(CntMap.size() == 1) {
          SPARK_FILE << "x";
        } else {
          SPARK_FILE << "x._" << i;
          i++;
        }
      } else {
        int id = IndexMap[it->first];
        SPARK_FILE << "arg" << id;
        if(it->first->getType()->isAnyPointerType())
          SPARK_FILE << ".value";
      }
    }
    for(auto it = InputOutputVarUse.begin(); it != InputOutputVarUse.end(); ++it)
    {
      bool isCnt = CntMap.find(it->first) != CntMap.end();

      // Separator
      if(!InputVarUse.empty() || it != InputOutputVarUse.begin())
        SPARK_FILE << ", ";
      if(isCnt) {
        if(CntMap.size() == 1) {
          SPARK_FILE << "x";
        } else {
          SPARK_FILE << "x._" << i;
          i++;
        }
      } else {
        int id = IndexMap[it->first];
        SPARK_FILE << "arg" << id;
        if(it->first->getType()->isAnyPointerType())
          SPARK_FILE << ".value";
      }
    }
    for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
    {
      int id = IndexMap[it->first];
      SPARK_FILE << ", size" << id;
    }

    SPARK_FILE << ") }.persist\n\n";
  }
  SPARK_FILE << "    mapres.foreachPartition{ x =>  }\n";
  SPARK_FILE << "    val mapres2 = mapres.repartition(info.getExecutorNumber)\n";
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& InputOutputVarUse = CGM.OpenMPSupport.getOffloadingInputOutputVarUse();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();

  unsigned NbOutputs = OutputVarDef.size() + InputOutputVarUse.size();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";
  int i=0;

  for (auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
  {
    int id = IndexMap[it->first];
    bool isReduced = CGM.OpenMPSupport.isReduced(it->first);

    if(NbOutputs == 1) {
      // 1 output -> return the result directly
      SPARK_FILE << "    val res" << id << " = mapres2";
    }
    else if(NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << "    val res" << id << " = mapres2.map { x => x._" << i+1 << "}";
    }
    else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << "    val res" << id << " = mapres2.map { x => x(" << i << ") }";
    }

    if(isReduced)
      SPARK_FILE << ".reduce(new OmpKernel().reduceMethod"<< it->first->getName() << "(_, _))\n";
    else
      SPARK_FILE << "\n";

    SPARK_FILE << "    val output" << id << " = res" << id << ".treeReduce(Util.bitor)\n";
    SPARK_FILE << "\n";

    // Find the bit size of one element
    QualType varType = it->first->getType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }

    SPARK_FILE << "    fs.write(" << id << ", output" << id << ")\n";
    i++;
  }

  for (auto it = InputOutputVarUse.begin(); it != InputOutputVarUse.end(); ++it)
  {
    int id = IndexMap[it->first];
    bool isReduced = CGM.OpenMPSupport.isReduced(it->first);

    if(NbOutputs == 1) {
      // 1 output -> return the result directly
      SPARK_FILE << "    val res" << id << " = mapres";
    }
    else if(NbOutputs == 2 || NbOutputs == 3) {
      // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
      SPARK_FILE << "    val res" << id << " = mapres.map { x => x._" << i+1 << "}";
    }
    else {
      // More than 3 outputs -> extract each variable from the Collection
      SPARK_FILE << "    val res" << id << " = mapres.map { x => x(" << i << ") }";
    }

    if(isReduced)
      SPARK_FILE << ".reduce(new OmpKernel().reduceMethod"<< it->first->getName() << "(_, _))\n";
    else
      SPARK_FILE << "\n";

    SPARK_FILE << "    val output" << id << " = res" << id << ".treeReduce(Util.bitor)\n";
    SPARK_FILE << "\n";

    // Find the bit size of one element
    QualType varType = it->first->getType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }

    SPARK_FILE << "    fs.write(" << id << ", output" << id << ")\n";
    i++;
  }


}

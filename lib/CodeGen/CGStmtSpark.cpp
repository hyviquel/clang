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
  SPARK_FILE << "package org.llvm.openmp\n"
             << "\n";

  EmitSparkNativeKernel(SPARK_FILE);

  // Core
  SPARK_FILE << "object OmpKernel {"
             << "\n";

  SPARK_FILE << "  def main(args: Array[String]) {\n"
             << "    \n"
             << "    val info = new CloudInfo(args(0), args(1), args(2), args(3))\n"
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
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& reductionMap = CGM.OpenMPSupport.getReductionMap();
  auto& ReorderInputVarUse = CGM.OpenMPSupport.getReorderInputVarUse();
  auto& CntMap = CGM.OpenMPSupport.getOffloadingCounterInfo();

  unsigned NbInputs = 0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    NbInputs += it->second.size();
  // Outputs are reorder a posteriori
  unsigned NbOutputs = 0;
  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
    NbOutputs += it->second.size();

  if(verbose) llvm::errs() << "NbInput => " << NbInputs << "\n";
  if(verbose) llvm::errs() << "NbOutput => " << NbOutputs << "\n";
  SPARK_FILE << "\n";
  SPARK_FILE << "import org.apache.spark.SparkFiles\n";
  SPARK_FILE << "class OmpKernel {\n";
  SPARK_FILE << "  @native def mappingMethod(n0 : Array[Byte]";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i << " : Array[Byte]";
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
  SPARK_FILE << "  def mappingWrapper(n0 : Array[Byte]";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i << " : Array[Byte]";
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
  for(unsigned i = 1; i<NbInputs; i++)
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

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& InputReorderNb = CGM.OpenMPSupport.getOffloadingInputReorderNb();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();
  auto& CntMap = CGM.OpenMPSupport.getOffloadingCounterInfo();
  auto& ReorderInputVarUse = CGM.OpenMPSupport.getReorderInputVarUse();
  auto& InputsSet = CGM.OpenMPSupport.getOffloadingInputs();

  SPARK_FILE << "    // Read each input from HDFS and store them in RDDs\n";
  for (auto it = InputsSet.begin(); it != InputsSet.end(); ++it)
  {
    int id = IndexMap[*it];

    // Find the bit size of one element
    QualType varType = (*it)->getType();
    bool isRDD = varType->isAnyPointerType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(varType) / 8;

    SPARK_FILE << "    val arg" << id << " = ";
    if(isRDD)
      SPARK_FILE << "info.indexedRead(" << id << ", " << SizeInByte << ")";
    else
      SPARK_FILE << "info.read(" << id << ", " << SizeInByte << ")";
    SPARK_FILE << " // Variable " << (*it)->getName() <<"\n";
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

    llvm::APSInt initValue, checkValue, stepValue;

    bool isInitEvaluable = Init->EvaluateAsInt(initValue, getContext());
    bool isCheckEvaluable = Check->EvaluateAsInt(checkValue, getContext());
    bool isStepEvaluable = Step->EvaluateAsInt(stepValue, getContext());

    if(!isInitEvaluable || !isCheckEvaluable || !isStepEvaluable) {
      llvm::errs()  << "Cannot fully detect its scope statically:\n"
                    << "Require the generation of native kernels to compute it during the execution.\n";
    } else {
      llvm::errs() << "From " << initValue.getSExtValue() << " until " << checkValue.getSExtValue() << " with step " << stepValue.getSExtValue() << "\n";
    }

    SPARK_FILE << "    val index" << NbIndex << " = info.sc.parallelize(" << initValue.getSExtValue() << ".toLong to " << checkValue.getSExtValue() << " by " << stepValue.getSExtValue() << ")";
    SPARK_FILE << " // Index " << VarCnt->getName() << "\n";
    NbIndex++;
  }

  SPARK_FILE << "    val index = index0";
  for(int i=1; i<NbIndex; i++) {
    SPARK_FILE << ".cartesian(index" << i << ")";
  }
  SPARK_FILE << ".zipWithIndex().map{case (x,y) => (y,x)}\n"; // FIXME: Inverse with more indexes

  SPARK_FILE << "\n";
  SPARK_FILE << "    // Create RDDs containing reordered indexes\n";
  for(auto it = ReorderInputVarUse.begin(); it != ReorderInputVarUse.end(); ++it) {
    unsigned hash = it->first;
    int NbInputs = it->second.size();
    auto& InputVarUse = ReorderInputVarUse[hash];

    if(NbIndex == 1) {
      SPARK_FILE << "    val reorder" << std::to_string(hash) << " = index.mapValues{new OmpKernel().reorderMethodWrapper"<< std::to_string(hash) << "(_)}\n";
    }
    else {
      SPARK_FILE << "    val reorder" << std::to_string(hash) << " = index.mapValues{x => new OmpKernel().reorderMethodWrapper"<< std::to_string(hash) << "(";

      // Assign each argument according to its type
      int i=1;
      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        // Separator
        if(it2 != it->second.begin())
          SPARK_FILE << ", ";

        bool isCnt = CntMap.find(it2->first) != CntMap.end();
        if(isCnt) {
          SPARK_FILE << "x._" << i;
          i++;
        } else {
          int id = IndexMap[it2->first];
          SPARK_FILE << "arg" << id;
        }
    }
      SPARK_FILE << ")}\n";
    }
  }

  SPARK_FILE << "\n";
  SPARK_FILE << "    // Reorder input when needed\n";

  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it) {
    int id = IndexMap[it->first];
    int id2 = 0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);
        SPARK_FILE << "    val input" << id << "_" << id2 << " = reorder" << std::to_string(ExprID.ComputeHash()) << ".map{case (x,y) => (y,x)}.join(arg" << id << ").map(_._2)\n";
      }
      id2++;
    }
  }

  SPARK_FILE << "    \n";
  SPARK_FILE << "    // Create RDD of tuples with each argument to one call of the map function\n";
  SPARK_FILE << "    var mapargs = ";

  int i=0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
  {
    // Add only array argument to the RDD
    bool isRDD = it->first->getType()->isAnyPointerType();
    if(isRDD) {
      int id = IndexMap[it->first];
      int id2 = 0;
      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        if(i!=0) SPARK_FILE << ".join(";
        if(const Expr* reorderExpr = ReorderMap[*it2]) {
          SPARK_FILE << "input" << id << "_" << id2;
        }
        else {
          SPARK_FILE << "arg" << id;
        }
        if(i!=0) SPARK_FILE << ")";
        if(i>1) {
          int j;
          // Flatten the RDDs after the join when needed
          SPARK_FILE << ".mapValues{case ((u0";
          for(j=1; j<i; j++) {
            SPARK_FILE << ", " << "u" << j;
          }
          SPARK_FILE << "), u" << j << ") => (u0";
          for(j=1; j<=i; j++) {
            SPARK_FILE << ", " << "u" << j;
          }
          SPARK_FILE << ")}";
        }
        i++;
        id2++;
      }
    }
  }
  SPARK_FILE << "\n\n";
}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();

  unsigned NbInputs = 0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    NbInputs += it->second.size();

  SPARK_FILE << "    // Perform Map-Reduce operations\n";
  if (NbInputs == 1)
    SPARK_FILE << "    var mapres = mapargs.mapValues{ new OmpKernel().mappingWrapper(_) }\n";
  else {
    SPARK_FILE << "    var mapres = mapargs.mapValues{ x => new OmpKernel().mappingWrapper(";

    // Assign each argument according to its type
    int i=1;
    for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    {
      bool isRDD = it->first->getType()->isAnyPointerType();

      for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
        // Separator
        if(it != InputVarUse.begin() || it2 != it->second.begin())
          SPARK_FILE << ", ";
        if(isRDD) {
          SPARK_FILE << "x._" << i;
          i++;
        } else {
          int id = IndexMap[it->first];
          SPARK_FILE << "arg" << id;
        }
      }
    }

    SPARK_FILE << ") }\n\n";
  }
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();
  auto& IndexMap = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex();

  unsigned NbOutputs = 0;
  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
    NbOutputs += it->second.size();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";
  int i=0;

  for (auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
  {
    int id = IndexMap[it->first];
    int id2 = 0;
    bool isReduced = CGM.OpenMPSupport.isReduced(it->first);

    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(NbOutputs == 1) {
        // 1 output -> return the result directly
        SPARK_FILE << "    val res" << id << "_" << id2 << " = mapres";
      }
      else if(NbOutputs == 2 || NbOutputs == 3) {
        // 2 or 3 outputs -> extract each variable from the Tuple2 or Tuple3
        SPARK_FILE << "    val res" << id << "_" << id2 << " = mapres.mapValues { x => x._" << i+1 << "}";
      }
      else {
        // More than 3 outputs -> extract each variable from the Collection
        SPARK_FILE << "    val res" << id << "_" << id2 << " = mapres.mapValues { x => x(" << i << ") }";
      }

      if(isReduced)
        SPARK_FILE << ".reduce(new OmpKernel().reduceMethod"<< it->first->getName() << "(_, _))\n";
      else
        SPARK_FILE << "\n";

      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);
        SPARK_FILE << "    val ordered_arg" << id << "_" << id2 << " = reorder" << std::to_string(ExprID.ComputeHash()) << ".join(res" << id << "_" << id2 << ").map(_._2)\n";
      } else {
        SPARK_FILE << "    val ordered_arg" << id << "_" << id2 << " = res" << id << "_" << id2 << "\n";
      }

      id2++;
    }

    SPARK_FILE << "    val output" << id << " = ";
    std::string sep1, sep2 = "";
    id2 = 0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
       SPARK_FILE << sep1 << "ordered_arg" << id << "_" << id2 << sep2;
       sep1 = ".++(";
       sep2 = ")";
       id2++;
    }
    SPARK_FILE << "\n";

    // Find the bit size of one element
    QualType varType = it->first->getType();

    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(varType) / 8;

    SPARK_FILE << "    info.indexedWrite(" << id << ", " << SizeInByte << ", output" << id << ")\n";
    i++;
  }

}

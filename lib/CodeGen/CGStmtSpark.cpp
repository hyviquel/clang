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
             << "    val info = new CloudInfo(args(0), args(1), args(2))\n"
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
  else if (NbOutputs == 2)
    SPARK_FILE << "Tuple2[Array[Byte], Array[Byte]]";
  else if (NbOutputs == 3)
    SPARK_FILE << "Tuple3[Array[Byte], Array[Byte], Array[Byte]]";
  else
    SPARK_FILE << "Seq[Array[Byte]]";
  SPARK_FILE << "\n";
  SPARK_FILE << "  def mappingWrapper(n0 : Array[Byte]";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i << " : Array[Byte]";
  SPARK_FILE << ") : ";
  if (NbOutputs == 1)
    SPARK_FILE << "Array[Byte]";
  else if (NbOutputs == 2)
    SPARK_FILE << "Tuple2[Array[Byte], Array[Byte]]";
  else if (NbOutputs == 3)
    SPARK_FILE << "Tuple3[Array[Byte], Array[Byte], Array[Byte]]";
  else
    SPARK_FILE << "Seq[Array[Byte]]";
  SPARK_FILE << " = {\n";
  SPARK_FILE << "    System.load(SparkFiles.get(\"libmr.so\"))\n";
  SPARK_FILE << "    return mappingMethod(n0";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i;
  SPARK_FILE << ")\n";
  SPARK_FILE << "  }\n\n";

  llvm::DenseMap<const VarDecl *, unsigned> reductionMap = CGM.OpenMPSupport.getReductionMap();

  for(auto it = reductionMap.begin(); it != reductionMap.end(); ++it) {
    SPARK_FILE << "  @native def reduceMethod"<< it->first->getName() << "(n0 : Array[Byte], n1 : Array[Byte]) : Array[Byte]\n\n" ;
  }

  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);
        SPARK_FILE << "  @native def reorderMethod"<< std::to_string(ExprID.ComputeHash()) << "(n0 : Long";
        // TODO: Add multiple input
        SPARK_FILE << ") : Long\n";
      }
    }
  }

  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);
        SPARK_FILE << "  @native def reorderMethod"<< std::to_string(ExprID.ComputeHash()) << "(n0 : Long";
        // TODO: Add multiple input
        SPARK_FILE << ") : Long\n";
      }
    }
  }

  SPARK_FILE << "}\n\n";
}

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& InputReorderNb = CGM.OpenMPSupport.getOffloadingInputReorderNb();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();

  SPARK_FILE << "    // Read each input from HDFS and store them in RDDs\n";
  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
  {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];

    // Find the bit size of one element
    QualType varType = it->first->getType();
    while(varType->isAnyPointerType()) {
      varType = varType->getPointeeType();
    }
    int64_t SizeInByte = getContext().getTypeSize(varType) / 8;

    SPARK_FILE << "    val arg" << id << " = info.indexedRead(" << id << ", " << SizeInByte << ")"
               << " // Variable " << it->first->getName() <<"\n";
  }

  int NbIndex = 1;

  for(int i=0; i<NbIndex; i++) {
    SPARK_FILE << "    val index" << i << " = info.sc.parallelize(0.toLong to 100000)";
    SPARK_FILE << "// Variable ??\n";
  }

  SPARK_FILE << "    val index = index0";
  for(int i=1; i<NbIndex; i++) {
    SPARK_FILE << ".cartesian(index" << i << ")";
  }
  SPARK_FILE << ".zipWithIndex().map{case (x,y) => (y,x)}"; // FIXME: Inverse with more indexes

  SPARK_FILE << "    // Reorder input when needed\n";


  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    int id2 = 0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);

        if(NbIndex == 1) {
          SPARK_FILE << "    val reorder" << id << "_" << id2 << " = index.mapValues{new OmpKernel().reorderMethod"<< std::to_string(ExprID.ComputeHash()) << "(_)}\n";
        }
        else {
          // TODO:
        }

        SPARK_FILE << "    val input" << id << "_" << id2 << " = reorder" << id << "_" << id2 << ".join(arg" << id << ").map(_._2)\n";
      }
      id2++;
    }
  }

  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it) {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    int id2 = 0;
    for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      if(const Expr* reorderExpr = ReorderMap[*it2]) {
        llvm::FoldingSetNodeID ExprID;
        reorderExpr->Profile(ExprID, getContext(), true);

        if(NbIndex == 1) {
          SPARK_FILE << "    val reorder" << id << "_" << id2 << " = index.mapValues{new OmpKernel().reorderMethod"<< std::to_string(ExprID.ComputeHash()) << "(_)}\n";
        }
        else {
          // TODO:
        }

        // SPARK_FILE << "    val input" << id << "_" << id2 << " = reorder" << id << "_" << id2 << ".join(arg" << id << ").map(_._2)\n";
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
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
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
      if(i>1) SPARK_FILE << ".map{case (k, ((x, y), z)) => (k, (x, y, z))}";
      i++;
      id2++;
    }
  }
  SPARK_FILE << "\n\n";
}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();

  unsigned NbInputs = 0;
  for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    NbInputs += it->second.size();

  SPARK_FILE << "    // Perform Map-Reduce operations\n";
  if (NbInputs == 1)
    SPARK_FILE << "    var mapres = mapargs.mapValues{ new OmpKernel().mappingWrapper(_) }\n";
  else {
    SPARK_FILE << "    var mapres = mapargs.mapValues{ x => new OmpKernel().mappingWrapper(x._1";
    for(unsigned i = 1; i<NbInputs; ++i)
      SPARK_FILE << ", x._" << i+1;
    SPARK_FILE << ") }\n\n";
  }
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& OutputVarDef = CGM.OpenMPSupport.getOffloadingOutputVarDef();
  auto& ReorderMap = CGM.OpenMPSupport.getReorderMap();

  unsigned NbOutputs = 0;
  for(auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
    NbOutputs += it->second.size();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";
  int i=0;

  for (auto it = OutputVarDef.begin(); it != OutputVarDef.end(); ++it)
  {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
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
        SPARK_FILE << "    val ordered_arg" << id << "_" << id2 << " = reorder" << id << "_" << id2 << ".join(res" << id << "_" << id2 << ").map(_._2)\n";
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

    SPARK_FILE << "    info.indexedWrite(" << id << ", output" << id << ")\n";
    i++;
  }

}

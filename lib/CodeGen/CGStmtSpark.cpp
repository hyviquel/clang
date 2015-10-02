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
  unsigned NbInputs = CGM.OpenMPSupport.getOffloadingInputVarUse().size();
  unsigned NbOutputs = CGM.OpenMPSupport.getOffloadingOutputVarDef().size();

  llvm::errs() << "NbInput => " << NbInputs << "\n";
  llvm::errs() << "NbInput => " << NbOutputs << "\n";

  SPARK_FILE << "\n";
  SPARK_FILE << "class OmpKernel {\n";
  SPARK_FILE << "  @native def mappingMethod(n0 : Array[Byte]";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i << " : Array[Byte]";
  SPARK_FILE << ") : ";
  if (NbOutputs == 1)
    SPARK_FILE << "Array[Byte]";
  else
    SPARK_FILE << "Seq[Array[Byte]]";
  SPARK_FILE << "\n";
  SPARK_FILE << "}\n\n";
}

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& InputVarUse = CGM.OpenMPSupport.getOffloadingInputVarUse();

  SPARK_FILE << "    // Read each input from HDFS and store them in RDDs\n";
  for (auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
  {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
    // FIXME: compute exact size
    SPARK_FILE << "    val arg" << id << " = info.read(" << id << ", 4)"
               << " // Variable " << it->first->getName() <<"\n";
  }

  if (InputVarUse.size() > 1) {
    SPARK_FILE << "    \n";
    SPARK_FILE << "    // Create RDD of tuples with each argument to one call of the map function\n";
    SPARK_FILE << "    var mapargs = Util.makeZip(Seq(";
    std::string prevSep = "";
    for(auto it = InputVarUse.begin(); it != InputVarUse.end(); ++it)
    {
      SPARK_FILE << prevSep << "arg" << CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
      prevSep = ", ";
    }
    SPARK_FILE << "))\n\n";
  }

}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE) {
  unsigned NbInputs = CGM.OpenMPSupport.getOffloadingInputVarUse().size();

  SPARK_FILE << "    // Perform Map-Reduce operations\n";
  if (NbInputs == 1)
    SPARK_FILE << "    var mapres = arg0.map{ new OmpKernel().mappingMethod(_) }\n";
  else {
    SPARK_FILE << "    var mapres = mapargs.map{ x => new OmpKernel().mappingMethod(x(0)";
    for(unsigned i = 1; i<NbInputs; i++)
      SPARK_FILE << ", x(" << i << ")";
    SPARK_FILE << ") }\n\n";
  }
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  auto& OutputVarUse = CGM.OpenMPSupport.getOffloadingOutputVarDef();

  SPARK_FILE << "    // Get the results back and write them in the HDFS\n";

  for (auto it = OutputVarUse.begin(); it != OutputVarUse.end(); ++it)
  {
      int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
      if(OutputVarUse.size() == 1)
        SPARK_FILE << "    val res" << id << " = mapres.collect()\n";
      else
        SPARK_FILE << "    val res" << id << " = mapres.map { x => x(" << id << ") }.collect()\n";
      SPARK_FILE << "    info.write(" << id << ", res" << id << ".flatten)\n";
  }

}

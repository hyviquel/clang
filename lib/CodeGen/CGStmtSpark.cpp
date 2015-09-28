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
  SPARK_FILE << "package test.dummy\n" // FIXME: Update package name
             << "\n"
             << "import org.apache.spark.rdd.RDD\n"
             << "import org.apache.spark.SparkConf\n"
             << "import org.apache.spark.SparkContext\n"
             << "\n"
             << "import org.llvm.openmp.AddressTable\n"
             << "import org.llvm.openmp.HdfsInfo\n"
             << "import org.llvm.openmp.Util\n"
             << "\n";

  // Core
  SPARK_FILE << "object CleanTest {" // FIXME: Update object name
             << "\n";

  EmitSparkNativeKernel(SPARK_FILE);

  SPARK_FILE << "  def main(args: Array[String]) {\n"
             << "    \n"
             << "    val conf = new SparkConf()\n"
             << "    val sc = new SparkContext(conf)\n"
             << "    val info = new HdfsInfo(args(0), args(1), args(2))\n"
             << "    \n"
             << "    // Load JNI library containing mapping kernels to every nodes\n"
             << "    Util.loadLibrary(sc, info)\n"
             << "    \n"
             << "    // Parsing address table\n"
             << "    val addressTable = new AddressTable(sc, info)\n"
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

  /*
  // FIXME: Update signature
  SPARK_FILE << "  \n"
             << "  @native def mappingMethod(n0 : Array[Byte]";
  for(unsigned i = 1; i<NbInputs; i++)
    SPARK_FILE << ", n" << i << " : Array[Byte]";
  SPARK_FILE << ") : ";

  if(NbOutputs == 1) {
    // Use a simple ByteArray
    SPARK_FILE << "Array[Byte]";
  }
  else {
    // Use a tuple of ByteArray
    SPARK_FILE << "(Array[Byte],";
    for(unsigned i = 1; i<NbOutputs-1; i++)
      SPARK_FILE << " Array[Byte],";
    SPARK_FILE << " Array[Byte])";
  }*/

  SPARK_FILE << "  \n";
  SPARK_FILE << "  @native def mappingMethod(n: ";
  if (NbInputs == 1)
    SPARK_FILE << "Array[Byte]";
  else
    SPARK_FILE << "Seq[Array[Byte]]";
  SPARK_FILE << "): ";
  if (NbOutputs == 1)
    SPARK_FILE << "Array[Byte]";
  else
    SPARK_FILE << "Seq[Array[Byte]]";
  SPARK_FILE << "\n";
  SPARK_FILE << "\n\n";
}

void CodeGenFunction::EmitSparkInput(llvm::raw_fd_ostream &SPARK_FILE) {

  SPARK_FILE << "    // Create RDD of tuples with each argument to one call of the map function\n";
            // << "    var mapargs = arguments(2)\n";
  for (auto it = CGM.OpenMPSupport.getOffloadingInputVarUse().begin(); it != CGM.OpenMPSupport.getOffloadingInputVarUse().end(); ++it)
  {
    int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];

    SPARK_FILE << "    val arg" << id << " = sc.binaryRecords(addressTable.fullPath(" << id << "), 4)\n";
  }

  //SPARK_FILE << "var maparg = Util.makeZip(" << CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first] << ")";
  SPARK_FILE << "    var mapargs = Util.makeZip(Seq(arg0))";

}

void CodeGenFunction::EmitSparkMapping(llvm::raw_fd_ostream &SPARK_FILE) {
  unsigned NbInputs = CGM.OpenMPSupport.getOffloadingInputVarUse().size();

  SPARK_FILE << "    // Create RDD of tuples with each argument to one call of the map function\n";
  if (NbInputs == 1)
    SPARK_FILE << "    var mapres = arg0.map{ mappingMethod(_) }\n";
  else {
    SPARK_FILE << "    var mapres = mapargs.map{ mappingMethod(_) }\n";
  }
}

void CodeGenFunction::EmitSparkOutput(llvm::raw_fd_ostream &SPARK_FILE) {
  unsigned NbOutputs = CGM.OpenMPSupport.getOffloadingOutputVarDef().size();

  SPARK_FILE << "    // Get the result\n";

  for (auto it = CGM.OpenMPSupport.getOffloadingOutputVarDef().begin(); it != CGM.OpenMPSupport.getOffloadingOutputVarDef().end(); ++it)
  {
      int id = CGM.OpenMPSupport.getLastOffloadingMapVarsIndex()[it->first];
      if(NbOutputs == 1)
        SPARK_FILE << "    val res" << id << " = mapres.collect()\n";
      else
        SPARK_FILE << "    val res" << id << " = mapres.map { x => x(" << id << ") }.collect()\n";
      SPARK_FILE << "    info.write(addressTable(" << id << ").path, res" << id << ".flatten)\n";
  }

}

//===-- clang/Analysis/DefUse.h - DefUse analysis -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of DefUse analysis headers
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_DEFUSE_H
#define LLVM_CLANG_DEFUSE_H

#include "llvm/ADT/DenseMap.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtVisitor.h"

namespace clang {

// forward definitions
class CFG;
class CFGBlock;

class DefUse;

namespace defuse {
//===------------------------- DefUseNode -------------------------===//
// Keeps the information for a particular 'definition' or 'use' of a variable
// The structure is needed because a defuse chain can contains variable declarations
// as well as variable reference. As a DeclStmt can contains several declarations
// to address a particular variable we need to store its VarDecl or DeclRefExp
class DefUseNode {
public:
  enum NodeKind {
    VarDecl, VarRef
  };
  enum UseKind {
    Use, Def, UseDef
  };

  DefUseNode(clang::VarDecl const* decl) :
    var_decl(decl), kind(VarDecl), usage(Def) {
  }
  DefUseNode(DeclRefExpr const* ref, UseKind u = Use) :
    var_ref(ref), kind(VarRef), usage(u) {
  }

  clang::VarDecl const* getDecl() const;

  NodeKind const& getKind() const {
    return kind;
  }
  UseKind const& getUse() const {
    return usage;
  }

  clang::VarDecl const* getVarDecl() const {
    assert(kind==VarDecl);
    return var_decl;
  }
  DeclRefExpr const* getVarRef() const {
    assert(kind==VarRef);
    return var_ref;
  }

  bool operator==(DefUseNode const& n) const;

private:
  // a def-use node can be either a VarDecl or a DeclRefExpr
  union {
    clang::VarDecl const* var_decl;
    DeclRefExpr const* var_ref;
  };
  NodeKind kind;
  UseKind usage;
};

//===------------------------- Typedefs -------------------------===//
class DefUseBlock;
class VarDeclMap;
typedef std::vector<DefUseBlock> DefUseData;
typedef llvm::DenseMap<Stmt const*, unsigned> VarRefBlockMap;
typedef std::vector<DefUseNode> DefUseVect;
typedef std::vector<DefUseNode const*> VarRefsVect;

//===------------------------- DefUseHelper -------------------------===//

class DefUseHelper : public ConstStmtVisitor<DefUseHelper> {
  struct DefUseHelperImpl;
  DefUseHelperImpl* pimpl;

  void InitializeValues(DefUseData* data, VarRefBlockMap* bm, VarDeclMap* dm);

  friend class clang::DefUse;
public:
  DefUseNode::UseKind current_use;
  DefUseHelper();

  virtual void HandleDeclRefExpr(const DeclRefExpr *DR); // remember to call the
                              // super class implementation of the method
  virtual void HandleDeclStmt(const DeclStmt *DS);

  virtual void HandleBinaryOperator(const BinaryOperator* B);
  virtual void HandleConditionalOperator(const ConditionalOperator* C);
  virtual void HandleCallExpr(const CallExpr* C);
  virtual void HandleUnaryOperator(const UnaryOperator* U);
  virtual void HandleArraySubscriptExpr(const ArraySubscriptExpr* AS);
  virtual void HandleOMPTargetDataDirective(const OMPTargetDataDirective *S);

  void VisitDeclRefExpr(const DeclRefExpr *DR) {
    return HandleDeclRefExpr(DR);
  }
  void VisitDeclStmt(const DeclStmt *DS) {
    return HandleDeclStmt(DS);
  }
  void VisitBinaryOperator(const BinaryOperator* B) {
    return HandleBinaryOperator(B);
  }
  void VisitConditionalOperator(const ConditionalOperator* C) {
    return HandleConditionalOperator(C);
  }
  void VisitCallExpr(const CallExpr* C) {
    return HandleCallExpr(C);
  }
  void VisitUnaryOperator(const UnaryOperator* U) {
    return HandleUnaryOperator(U);
  }
  void VisitArraySubscriptExpr(const ArraySubscriptExpr* AS) {
    return HandleArraySubscriptExpr(AS);
  }
  void VisitOMPTargetDataDirective(const OMPTargetDataDirective *S) {
    return HandleOMPTargetDataDirective(S);
  }

  void VisitCFGBlock(const clang::CFGBlock &blk, const CFGBlock &entry);
  void VisitStmt(const Stmt* S);

  virtual ~DefUseHelper();
};

void PrintVarDefs(DefUse const* DU, DeclRefExpr const* DR, ASTContext& ctx,
    llvm::raw_ostream& out);
void PrintVarUses(DefUse const* DU, DeclRefExpr const* DR, ASTContext& ctx,
    llvm::raw_ostream& out);
void PrintVarUses(DefUse const* DU, VarDecl const* VD, ASTContext& ctx,
    llvm::raw_ostream& out);

ASTConsumer* CreateDefUseTestConsumer(llvm::raw_ostream& out);

} // end namespace defuse

//===------------------------- DefUse -------------------------===//

class DefUse {
  ASTContext const& ctx;
  defuse::DefUseData const* analysis_data;
  defuse::VarDeclMap const* decl_map;
  defuse::VarRefBlockMap const* block_map;
  unsigned const num_cfg_blocks;

  DefUse(ASTContext const& ctx_,
      defuse::DefUseData const* analysis_data_,
      defuse::VarDeclMap const* decl_map_,
      defuse::VarRefBlockMap const* block_map_,
      unsigned num_blocks) :
    ctx(ctx_), analysis_data(analysis_data_), decl_map(decl_map_),
    block_map(block_map_), num_cfg_blocks(num_blocks) {
  }

  bool isDef(defuse::DefUseNode const& n) const;

  class iterator_impl {
  public:
    DefUse const* du;
    defuse::DefUseNode const* node;

    struct iter {
      int block_id;
      defuse::DefUseVect::const_iterator block_it;
      iter(int blk_id) :
        block_id(blk_id) {
      }
    };
    iterator_impl() : du(NULL) { }
    iterator_impl(DefUse const* du_) : du(du_) { }
    iterator_impl& operator++() {
      return inc(false);
    }
    virtual iterator_impl& inc(bool) = 0;
    virtual ~iterator_impl();
  };
public:
  class uses_iterator : public std::iterator<std::input_iterator_tag,
    DeclRefExpr, std::ptrdiff_t, DeclRefExpr const*>,
    public iterator_impl {
    iter iter_ptr;
    bool inDefBlock;

    uses_iterator() :
      iterator_impl(), iter_ptr(-1), inDefBlock(true) { }
    uses_iterator(DefUse const* du, defuse::DefUseNode const& n);
    uses_iterator& inc(bool first);
    friend class DefUse;
  public:
    bool operator!=(uses_iterator const& iter);
    DeclRefExpr const* operator*();
  };

  class defs_iterator : public std::iterator<std::input_iterator_tag,
  defuse::DefUseNode, std::ptrdiff_t, defuse::DefUseNode const*>,
  public iterator_impl {
    struct iter_ : public iter {
      defuse::VarRefsVect::const_iterator reaches_it;
      iter_(int blk_id) :
        iter(blk_id) { }
    } iter_ptr;
    bool blockDef;

    defs_iterator() :
      iterator_impl(), iter_ptr(-1), blockDef(false) { }
    defs_iterator(DefUse const* du, DeclRefExpr const& n);
    defs_iterator& inc(bool first);
    friend class DefUse;
  public:
    bool operator!=(defs_iterator const& iter);
    defuse::DefUseNode const* operator*();
  };

  // USES //
  defs_iterator defs_begin(DeclRefExpr const* var) const;
  defs_iterator defs_end() const;

  // DEFS //
  uses_iterator uses_begin(DeclRefExpr const* var) const;
  uses_iterator uses_begin(VarDecl const* var) const;
  uses_iterator uses_end() const;

  bool isUse(DeclRefExpr const* var) const;
  bool isDef(DeclRefExpr const* var) const {
    return isDef(defuse::DefUseNode(var, defuse::DefUseNode::Def));
  }
  bool isDef(VarDecl const* var) const {
    return isDef(defuse::DefUseNode(var));
  }

  ~DefUse();

  static DefUse* BuildDefUseChains(Stmt* body, ASTContext *ctx,
      CFG* cfg, ParentMap* pm, defuse::DefUseHelper* helper = 0,
      bool verbose = false, llvm::raw_ostream& out = llvm::outs());
};

} // end namespace clang

#endif

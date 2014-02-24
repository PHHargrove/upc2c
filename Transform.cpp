#include <clang/Tooling/Tooling.h>
#include <clang/Sema/SemaConsumer.h>
#include <clang/Sema/Scope.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/Arg.h>
#include <clang/Driver/Options.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Path.h>
#include <string>
#include <cctype>
#include "../../lib/Sema/TreeTransform.h"

using namespace clang;
using namespace clang::tooling;
using llvm::APInt;

namespace {

  struct is_ident_char {
    typedef bool result_type;
    typedef char argument_type;
    bool operator()(char arg) const {
      return std::isalnum(arg) || arg == '_';
    }
  };

  std::string get_file_id(const std::string& filename) {
    uint32_t seed = 0;
    for(std::string::const_iterator iter = filename.begin(), end = filename.end(); iter != end; ++iter) {
      seed ^= uint32_t(*iter) + 0x9e3779b9 + (seed<<6) + (seed>>2);
    }
    std::string as_identifier(llvm::sys::path::stem(filename));
    std::replace_if(as_identifier.begin(), as_identifier.end(), std::not1(is_ident_char()), '_');
    return (as_identifier + "_" + llvm::Twine(seed)).str();
  }

  /* Copied from DeclPrinter.cpp */
  static QualType GetBaseType(QualType T) {
    // FIXME: This should be on the Type class!
    QualType BaseType = T;
    while (!BaseType->isSpecifierType()) {
      if (isa<TypedefType>(BaseType))
	break;
      else if (const PointerType* PTy = BaseType->getAs<PointerType>())
	BaseType = PTy->getPointeeType();
      else if (const BlockPointerType *BPy = BaseType->getAs<BlockPointerType>())
	BaseType = BPy->getPointeeType();
      else if (const ArrayType* ATy = dyn_cast<ArrayType>(BaseType))
	BaseType = ATy->getElementType();
      else if (const FunctionType* FTy = BaseType->getAs<FunctionType>())
	BaseType = FTy->getResultType();
      else if (const VectorType *VTy = BaseType->getAs<VectorType>())
	BaseType = VTy->getElementType();
      else if (const ReferenceType *RTy = BaseType->getAs<ReferenceType>())
	BaseType = RTy->getPointeeType();
      else
	llvm_unreachable("Unknown declarator!");
    }
    return BaseType;
  }

  static QualType getDeclType(Decl* D) {
    if (TypedefNameDecl* TDD = dyn_cast<TypedefNameDecl>(D))
      return TDD->getUnderlyingType();
    if (ValueDecl* VD = dyn_cast<ValueDecl>(D))
      return VD->getType();
    return QualType();
  }

  struct UPCRDecls {
    FunctionDecl * upcr_notify;
    FunctionDecl * upcr_wait;
    FunctionDecl * upcr_barrier;
    FunctionDecl * upcr_poll;
    FunctionDecl * upcr_mythread;
    FunctionDecl * upcr_threads;
    FunctionDecl * upcr_hasMyAffinity_pshared;
    FunctionDecl * upcr_hasMyAffinity_shared;
    FunctionDecl * UPCR_BEGIN_FUNCTION;
    FunctionDecl * UPCRT_STARTUP_PSHALLOC;
    FunctionDecl * UPCRT_STARTUP_SHALLOC;
    FunctionDecl * upcr_startup_pshalloc;
    FunctionDecl * upcr_startup_shalloc;
    FunctionDecl * upcr_put_pshared;
    FunctionDecl * upcr_put_shared;
    FunctionDecl * UPCR_GET_PSHARED;
    FunctionDecl * UPCR_PUT_PSHARED;
    FunctionDecl * UPCR_GET_SHARED;
    FunctionDecl * UPCR_PUT_SHARED;
    FunctionDecl * UPCR_ADD_SHARED;
    FunctionDecl * UPCR_INC_SHARED;
    FunctionDecl * UPCR_GET_PSHARED_STRICT;
    FunctionDecl * UPCR_PUT_PSHARED_STRICT;
    FunctionDecl * UPCR_GET_SHARED_STRICT;
    FunctionDecl * UPCR_PUT_SHARED_STRICT;
    FunctionDecl * UPCR_ADD_PSHAREDI;
    FunctionDecl * UPCR_ADD_PSHARED1;
    FunctionDecl * UPCR_INC_PSHAREDI;
    FunctionDecl * UPCR_INC_PSHARED1;
    FunctionDecl * UPCR_SUB_SHARED;
    FunctionDecl * UPCR_SUB_PSHAREDI;
    FunctionDecl * UPCR_SUB_PSHARED1;
    FunctionDecl * UPCR_ISEQUAL_SHARED_SHARED;
    FunctionDecl * UPCR_ISEQUAL_SHARED_PSHARED;
    FunctionDecl * UPCR_ISEQUAL_PSHARED_SHARED;
    FunctionDecl * UPCR_ISEQUAL_PSHARED_PSHARED;
    FunctionDecl * UPCR_PSHARED_TO_LOCAL;
    FunctionDecl * UPCR_SHARED_TO_LOCAL;
    FunctionDecl * UPCR_ISNULL_PSHARED;
    FunctionDecl * UPCR_ISNULL_SHARED;
    FunctionDecl * UPCR_SHARED_TO_PSHARED;
    FunctionDecl * UPCR_PSHARED_TO_SHARED;
    FunctionDecl * UPCR_SHARED_RESETPHASE;
    VarDecl * upcrt_forall_control;
    VarDecl * upcr_null_shared;
    VarDecl * upcr_null_pshared;
    QualType upcr_shared_ptr_t;
    QualType upcr_pshared_ptr_t;
    QualType upcr_startup_shalloc_t;
    QualType upcr_startup_pshalloc_t;
    SourceLocation FakeLocation;
    explicit UPCRDecls(ASTContext& Context) {
      SourceManager& SourceMgr = Context.getSourceManager();
      FakeLocation = SourceMgr.getLocForStartOfFile(SourceMgr.getMainFileID());

      // types

      // Make sure that the size and alignment are correct.
      QualType SharedPtrTy = Context.getPointerType(Context.getSharedType(Context.VoidTy));
      upcr_shared_ptr_t = CreateTypedefType(Context, "upcr_shared_ptr_t", SharedPtrTy);
      upcr_pshared_ptr_t = CreateTypedefType(Context, "upcr_pshared_ptr_t", SharedPtrTy);
      upcr_startup_shalloc_t = CreateTypedefType(Context, "upcr_startup_shalloc_t");
      upcr_startup_pshalloc_t = CreateTypedefType(Context, "upcr_startup_pshalloc_t");

      // upcr_notify
      {
	QualType argTypes[] = { Context.IntTy, Context.IntTy };
	upcr_notify = CreateFunction(Context, "upcr_notify", Context.VoidTy, argTypes, 2);
      }
      // upcr_wait
      {
	QualType argTypes[] = { Context.IntTy, Context.IntTy };
	upcr_wait = CreateFunction(Context, "upcr_wait", Context.VoidTy, argTypes, 2);
      }
      // upcr_barrier
      {
	QualType argTypes[] = { Context.IntTy, Context.IntTy };
	upcr_barrier = CreateFunction(Context, "upcr_barrier", Context.VoidTy, argTypes, 2);
      }
      // upcr_poll
      {
	upcr_poll = CreateFunction(Context, "upcr_poll", Context.VoidTy, 0, 0);
      }
      // upcr_mythread
      {
	upcr_mythread = CreateFunction(Context, "upcr_mythread", Context.IntTy, 0, 0);
      }
      // upcr_threads
      {
	upcr_threads = CreateFunction(Context, "upcr_threads", Context.IntTy, 0, 0);
      }
      // upcr_hasMyAffinity_pshared
      {
	QualType argTypes[] = { upcr_pshared_ptr_t };
	upcr_hasMyAffinity_pshared = CreateFunction(Context, "upcr_hasMyAffinity_pshared", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcr_hasMyAffinity_shared
      {
	QualType argTypes[] = { upcr_shared_ptr_t };
	upcr_hasMyAffinity_shared = CreateFunction(Context, "upcr_hasMyAffinity_shared", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcr_put_pshared
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	upcr_put_pshared = CreateFunction(Context, "upcr_put_pshared", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcr_put_shared
      {
	QualType argTypes[] = { upcr_shared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	upcr_put_shared = CreateFunction(Context, "upcr_put_shared", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_GET_PSHARED
      {
	QualType argTypes[] = { Context.VoidPtrTy, upcr_pshared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_GET_PSHARED = CreateFunction(Context, "UPCR_GET_PSHARED", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PUT_PSHARED
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	UPCR_PUT_PSHARED = CreateFunction(Context, "UPCR_PUT_PSHARED", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_GET_SHARED
      {
	QualType argTypes[] = { Context.VoidPtrTy, upcr_shared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_GET_SHARED = CreateFunction(Context, "UPCR_GET_SHARED", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PUT_SHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	UPCR_PUT_SHARED = CreateFunction(Context, "UPCR_PUT_SHARED", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_GET_PSHARED_STRICT
      {
	QualType argTypes[] = { Context.VoidPtrTy, upcr_pshared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_GET_PSHARED_STRICT = CreateFunction(Context, "UPCR_GET_PSHARED_STRICT", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PUT_PSHARED_STRICT
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	UPCR_PUT_PSHARED_STRICT = CreateFunction(Context, "UPCR_PUT_PSHARED_STRICT", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_GET_SHARED_STRICT
      {
	QualType argTypes[] = { Context.VoidPtrTy, upcr_shared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_GET_SHARED_STRICT = CreateFunction(Context, "UPCR_GET_SHARED_STRICT", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PUT_SHARED_STRICT
      {
	QualType argTypes[] = { upcr_shared_ptr_t, Context.IntTy, Context.VoidPtrTy, Context.IntTy };
	UPCR_PUT_SHARED_STRICT = CreateFunction(Context, "UPCR_PUT_SHARED_STRICT", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ADD_SHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t, Context.IntTy, Context.IntTy, Context.IntTy };
	UPCR_ADD_SHARED = CreateFunction(Context, "UPCR_ADD_SHARED", upcr_shared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ADD_PSHAREDI
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_ADD_PSHAREDI = CreateFunction(Context, "UPCR_ADD_PSHAREDI", upcr_pshared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ADD_PSHARED1
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_ADD_PSHARED1 = CreateFunction(Context, "UPCR_ADD_PSHARED1", upcr_pshared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_INC_SHARED
      {
	QualType argTypes[] = { Context.getPointerType(upcr_shared_ptr_t), Context.IntTy, Context.IntTy, Context.IntTy };
	UPCR_INC_SHARED = CreateFunction(Context, "upcr_inc_shared", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_INC_PSHAREDI
      {
	QualType argTypes[] = { Context.getPointerType(upcr_pshared_ptr_t), Context.IntTy, Context.IntTy };
	UPCR_INC_PSHAREDI = CreateFunction(Context, "upcr_inc_psharedI", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_INC_PSHARED1
      {
	QualType argTypes[] = { Context.getPointerType(upcr_pshared_ptr_t), Context.IntTy, Context.IntTy };
	UPCR_INC_PSHARED1 = CreateFunction(Context, "upcr_inc_pshared1", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SUB_SHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t, upcr_shared_ptr_t, Context.IntTy, Context.IntTy };
	UPCR_SUB_SHARED = CreateFunction(Context, "UPCR_SUB_SHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SUB_PSHAREDI
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, upcr_pshared_ptr_t, Context.IntTy };
	UPCR_SUB_PSHAREDI = CreateFunction(Context, "UPCR_SUB_PSHAREDI", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SUB_PSHARED1
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, upcr_pshared_ptr_t, Context.IntTy };
	UPCR_SUB_PSHARED1 = CreateFunction(Context, "UPCR_SUB_PSHARED1", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISEQUAL_SHARED_SHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t, upcr_shared_ptr_t };
	UPCR_ISEQUAL_SHARED_SHARED = CreateFunction(Context, "UPCR_ISEQUAL_SHARED_SHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISEQUAL_SHARED_PSHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t, upcr_pshared_ptr_t };
	UPCR_ISEQUAL_SHARED_PSHARED = CreateFunction(Context, "UPCR_ISEQUAL_SHARED_PSHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISEQUAL_PSHARED_SHARED
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, upcr_shared_ptr_t };
	UPCR_ISEQUAL_PSHARED_SHARED = CreateFunction(Context, "UPCR_ISEQUAL_PSHARED_SHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISEQUAL_PSHARED_PSHARED
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, upcr_pshared_ptr_t };
	UPCR_ISEQUAL_PSHARED_PSHARED = CreateFunction(Context, "UPCR_ISEQUAL_PSHARED_PSHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SHARED_TO_LOCAL
      {
	QualType argTypes[] = { upcr_shared_ptr_t };
	UPCR_SHARED_TO_LOCAL = CreateFunction(Context, "UPCR_SHARED_TO_LOCAL", Context.VoidPtrTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PSHARED_TO_LOCAL
      {
	QualType argTypes[] = { upcr_pshared_ptr_t };
	UPCR_PSHARED_TO_LOCAL = CreateFunction(Context, "UPCR_PSHARED_TO_LOCAL", Context.VoidPtrTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISNULL_SHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t };
	UPCR_ISNULL_SHARED = CreateFunction(Context, "UPCR_ISNULL_SHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_ISNULL_PSHARED
      {
	QualType argTypes[] = { upcr_pshared_ptr_t };
	UPCR_ISNULL_PSHARED = CreateFunction(Context, "UPCR_ISNULL_PSHARED", Context.IntTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SHARED_TO_PSHARED
      {
	QualType argTypes[] = { upcr_shared_ptr_t };
	UPCR_SHARED_TO_PSHARED = CreateFunction(Context, "UPCR_SHARED_TO_PSHARED", upcr_pshared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_PSHARED_TO_SHARED
      {
	QualType argTypes[] = { upcr_pshared_ptr_t };
	UPCR_PSHARED_TO_SHARED = CreateFunction(Context, "UPCR_PSHARED_TO_SHARED", upcr_shared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_SHARED_RESETPHASE
      {
	QualType argTypes[] = { upcr_shared_ptr_t };
	UPCR_SHARED_RESETPHASE = CreateFunction(Context, "UPCR_SHARED_RESETPHASE", upcr_shared_ptr_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCR_BEGIN_FUNCTION
      {
	UPCR_BEGIN_FUNCTION = CreateFunction(Context, "UPCR_BEGIN_FUNCTION", Context.VoidTy, NULL, 0);
      }
      // UPCRT_STARTUP_PSHALLOC
      {
	QualType argTypes[] = { upcr_pshared_ptr_t, Context.IntTy, Context.IntTy, Context.IntTy, Context.IntTy, Context. getPointerType(Context.getConstType(Context.CharTy)) };
	UPCRT_STARTUP_PSHALLOC = CreateFunction(Context, "UPCRT_STARTUP_PSHALLOC", upcr_startup_pshalloc_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // UPCRT_STARTUP_SHALLOC
      {
	QualType argTypes[] = { upcr_shared_ptr_t, Context.IntTy, Context.IntTy, Context.IntTy, Context.IntTy, Context. getPointerType(Context.getConstType(Context.CharTy)) };
	UPCRT_STARTUP_SHALLOC = CreateFunction(Context, "UPCRT_STARTUP_SHALLOC", upcr_startup_shalloc_t, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcr_startup_pshalloc
      {
	QualType argTypes[] = { Context.getPointerType(upcr_startup_pshalloc_t), Context.IntTy };
	upcr_startup_pshalloc = CreateFunction(Context, "upcr_startup_pshalloc", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcr_startup_shalloc
      {
	QualType argTypes[] = { Context.getPointerType(upcr_startup_shalloc_t), Context.IntTy };
	upcr_startup_shalloc = CreateFunction(Context, "upcr_startup_shalloc", Context.VoidTy, argTypes, sizeof(argTypes)/sizeof(argTypes[0]));
      }
      // upcrt_forall_control
      {
	DeclContext *DC = Context.getTranslationUnitDecl();
	upcrt_forall_control = VarDecl::Create(Context, DC, SourceLocation(), SourceLocation(), &Context.Idents.get("upcrt_forall_control"), Context.IntTy, Context.getTrivialTypeSourceInfo(Context.IntTy), SC_Extern);
      }
      // upcr_null_shared
      {
	DeclContext *DC = Context.getTranslationUnitDecl();
	upcr_null_shared = VarDecl::Create(Context, DC, SourceLocation(), SourceLocation(), &Context.Idents.get("upcr_null_shared"), upcr_shared_ptr_t, Context.getTrivialTypeSourceInfo(upcr_shared_ptr_t), SC_Extern);
      }
      // upcr_null_pshared
      {
	DeclContext *DC = Context.getTranslationUnitDecl();
	upcr_null_pshared = VarDecl::Create(Context, DC, SourceLocation(), SourceLocation(), &Context.Idents.get("upcr_null_pshared"), upcr_pshared_ptr_t, Context.getTrivialTypeSourceInfo(upcr_pshared_ptr_t), SC_Extern);
      }

    }
    FunctionDecl *CreateFunction(ASTContext& Context, StringRef name, QualType RetType, QualType * argTypes, int numArgs) {
      DeclContext *DC = Context.getTranslationUnitDecl();
      QualType Ty = Context.getFunctionType(RetType, llvm::makeArrayRef(argTypes, numArgs), FunctionProtoType::ExtProtoInfo());
      FunctionDecl *Result = FunctionDecl::Create(Context, DC, FakeLocation, FakeLocation, DeclarationName(&Context.Idents.get(name)), Ty, Context.getTrivialTypeSourceInfo(Ty), SC_Extern);
      llvm::SmallVector<ParmVarDecl *, 4> Params;
      for(int i = 0; i < numArgs; ++i) {
	Params.push_back(ParmVarDecl::Create(Context, Result, SourceLocation(), SourceLocation(), 0, argTypes[i], 0, SC_None, 0));
	Params[i]->setScopeInfo(0, i);
      }
      Result->setParams(Params);
      return Result;
    }
    QualType CreateTypedefType(ASTContext& Context, StringRef name) {
      return CreateTypedefType(Context, name, Context.IntTy);
    }
    QualType CreateTypedefType(ASTContext& Context, StringRef name, QualType BaseTy) {
      DeclContext *DC = Context.getTranslationUnitDecl();
      TypedefDecl *Typedef = TypedefDecl::Create(Context, DC, SourceLocation(), SourceLocation(), &Context.Idents.get(name), Context.getTrivialTypeSourceInfo(BaseTy));
      return Context.getTypedefType(Typedef);
    }
  };

  class SubstituteType : public clang::TreeTransform<SubstituteType> {
    typedef TreeTransform<SubstituteType> TreeTransformS;
  public:
    SubstituteType(Sema &S, QualType F, QualType T) : TreeTransformS(S), From(F), To(T) {}
    TypeSourceInfo * TransformType(TypeSourceInfo *TI) {
      if(SemaRef.Context.hasSameType(TI->getType(), From)) {
	return SemaRef.Context.getTrivialTypeSourceInfo(To);
      } else {
	return TreeTransformS::TransformType(TI);
      }
    }
    using TreeTransformS::TransformType;
  private:
    QualType From;
    QualType To;
  };

  class RemoveUPCTransform : public clang::TreeTransform<RemoveUPCTransform> {
    typedef TreeTransform<RemoveUPCTransform> TreeTransformUPC;
  public:
    RemoveUPCTransform(Sema& S, UPCRDecls* D, const std::string& fileid)
      : TreeTransformUPC(S), AnonRecordID(0), Decls(D), FileString(fileid) {
      UPCSystemHeaders.insert("upc.h");
      UPCSystemHeaders.insert("upc_bits.h");
      UPCSystemHeaders.insert("upc_castable.h");
      UPCSystemHeaders.insert("upc_castable_bits.h");
      UPCSystemHeaders.insert("upc_collective.h");
      UPCSystemHeaders.insert("upc_collective_bits.h");
      UPCSystemHeaders.insert("upc_io.h");
      UPCSystemHeaders.insert("upc_io_bits.h");
      UPCSystemHeaders.insert("upc_relaxed.h");
      UPCSystemHeaders.insert("upc_strict.h");
      UPCSystemHeaders.insert("upc_tick.h");
      UPCSystemHeaders.insert("bupc_extensions.h");
      UPCSystemHeaders.insert("bupc_atomics.h");
      UPCSystemHeaders.insert("pupc.h");

      UPCHeaderRenames["upc_types.h"] = "upcr_preinclude/upc_types.h";
    }
    bool AlwaysRebuild() { return true; }
    ExprResult BuildParens(Expr * E) {
      return SemaRef.ActOnParenExpr(SourceLocation(), SourceLocation(), E);
    }
    ExprResult BuildComma(Expr * LHS, Expr * RHS) {
      return SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Comma, LHS, RHS);
    }
    // TreeTransform ignores AlwayRebuild for literals
    ExprResult TransformIntegerLiteral(IntegerLiteral *E) {
      return IntegerLiteral::Create(SemaRef.Context, E->getValue(), E->getType(), E->getLocation());
    }
    ExprResult BuildUPCRCall(FunctionDecl *FD, std::vector<Expr*>& args) {
      ExprResult Fn = SemaRef.BuildDeclRefExpr(FD, FD->getType(), VK_LValue, SourceLocation());
      return SemaRef.BuildResolvedCallExpr(Fn.get(), FD, SourceLocation(), args, SourceLocation());
    }
    ExprResult BuildUPCRDeclRef(VarDecl *VD) {
      return SemaRef.BuildDeclRefExpr(VD, VD->getType(), VK_LValue, SourceLocation());
    }
    Expr * CreateSimpleDeclRef(VarDecl *VD) {
      return SemaRef.BuildDeclRefExpr(VD, VD->getType(), VK_LValue, SourceLocation()).get();
    }
    int AnonRecordID;
    IdentifierInfo *getRecordDeclName(IdentifierInfo * OrigName) {
      return OrigName;
    }
    struct ArrayDimensionT {
      ArrayDimensionT(ASTContext& Context) :
	ArrayDimension(Context.getTypeSize(Context.getSizeType()), 1),
	HasThread(false),
	ElementSize(0),
	E(NULL)
      {}
      llvm::APInt ArrayDimension;
      bool HasThread;
      int ElementSize;
      Expr *E;
    };
    ArrayDimensionT GetArrayDimension(QualType Ty) {
      ArrayDimensionT Result(SemaRef.Context);
      QualType ElemTy = Ty.getCanonicalType();
      while(const ArrayType *AT = dyn_cast<ArrayType>(ElemTy.getTypePtr())) {
	if(const ConstantArrayType *CAT = dyn_cast<ConstantArrayType>(AT)) {
	  Result.ArrayDimension *= CAT->getSize();
	} else if(const UPCThreadArrayType *TAT = dyn_cast<UPCThreadArrayType>(AT)) {
	  if(TAT->getThread()) {
	    Result.HasThread = true;
	  }
	  Result.ArrayDimension *= TAT->getSize();
	} else if(const VariableArrayType *VAT = dyn_cast<VariableArrayType>(AT)) {
	  Expr *Val = BuildParens(VAT->getSizeExpr()).get();
	  if(Result.E) {
	    Result.E = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Mul, Result.E, Val).get();
	  } else {
	    Result.E = Val;
	  }
	} else if(isa<IncompleteArrayType>(AT)) {
	  Result.ArrayDimension = 0;
	} else {
	  assert(!"Other array types should not syntax check");
	}
	ElemTy = AT->getElementType();
      }
      Result.ElementSize = SemaRef.Context.getTypeSizeInChars(ElemTy).getQuantity();
      return Result;
    }
    Expr * MaybeAddParensForMultiply(Expr * E) {
      if(isa<ParenExpr>(E) || isa<IntegerLiteral>(E) ||
	 isa<CallExpr>(E))
	return E;
      else
	return BuildParens(E).get();
    }
    ExprResult MaybeAdjustForArray(const ArrayDimensionT & Dims, Expr * E, BinaryOperatorKind Op) {
      if(Dims.ArrayDimension == 1 && !Dims.E && !Dims.HasThread) {
	return SemaRef.Owned(E);
      } else {
	Expr *Dimension = IntegerLiteral::Create(SemaRef.Context, Dims.ArrayDimension, SemaRef.Context.getSizeType(), SourceLocation());
	if(Dims.HasThread) {
	  std::vector<Expr*> args;
	  Expr *Threads = BuildUPCRCall(Decls->upcr_threads, args).get();
	  Dimension = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Mul, Dimension, Threads).get();
	}
	if(Dims.E) {
	  Dimension = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Mul, Dimension, Dims.E).get();
	}
	if(Dims.HasThread || Dims.E) {
	  Dimension = BuildParens(Dimension).get();
	}
	return BuildParens(SemaRef.CreateBuiltinBinOp(SourceLocation(), Op, MaybeAddParensForMultiply(E), Dimension).get());
      }
    }
    StmtResult TransformUPCNotifyStmt(UPCNotifyStmt *S) {
      Expr *ID = S->getIdValue();
      std::vector<Expr*> args;
      if(ID) {
	args.push_back(TransformExpr(ID).get());
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
      } else {
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 1), SemaRef.Context.IntTy, SourceLocation()));
      }
      Stmt *result = BuildUPCRCall(Decls->upcr_notify, args).get();
      return SemaRef.Owned(result);
    }
    StmtResult TransformUPCWaitStmt(UPCWaitStmt *S) {
      Expr *ID = S->getIdValue();
      std::vector<Expr*> args;
      if(ID) {
	args.push_back(TransformExpr(ID).get());
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
      } else {
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 1), SemaRef.Context.IntTy, SourceLocation()));
      }
      Stmt *result = BuildUPCRCall(Decls->upcr_wait, args).get();
      return SemaRef.Owned(result);
    }
    StmtResult TransformUPCBarrierStmt(UPCBarrierStmt *S) {
      Expr *ID = S->getIdValue();
      std::vector<Expr*> args;
      if(ID) {
	args.push_back(TransformExpr(ID).get());
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
      } else {
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 0), SemaRef.Context.IntTy, SourceLocation()));
	args.push_back(IntegerLiteral::Create(
	  SemaRef.Context, APInt(32, 1), SemaRef.Context.IntTy, SourceLocation()));
      }
      Stmt *result = BuildUPCRCall(Decls->upcr_barrier, args).get();
      return SemaRef.Owned(result);
    }
    StmtResult TransformUPCFenceStmt(UPCFenceStmt *S) {
      std::vector<Expr*> args;
      Stmt *result = BuildUPCRCall(Decls->upcr_poll, args).get();
      return SemaRef.Owned(result);
    }
    ExprResult TransformInitializer(Expr *Init, bool CXXDirectInit) {
      if(!Init)
	return SemaRef.Owned(Init);

      // Have to handle this separately, as TreeTransform
      // strips off ImplicitCastExprs in TransformInitializer.
      if(ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(Init)) {
	if((ICE->getCastKind() == CK_LValueToRValue && ICE->getSubExpr()->getType().getQualifiers().hasShared()) ||
	   isPointerToShared(ICE->getSubExpr()->getType())) {
	  return TransformExpr(ICE);
	} else {
	  ExprResult UPCCast = MaybeTransformUPCRCast(ICE);
	  if(!UPCCast.isInvalid())
	    return UPCCast;
	  return TransformInitializer(ICE->getSubExpr(), CXXDirectInit);
	}
      }

      return TreeTransformUPC::TransformInitializer(Init, CXXDirectInit);
    }
    ExprResult TransformCStyleCastExpr(CStyleCastExpr *E) {
      ExprResult UPCCast = MaybeTransformUPCRCast(E);
      if(!UPCCast.isInvalid()) {
	return UPCCast;
      } else {
	// The default transform strips off implicit casts
	TypeSourceInfo *Type = TransformType(E->getTypeInfoAsWritten());
	if (!Type)
	  return ExprError();

	ExprResult SubExpr = TransformExpr(E->getSubExpr());
	if (SubExpr.isInvalid())
	  return ExprError();
	
	return RebuildCStyleCastExpr(E->getLParenLoc(),
				     Type,
				     E->getRParenLoc(),
				     SubExpr.get());
      }
    }
    ExprResult TransformImplicitCastExpr(ImplicitCastExpr *E) {
      if(E->getCastKind() == CK_LValueToRValue && E->getSubExpr()->getType().getQualifiers().hasShared()) {
	return BuildUPCRLoad(TransformExpr(E->getSubExpr()).get(), E->getType().getUnqualifiedType(), E->getSubExpr()->getType());
      } else {
	ExprResult UPCCast = MaybeTransformUPCRCast(E);
	if(!UPCCast.isInvalid()) {
	  return UPCCast;
	}
	// We can't use the default transform, because it
	// strips off all implicit casts.  We may need to
	// process the subexpression
	return TransformExpr(E->getSubExpr());
      }
    }
    bool isPointerToShared(QualType Ty) {
      if(const PointerType * PTy = Ty->getAs<PointerType>()) {
	return PTy->getPointeeType().getQualifiers().hasShared();
      } else {
	return false;
      }
    }
    IntegerLiteral *CreateInteger(QualType Ty, int Value) {
      return IntegerLiteral::Create(SemaRef.Context, APInt(SemaRef.Context.getTypeSize(Ty), Value), Ty, SourceLocation());
    }
    ExprResult BuildUPCRLoad(Expr * E, QualType ResultType, QualType Ty) {
      std::pair<Expr *, Expr *> LoadAndVar = BuildUPCRLoadParts(E, ResultType, Ty);
      return BuildParens(BuildComma(LoadAndVar.first, LoadAndVar.second).get());
    }
    // Returns a pair containing the load stmt and a declrefexpr to the
    // temporary variable created.
    std::pair<Expr *, Expr *> BuildUPCRLoadParts(Expr * E, QualType ResultType, QualType Ty) {
      int SizeTypeSize = SemaRef.Context.getTypeSize(SemaRef.Context.getSizeType());
      Qualifiers Quals = Ty.getQualifiers();
      bool Phaseless = isPhaseless(Ty);
      bool Strict = Quals.hasStrict();
      // Select the correct function to call
      FunctionDecl *Accessor;
      if(Phaseless) {
	if(Strict) {
	  Accessor = Decls->UPCR_GET_PSHARED_STRICT;
	} else {
	  Accessor = Decls->UPCR_GET_PSHARED;
	}
      } else {
	if(Strict) {
	  Accessor = Decls->UPCR_GET_SHARED_STRICT;
	} else {
	  Accessor = Decls->UPCR_GET_SHARED;
	}
      }
      VarDecl *TmpVar = CreateTmpVar(TransformType(ResultType));
      // FIXME: Handle other layout qualifiers
      std::vector<Expr*> args;
      args.push_back(SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, CreateSimpleDeclRef(TmpVar)).get());
      args.push_back(E);
      // offset
      args.push_back(IntegerLiteral::Create(SemaRef.Context, APInt(SizeTypeSize, 0), SemaRef.Context.getSizeType(), SourceLocation()));
      // size
      args.push_back(IntegerLiteral::Create(SemaRef.Context, APInt(SizeTypeSize, SemaRef.Context.getTypeSizeInChars(ResultType).getQuantity()), SemaRef.Context.getSizeType(), SourceLocation()));
      Expr *Load = BuildUPCRCall(Accessor, args).get();
      return std::make_pair(Load, CreateSimpleDeclRef(TmpVar));
    }
    ExprResult MaybeTransformUPCRCast(CastExpr *E) {
      if(E->getCastKind() == CK_UPCSharedToLocal) {
	bool Phaseless = isPhaseless(E->getSubExpr()->getType()->getAs<PointerType>()->getPointeeType());
	FunctionDecl *Accessor = Phaseless? Decls->UPCR_PSHARED_TO_LOCAL : Decls->UPCR_SHARED_TO_LOCAL;
	std::vector<Expr*> args;
	args.push_back(TransformExpr(E->getSubExpr()).get());
	ExprResult Result = BuildUPCRCall(Accessor, args);
	TypeSourceInfo *Ty = SemaRef.Context.getTrivialTypeSourceInfo(TransformType(E->getType()));
	return SemaRef.BuildCStyleCastExpr(SourceLocation(), Ty, SourceLocation(), Result.get());
      } else if(E->getCastKind() == CK_NullToPointer && isPointerToShared(E->getType())) {
	bool Phaseless = isPhaseless(E->getType()->getAs<PointerType>()->getPointeeType());
	return BuildUPCRDeclRef(Phaseless? Decls->upcr_null_pshared : Decls->upcr_null_shared);
      } else if((E->getCastKind() == CK_BitCast  ||
		 E->getCastKind() == CK_UPCBitCastZeroPhase) &&
		isPointerToShared(E->getType())) {
	QualType DstPointee = E->getType()->getAs<PointerType>()->getPointeeType();
	QualType SrcPointee = E->getSubExpr()->getType()->getAs<PointerType>()->getPointeeType();
	FunctionDecl *CastFn = 0;
	if(isPhaseless(DstPointee) && !isPhaseless(SrcPointee)) {
	  CastFn = Decls->UPCR_SHARED_TO_PSHARED;
	} else if(!isPhaseless(DstPointee) && isPhaseless(SrcPointee)) {
	  CastFn = Decls->UPCR_PSHARED_TO_SHARED;
	} else if(!isPhaseless(DstPointee) && !isPhaseless(SrcPointee) &&
		  E->getCastKind() == CK_UPCBitCastZeroPhase) {
	  CastFn = Decls->UPCR_SHARED_RESETPHASE;
	}
	if(CastFn) {
	  std::vector<Expr *> args;
	  args.push_back(TransformExpr(E->getSubExpr()).get());
	  return BuildUPCRCall(CastFn, args);
	} else {
	  return TransformExpr(E->getSubExpr());
	}
      } else if(E->getCastKind() == CK_PointerToIntegral &&
		isPointerToShared(E->getSubExpr()->getType())) {
	// create temporary
	// memcpy
	// load
	QualType SrcType = TransformType(E->getSubExpr()->getType());
	QualType DstType = TransformType(E->getType());
	CharUnits SrcSize = SemaRef.Context.getTypeSizeInChars(E->getSubExpr()->getType());
	CharUnits DstSize = SemaRef.Context.getTypeSizeInChars(E->getType());
	if(SrcSize < DstSize) {
	  VarDecl *Dst = CreateTmpVar(DstType);
	  Expr * DstVal = CreateSimpleDeclRef(Dst);
	  Expr * DstAddr = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, DstVal).get();
	  SemaRef.AddInitializerToDecl(Dst, CreateInteger(SemaRef.Context.IntTy, 0), false, false);
	  Expr * Target = SemaRef.BuildCStyleCastExpr(SourceLocation(), SemaRef.Context.getTrivialTypeSourceInfo(SemaRef.Context.getPointerType(SrcType)), SourceLocation(), DstAddr).get();
	  Expr * Deref = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, Target).get();
	  Expr * Assign = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, Deref, TransformExpr(E->getSubExpr()).get()).get();
	  return BuildParens(BuildComma(Assign, DstVal).get()).get();
	} else {
	  VarDecl *Src = CreateTmpVar(TransformType(E->getSubExpr()->getType()));
	  Expr * SrcVal = CreateSimpleDeclRef(Src);
	  Expr * SrcAddr = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, SrcVal).get();
	  Expr * SetVal = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, SrcVal, TransformExpr(E->getSubExpr()).get()).get();
	  Expr * Result = SemaRef.BuildCStyleCastExpr(SourceLocation(), SemaRef.Context.getTrivialTypeSourceInfo(SemaRef.Context.getPointerType(DstType)), SourceLocation(), SrcAddr).get();
	  Expr * Deref = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, Result).get();
	  return BuildParens(BuildComma(SetVal, Deref).get()).get();
	}
      }
      return ExprError();
    }
    ExprResult BuildUPCRStore(Expr * LHS, Expr * RHS, QualType Ty, bool ReturnValue = true) {
      int SizeTypeSize = SemaRef.Context.getTypeSize(SemaRef.Context.getSizeType());
      Qualifiers Quals = Ty.getQualifiers(); 
      bool Phaseless = isPhaseless(Ty);
      bool Strict = Quals.hasStrict();
      // Select the correct function to call
      FunctionDecl *Accessor;
      if(Phaseless) {
	if(Strict) {
	  Accessor = Decls->UPCR_PUT_PSHARED_STRICT;
	} else {
	  Accessor = Decls->UPCR_PUT_PSHARED;
	}
      } else {
	if(Strict) {
	  Accessor = Decls->UPCR_PUT_SHARED_STRICT;
	} else {
	  Accessor = Decls->UPCR_PUT_SHARED;
	}
      }
      VarDecl *TmpVar = CreateTmpVar(TransformType(Ty).getUnqualifiedType());
      Expr *SetTmp = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, CreateSimpleDeclRef(TmpVar), RHS).get();
      std::vector<Expr*> args;
      args.push_back(LHS);
      // offset
      args.push_back(IntegerLiteral::Create(SemaRef.Context, APInt(SizeTypeSize, 0), SemaRef.Context.getSizeType(), SourceLocation()));
      args.push_back(SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, CreateSimpleDeclRef(TmpVar)).get());
      // size
      args.push_back(IntegerLiteral::Create(SemaRef.Context, APInt(SizeTypeSize, SemaRef.Context.getTypeSizeInChars(Ty).getQuantity()), SemaRef.Context.getSizeType(), SourceLocation()));
      Expr *Store = BuildUPCRCall(Accessor, args).get();
      Expr *CommaRHS = Store;
      if(ReturnValue) {
	CommaRHS = BuildComma(Store, CreateSimpleDeclRef(TmpVar)).get();
      }
      return BuildParens(BuildComma(SetTmp, CommaRHS).get());
    }
    ExprResult CreateUPCPointerArithmetic(Expr *Ptr, Expr *IntVal, QualType PtrTy) {
      QualType PointeeType = PtrTy->getAs<PointerType>()->getPointeeType();
      ArrayDimensionT Dims = GetArrayDimension(PointeeType);
      int ElementSize = Dims.ElementSize;
      IntVal = MaybeAdjustForArray(Dims, IntVal, BO_Mul).get();
      std::vector<Expr*> args;
      args.push_back(Ptr);
      args.push_back(CreateInteger(SemaRef.Context.getSizeType(), ElementSize));
      args.push_back(IntVal);
      int LayoutQualifier = PointeeType.getQualifiers().getLayoutQualifier();
      if(LayoutQualifier == 0) {
	return BuildUPCRCall(Decls->UPCR_ADD_PSHAREDI, args);
      } else if(isPhaseless(PointeeType) && LayoutQualifier == 1) {
	return BuildUPCRCall(Decls->UPCR_ADD_PSHARED1, args);
      } else {
	args.push_back(CreateInteger(SemaRef.Context.getSizeType(), LayoutQualifier));
	return BuildUPCRCall(Decls->UPCR_ADD_SHARED, args);
      }
    }
    ExprResult CreateArithmeticExpr(Expr *LHS, Expr *RHS, QualType LHSTy, BinaryOperatorKind Op) {
      if(isPointerToShared(LHSTy)) {
	if(Op == BO_Sub) {
	  RHS = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Minus, RHS).get();
	}
	return CreateUPCPointerArithmetic(LHS, RHS, LHSTy);
      } else {
	return SemaRef.CreateBuiltinBinOp(SourceLocation(), Op, LHS, RHS);
      }
    }
    ExprResult TransformUnaryOperator(UnaryOperator *E) {
      QualType ArgType = E->getSubExpr()->getType();
      if((E->getOpcode() == UO_Deref && isPointerToShared(ArgType)) ||
	 (E->getOpcode() == UO_AddrOf && isPointerToShared(E->getType()))) {
	// Strip off * and &.  shared lvalues and pointers-to-shared
	// have the same representation.
	return TransformExpr(E->getSubExpr());
      } else if(ArgType.getQualifiers().hasShared() && E->isIncrementDecrementOp()) {
	bool Phaseless = isPhaseless(ArgType);
	QualType PtrType = Phaseless? Decls->upcr_pshared_ptr_t : Decls->upcr_shared_ptr_t;
	VarDecl * TmpPtrDecl = CreateTmpVar(PtrType);
	Expr * TmpPtr = SemaRef.BuildDeclRefExpr(TmpPtrDecl, PtrType, VK_LValue, SourceLocation()).get();
	Expr * SaveArg = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpPtr, BuildParens(TransformExpr(E->getSubExpr()).get()).get()).get();
	std::pair<Expr *, Expr *> Load = BuildUPCRLoadParts(TmpPtr, ArgType.getUnqualifiedType(), ArgType);
	Expr * LoadExpr = Load.first;
	Expr * LoadVar = Load.second;
	Expr * NewVal = CreateArithmeticExpr(LoadVar, CreateInteger(SemaRef.Context.IntTy, 1), ArgType, E->isIncrementOp()?BO_Add:BO_Sub).get();

	if(E->isPrefix()) {
	  Expr * Result = BuildUPCRStore(TmpPtr, NewVal, ArgType).get();
	  return BuildParens(BuildComma(SaveArg, BuildComma(LoadExpr, Result).get()).get());
	} else {
	  Expr * Result = BuildUPCRStore(TmpPtr, NewVal, ArgType, false).get();
	  return BuildParens(BuildComma(SaveArg, BuildComma(LoadExpr, BuildComma(Result, LoadVar).get()).get()).get());
	}
      } else if(isPointerToShared(ArgType) && E->isIncrementDecrementOp()) {
	QualType TmpPtrType = SemaRef.Context.getPointerType(TransformType(ArgType));
	VarDecl * TmpPtrDecl = CreateTmpVar(TmpPtrType);
	Expr * TmpPtr = CreateSimpleDeclRef(TmpPtrDecl);
        Expr * Setup = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpPtr, SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, TransformExpr(E->getSubExpr()).get()).get()).get();
	Expr * Access = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, TmpPtr).get();

	Expr * Saved;
	Expr * TmpVal;
	if(E->isPostfix()) {
	  // Save the old value
	  VarDecl * TmpValDecl = CreateTmpVar(TransformType(ArgType).getUnqualifiedType());
	  TmpVal = CreateSimpleDeclRef(TmpValDecl);
	  Saved = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpVal, Access).get();
	}

	Expr * NewVal = CreateArithmeticExpr(Access, CreateInteger(SemaRef.Context.IntTy, 1),
					     ArgType, E->isIncrementOp()?BO_Add:BO_Sub).get();
	Expr * Operation = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, Access, NewVal).get();

	if(E->isPrefix()) {
	  return BuildParens(BuildComma(Setup, BuildComma(Operation, Access).get()).get());
	} else {
	  return BuildParens(BuildComma(Setup, BuildComma(Saved, BuildComma(Operation, TmpVal).get()).get()).get());
	}
      } else if(isPointerToShared(ArgType) && E->getOpcode() == UO_LNot) {
	bool Phaseless = isPhaseless(ArgType->getAs<PointerType>()->getPointeeType());
	std::vector<Expr*> args;
	args.push_back(TransformExpr(E->getSubExpr()).get());
	return BuildUPCRCall(Phaseless?Decls->UPCR_ISNULL_PSHARED:Decls->UPCR_ISNULL_SHARED, args);
      } else {
	return TreeTransformUPC::TransformUnaryOperator(E);
      }
    }
    ExprResult TransformBinaryOperator(BinaryOperator *E) {
      // Catch assignment to shared variables
      if(E->getOpcode() == BO_Assign && E->getLHS()->getType().getQualifiers().hasShared()) {
	Expr *LHS = TransformExpr(E->getLHS()).get();
	Expr *RHS = TransformExpr(E->getRHS()).get();
	return BuildUPCRStore(LHS, RHS, E->getLHS()->getType());
      } else {
	Expr *LHS = E->getLHS();
	Expr *RHS = E->getRHS();
	bool LHSIsShared = isPointerToShared(E->getLHS()->getType());
	bool RHSIsShared = isPointerToShared(E->getRHS()->getType());
	if(LHSIsShared && RHSIsShared && E->getOpcode() == BO_Sub) {
	  // Pointer - Pointer
	  ExprResult Result;
	  QualType PointeeType = LHS->getType()->getAs<PointerType>()->getPointeeType();
	  ArrayDimensionT Dims = GetArrayDimension(PointeeType);
	  int ElementSize = Dims.ElementSize;
	  std::vector<Expr*> args;
	  args.push_back(TransformExpr(LHS).get());
	  args.push_back(TransformExpr(RHS).get());
	  args.push_back(CreateInteger(SemaRef.Context.getSizeType(), ElementSize));
	  int LayoutQualifier = PointeeType.getQualifiers().getLayoutQualifier();
	  if(LayoutQualifier == 0) {
	    Result = BuildUPCRCall(Decls->UPCR_SUB_PSHAREDI, args);
	  } else if(isPhaseless(PointeeType) && LayoutQualifier == 1) {
	    Result = BuildUPCRCall(Decls->UPCR_SUB_PSHARED1, args);
	  } else {
	    args.push_back(CreateInteger(SemaRef.Context.getSizeType(), LayoutQualifier));
	    Result = BuildUPCRCall(Decls->UPCR_SUB_SHARED, args);
	  }
	  return MaybeAdjustForArray(Dims, Result.get(), BO_Div);
	} else if((LHSIsShared || RHSIsShared) && (E->getOpcode() == BO_Add || E->getOpcode() == BO_Sub)) {
	  // Pointer +/- Integer
	  if(RHSIsShared) { std::swap(LHS, RHS); }
	  Expr *IntVal = TransformExpr(RHS).get();
	  if(E->getOpcode() == BO_Sub) {
	    IntVal = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Minus, IntVal).get();
	  }
	  return CreateUPCPointerArithmetic(TransformExpr(LHS).get(), IntVal, LHS->getType());
	} else if(LHSIsShared && RHSIsShared && (E->getOpcode() == BO_EQ || E->getOpcode() == BO_NE)) {
	  // Equality Comparison
	  std::vector<Expr*> args;
	  args.push_back(TransformExpr(LHS).get());
	  args.push_back(TransformExpr(RHS).get());
	  QualType LHSPointee = LHS->getType()->getAs<PointerType>()->getPointeeType();
	  QualType RHSPointee = RHS->getType()->getAs<PointerType>()->getPointeeType();
	  ExprResult Result;
	  if(isPhaseless(LHSPointee) && isPhaseless(RHSPointee)) {
	    Result = BuildUPCRCall(Decls->UPCR_ISEQUAL_PSHARED_PSHARED, args);
	  } else if(isPhaseless(LHSPointee) && !isPhaseless(RHSPointee)) {
	    Result = BuildUPCRCall(Decls->UPCR_ISEQUAL_PSHARED_SHARED, args);
	  } else if(!isPhaseless(LHSPointee) && isPhaseless(RHSPointee)) {
	    Result = BuildUPCRCall(Decls->UPCR_ISEQUAL_SHARED_PSHARED, args);
	  } else if(!isPhaseless(LHSPointee) && !isPhaseless(RHSPointee)) {
	    Result = BuildUPCRCall(Decls->UPCR_ISEQUAL_SHARED_SHARED, args);
	  }
	  if(E->getOpcode() == BO_NE) {
	    Result = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_LNot, Result.get());
	  }
	  return Result;
	} else if(LHSIsShared && RHSIsShared && (E->getOpcode() == BO_LT || E->getOpcode() == BO_LE || E->getOpcode() == BO_GT || E->getOpcode() == BO_GE)) {
	  // Relational Comparison
	  QualType PointeeType = LHS->getType()->getAs<PointerType>()->getPointeeType();
	  int ElementSize = SemaRef.Context.getTypeSizeInChars(PointeeType).getQuantity();
	  std::vector<Expr*> args;
	  args.push_back(TransformExpr(LHS).get());
	  args.push_back(TransformExpr(RHS).get());
	  args.push_back(CreateInteger(SemaRef.Context.getSizeType(), ElementSize));
	  int LayoutQualifier = PointeeType.getQualifiers().getLayoutQualifier();
	  Expr *Diff;
	  if(LayoutQualifier == 0) {
	    Diff = BuildUPCRCall(Decls->UPCR_SUB_PSHAREDI, args).get();
	  } else if(LayoutQualifier == 1) {
	    Diff = BuildUPCRCall(Decls->UPCR_SUB_PSHARED1, args).get();
	  } else {
	    args.push_back(CreateInteger(SemaRef.Context.getSizeType(), LayoutQualifier));
	    Diff = BuildUPCRCall(Decls->UPCR_SUB_SHARED, args).get();
	  }
	  return SemaRef.CreateBuiltinBinOp(SourceLocation(), E->getOpcode(), Diff, CreateInteger(SemaRef.Context.IntTy, 0));
	}
      }
      // Otherwise use the default transform
      return TreeTransformUPC::TransformBinaryOperator(E);
    }
    ExprResult TransformCompoundAssignOperator(CompoundAssignOperator *E) {
      if(E->getLHS()->getType().getQualifiers().hasShared()) {
	QualType Ty = E->getLHS()->getType();
	bool Phaseless = isPhaseless(Ty);
	QualType PtrType = Phaseless? Decls->upcr_pshared_ptr_t : Decls->upcr_shared_ptr_t;
	VarDecl * TmpPtrDecl = CreateTmpVar(PtrType);
	BinaryOperatorKind Opc = BinaryOperator::getOpForCompoundAssignment(E->getOpcode());
	Expr * TmpPtr = SemaRef.BuildDeclRefExpr(TmpPtrDecl, PtrType, VK_LValue, SourceLocation()).get();
	Expr * SaveLHS = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpPtr, BuildParens(TransformExpr(E->getLHS()).get()).get()).get();
	Expr * RHS = BuildParens(TransformExpr(E->getRHS()).get()).get();
	Expr * LHSVal = BuildUPCRLoad(TmpPtr, Ty.getUnqualifiedType(), Ty).get();
	Expr * OpResult = CreateArithmeticExpr(LHSVal, RHS, Ty, Opc).get();
	Expr * Result = BuildUPCRStore(TmpPtr, OpResult, Ty).get();
	return BuildParens(BuildComma(SaveLHS, Result).get());
      }	else if(isPointerToShared(E->getLHS()->getType())) {
	QualType Ty = E->getLHS()->getType();
	BinaryOperatorKind Opc = BinaryOperator::getOpForCompoundAssignment(E->getOpcode());
	QualType PtrType = SemaRef.Context.getPointerType(TransformType(Ty));
	VarDecl * TmpPtrDecl = CreateTmpVar(PtrType);
	Expr * TmpPtr = CreateSimpleDeclRef(TmpPtrDecl);
	Expr * LHSPtr = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, BuildParens(TransformExpr(E->getLHS()).get()).get()).get();
	Expr * SetPtr = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpPtr,
						   LHSPtr).get();
	Expr * TmpVar = SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, TmpPtr).get();
	Expr * OpResult = CreateArithmeticExpr(TmpVar, TransformExpr(E->getRHS()).get(), Ty, Opc).get();
	Expr * Result = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, TmpVar, OpResult).get();
	return BuildParens(BuildComma(SetPtr, Result).get());
      } else {
	return TreeTransformUPC::TransformCompoundAssignOperator(E);
      }
    }
    ExprResult TransformArraySubscriptExpr(ArraySubscriptExpr *E) {
      if(isPointerToShared(E->getBase()->getType())) {
	Expr *LHS = E->getBase();
	Expr *RHS = E->getIdx();
	QualType PointeeType = LHS->getType()->getAs<PointerType>()->getPointeeType();
	ArrayDimensionT Dims = GetArrayDimension(PointeeType);
	int ElementSize = Dims.ElementSize;
	Expr *IntVal = TransformExpr(RHS).get();
	IntVal = MaybeAdjustForArray(Dims, IntVal, BO_Mul).get();
	std::vector<Expr*> args;
	args.push_back(TransformExpr(LHS).get());
	args.push_back(CreateInteger(SemaRef.Context.getSizeType(), ElementSize));
	args.push_back(IntVal);
	int LayoutQualifier = PointeeType.getQualifiers().getLayoutQualifier();
	if(LayoutQualifier == 0) {
	  return BuildUPCRCall(Decls->UPCR_ADD_PSHAREDI, args);
	} else if(LayoutQualifier == 1) {
	  return BuildUPCRCall(Decls->UPCR_ADD_PSHARED1, args);
	} else {
	  args.push_back(CreateInteger(SemaRef.Context.getSizeType(), LayoutQualifier));
	  return BuildUPCRCall(Decls->UPCR_ADD_SHARED, args);
	}
      } else {
	return TreeTransformUPC::TransformArraySubscriptExpr(E);
      }
    }
    ExprResult TransformMemberExpr(MemberExpr *E) {
      Expr *Base = E->getBase();
      QualType BaseType = Base->getType();
      if(const PointerType *PT = BaseType->getAs<PointerType>()) {
	BaseType = PT->getPointeeType();
      }
      if(BaseType.getQualifiers().hasShared()) {
	ValueDecl * FD = E->getMemberDecl();
	Expr *NewBase = TransformExpr(Base).get();
	if(!isPhaseless(BaseType)) {
	  std::vector<Expr*> args;
	  args.push_back(NewBase);
	  NewBase = BuildUPCRCall(Decls->UPCR_SHARED_TO_PSHARED, args).get();
	}
	CharUnits Offset = SemaRef.Context.toCharUnitsFromBits(SemaRef.Context.getFieldOffset(FD));
	std::vector<Expr *> args;
	args.push_back(NewBase);
	args.push_back(CreateInteger(SemaRef.Context.getSizeType(), 1));
	args.push_back(CreateInteger(SemaRef.Context.getSizeType(), Offset.getQuantity()));
	return BuildUPCRCall(Decls->UPCR_ADD_PSHAREDI, args);
      } else {
	return TreeTransformUPC::TransformMemberExpr(E);
      }
    }
    ExprResult TransformUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *E) {
      switch(E->getKind()) {
      case UETT_UPC_LocalSizeOf:
      case UETT_UPC_BlockSizeOf:
      case UETT_UPC_ElemSizeOf:
	{
	llvm::APSInt Value;
	SemaRef.VerifyIntegerConstantExpression(E, &Value);
	return SemaRef.Owned(IntegerLiteral::Create(SemaRef.Context, Value, E->getType(), SourceLocation()));
	}
      case UETT_SizeOf:
	{
	  // shared types can be changed by the transformation
	  // we need to calculate this up front.
	  if(E->getTypeOfArgument().getQualifiers().hasShared()) {
	    ArrayDimensionT Dims = GetArrayDimension(E->getTypeOfArgument());
	    int ElementSize = Dims.ElementSize;
	    Expr *IntVal = CreateInteger(SemaRef.Context.getSizeType(), ElementSize);
	    return MaybeAdjustForArray(Dims, IntVal, BO_Mul);
	  }
	  // fallthrough
	}
      default:
	return TreeTransformUPC::TransformUnaryExprOrTypeTraitExpr(E);
      }
    }
    StmtResult TransformUPCForAllStmt(UPCForAllStmt *S) {
      // Transform the initialization statement
      StmtResult Init = getDerived().TransformStmt(S->getInit());

      // Transform the condition
      ExprResult Cond;
      VarDecl *ConditionVar = 0;
      if (S->getConditionVariable()) {
	ConditionVar
        = cast_or_null<VarDecl>(
                     TransformDefinition(
                                        S->getConditionVariable()->getLocation(),
                                                      S->getConditionVariable()));
      } else {
	Cond = TransformExpr(S->getCond());
	
	if (S->getCond()) {
	  // Convert the condition to a boolean value.
	  ExprResult CondE = getSema().ActOnBooleanCondition(0, S->getForLoc(),
							     Cond.get());
	  
	  Cond = CondE.get();
	}
      }
      
      Sema::FullExprArg FullCond(getSema().MakeFullExpr(Cond.take()));
      
      // Transform the increment
      ExprResult Inc = TransformExpr(S->getInc());
      if (Inc.isInvalid())
	return StmtError();
      
      Sema::FullExprArg FullInc(getSema().MakeFullExpr(Inc.get()));

      // Transform the body
      StmtResult Body = TransformStmt(S->getBody());

      StmtResult PlainFor = SemaRef.ActOnForStmt(S->getForLoc(), S->getLParenLoc(),
						 Init.get(), FullCond, ConditionVar,
						 FullInc, S->getRParenLoc(), Body.get());

      // If the thread affinity is not specified, upc_forall is
      // the same as a for loop.
      if(!S->getAfnty()) {
	return PlainFor;
      }

      ExprResult Afnty = TransformExpr(S->getAfnty());
      ExprResult ThreadTest;
      if(isPointerToShared(S->getAfnty()->getType())) {
	bool Phaseless = isPhaseless(S->getAfnty()->getType()->getAs<PointerType>()->getPointeeType());
	std::vector<Expr*> args;
	args.push_back(Afnty.get());
	ThreadTest = BuildUPCRCall(Phaseless?Decls->upcr_hasMyAffinity_pshared:Decls->upcr_hasMyAffinity_shared, args);
      } else {
	std::vector<Expr*> args;
	Expr * Affinity = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Rem, BuildParens(Afnty.get()).get(), BuildUPCRCall(Decls->upcr_threads, args).get()).get();
	ThreadTest = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_EQ, Affinity, BuildUPCRCall(Decls->upcr_mythread, args).get());
      }

      StmtResult UPCBody = SemaRef.ActOnIfStmt(SourceLocation(), SemaRef.MakeFullExpr(ThreadTest.get()), NULL, Body.get(), SourceLocation(), NULL);

      StmtResult UPCFor = SemaRef.ActOnForStmt(S->getForLoc(), S->getLParenLoc(),
						 Init.get(), FullCond, ConditionVar,
						 FullInc, S->getRParenLoc(), UPCBody.get());

      StmtResult UPCForWrapper;
      {
	Sema::CompoundScopeRAII BodyScope(SemaRef);
	SmallVector<Stmt*, 8> Statements;
	Statements.push_back(SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, BuildUPCRDeclRef(Decls->upcrt_forall_control).get(), CreateInteger(SemaRef.Context.IntTy, 1)).get());
	Statements.push_back(UPCFor.get());
	Statements.push_back(SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, BuildUPCRDeclRef(Decls->upcrt_forall_control).get(), CreateInteger(SemaRef.Context.IntTy, 0)).get());

	UPCForWrapper = SemaRef.ActOnCompoundStmt(SourceLocation(), SourceLocation(), Statements, false);
      }

      return SemaRef.ActOnIfStmt(SourceLocation(), SemaRef.MakeFullExpr(BuildUPCRDeclRef(Decls->upcrt_forall_control).get()), NULL, PlainFor.get(), SourceLocation(), UPCForWrapper.get());
    }
    ExprResult TransformCondition(Expr *E) {
      ExprResult Result = TransformExpr(E);
      if(isPointerToShared(E->getType())) {
	bool Phaseless = isPhaseless(E->getType()->getAs<PointerType>()->getPointeeType());
	std::vector<Expr*> args;
	args.push_back(Result.get());
	ExprResult Test = BuildUPCRCall(Phaseless?Decls->UPCR_ISNULL_PSHARED:Decls->UPCR_ISNULL_SHARED, args);
	return SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_LNot, Test.get());
      } else {
	return Result;
      }
    }
    StmtResult TransformIfStmt(IfStmt *S) {
      // Transform the condition
      ExprResult Cond;
      VarDecl *ConditionVar = 0;
      if (S->getConditionVariable()) {
	ConditionVar
	  = cast_or_null<VarDecl>(
                       getDerived().TransformDefinition(
                                          S->getConditionVariable()->getLocation(),
                                                        S->getConditionVariable()));
	if (!ConditionVar)
	  return StmtError();
      } else {
	Cond = TransformCondition(S->getCond());
	
	if (Cond.isInvalid())
	  return StmtError();
	
	// Convert the condition to a boolean value.
	if (S->getCond()) {
	  ExprResult CondE = getSema().ActOnBooleanCondition(0, S->getIfLoc(),
							     Cond.get());
	  if (CondE.isInvalid())
	    return StmtError();
	  
	  Cond = CondE.get();
	}
      }
      
      Sema::FullExprArg FullCond(getSema().MakeFullExpr(Cond.take()));
      if (!S->getConditionVariable() && S->getCond() && !FullCond.get())
	return StmtError();
      
      // Transform the "then" branch.
      StmtResult Then = getDerived().TransformStmt(S->getThen());
      if (Then.isInvalid())
	return StmtError();

      // Transform the "else" branch.
      StmtResult Else = getDerived().TransformStmt(S->getElse());
      if (Else.isInvalid())
	return StmtError();

      return getDerived().RebuildIfStmt(S->getIfLoc(), FullCond, ConditionVar,
                                        Then.get(),
                                        S->getElseLoc(), Else.get());

    }
    using TreeTransformUPC::TransformCompoundStmt;
    StmtResult TransformCompoundStmt(CompoundStmt *S,
				     bool IsStmtExpr) {
      Sema::CompoundScopeRAII CompoundScope(getSema());

      bool SubStmtInvalid = false;
      bool SubStmtChanged = false;
      SmallVector<Stmt*, 8> Statements;
      for (CompoundStmt::body_iterator B = S->body_begin(), BEnd = S->body_end();
	   B != BEnd; ++B) {
	StmtResult Result = TransformStmt(*B);
	if (Result.isInvalid()) {
	  // Immediately fail if this was a DeclStmt, since it's very
	  // likely that this will cause problems for future statements.
	  if (isa<DeclStmt>(*B))
	    return StmtError();

	  // Otherwise, just keep processing substatements and fail later.
	  SubStmtInvalid = true;
	  continue;
	}

	SubStmtChanged = SubStmtChanged || Result.get() != *B;

	// Insert extra statments first
	Statements.append(SplitDecls.begin(), SplitDecls.end());
	SplitDecls.clear();

	// Skip NullStmts.  Several transformations
	// can generate them, and they aren't needed.
	if(!Result.isInvalid() && isa<NullStmt>(Result.get()))
	  continue;

	Statements.push_back(Result.takeAs<Stmt>());
      }

      if (SubStmtInvalid)
	return StmtError();

      if (!getDerived().AlwaysRebuild() &&
	  !SubStmtChanged)
	return SemaRef.Owned(S);

      return getDerived().RebuildCompoundStmt(S->getLBracLoc(),
					      Statements,
					      S->getRBracLoc(),
					      IsStmtExpr);
    }
    StmtResult TransformUPCPragmaStmt(UPCPragmaStmt *) {
      // #pragma upc should be stripped out
      return SemaRef.ActOnNullStmt(SourceLocation());
    }
    VarDecl *CreateTmpVar(QualType Ty) {
      int ID = static_cast<int>(LocalTemps.size());
      std::string name = (llvm::Twine("_bupc_spilld") + llvm::Twine(ID)).str();
      VarDecl *TmpVar = VarDecl::Create(SemaRef.Context, SemaRef.getFunctionLevelDeclContext(), SourceLocation(), SourceLocation(), &SemaRef.Context.Idents.get(name), Ty, SemaRef.Context.getTrivialTypeSourceInfo(Ty), SC_None);
      LocalTemps.push_back(TmpVar);
      return TmpVar;
    }
    // Allow decls to be skipped
    StmtResult TransformDeclStmt(DeclStmt *S) {
      SmallVector<Decl *, 4> Decls;
      for (DeclStmt::decl_iterator D = S->decl_begin(), DEnd = S->decl_end();
	   D != DEnd; ++D) {
	Decl *Transformed = TransformDefinition((*D)->getLocation(), *D);
	
	// Split shared struct S {} *value;
	// into shared struct S {}; upcr_pshared_ptr_t value;
	if (Transformed && Decls.size() == 1 &&
	    isa<TagDecl>(Decls[0]) &&
	    isPointerToShared(GetBaseType(getDeclType(Transformed))))
	{
	  SplitDecls.push_back(RebuildDeclStmt(Decls, S->getStartLoc(), S->getEndLoc()).get());
	  Decls.clear();
	}
	
	if(Transformed)
	  Decls.push_back(Transformed);
      }
      
      if(Decls.empty()) {
	return SemaRef.ActOnNullStmt(S->getEndLoc());
      } else {
	return RebuildDeclStmt(Decls, S->getStartLoc(), S->getEndLoc());
      }
    }
    Decl *TransformDecl(SourceLocation Loc, Decl *D) {
      if(D == NULL) return NULL;
      Decl *Result = TreeTransformUPC::TransformDecl(Loc, D);
      if(Result == D) {
	Result = TransformDeclaration(D, SemaRef.CurContext);
      }
      return Result;
    }
    //Decl *TransformDefinition(SourceLocation Loc, Decl *D) {
    //  return TransformDeclaration(D, SemaRef.CurContext);
    //}
    Decl *TransformDeclaration(Decl *D, DeclContext *DC) {
      Decl *Result = TransformDeclarationImpl(D, DC);
      if(Result) {
	if(D->isImplicit())
	  Result->setImplicit();
	transformedLocalDecl(D, Result);
      }
      return Result;
    }
    bool isPhaseless(QualType Pointee) {
      return Pointee.getQualifiers().getLayoutQualifier() <= 1 &&
	!Pointee->isVoidType();
    }
    QualType TransformQualifiedType(TypeLocBuilder &TLB, QualifiedTypeLoc T) {
      
      Qualifiers Quals = T.getType().getLocalQualifiers();

      QualType Result = getDerived().TransformType(TLB, T.getUnqualifiedLoc());
      if (Result.isNull())
	return QualType();

      // Silently suppress qualifiers if the result type can't be qualified.
      // FIXME: this is the right thing for template instantiation, but
      // probably not for other clients.
      if (Result->isFunctionType() || Result->isReferenceType())
	return Result;

      // Suppress restrict on pointers-to-shared
      if (Quals.hasRestrict() && (!Result->isPointerType() ||
				  Result->hasPointerToSharedRepresentation()))
	Quals.removeRestrict();

      if (!Quals.empty()) {
	Result = SemaRef.BuildQualifiedType(Result, T.getBeginLoc(), Quals);
	// BuildQualifiedType might not add qualifiers if they are invalid.
	if (Result.hasLocalQualifiers())
	  TLB.push<QualifiedTypeLoc>(Result);
	// No location information to preserve.
      }

      return Result;
    }
    QualType TransformPointerType(TypeLocBuilder &TLB, PointerTypeLoc TL) {
      if(isPointerToShared(TL.getType())) {
	QualType Result = isPhaseless(TL.getType()->getAs<PointerType>()->getPointeeType())?
	  Decls->upcr_pshared_ptr_t : Decls->upcr_shared_ptr_t;
	TypedefTypeLoc NewT = TLB.push<TypedefTypeLoc>(Result);
	NewT.setNameLoc(SourceLocation());
	return Result;
      } else {
	return TreeTransformUPC::TransformPointerType(TLB, TL);
      }
    }
    QualType TransformDecayedType(TypeLocBuilder &TLB, DecayedTypeLoc TL) {
      // For pointers to shared, we need to ignore the
      // fact that it was written as an array.
      if(isPointerToShared(TL.getType())) {
	QualType Result = isPhaseless(TL.getType()->getAs<PointerType>()->getPointeeType())?
	  Decls->upcr_pshared_ptr_t : Decls->upcr_shared_ptr_t;
	TypedefTypeLoc NewT = TLB.push<TypedefTypeLoc>(Result);
	NewT.setNameLoc(SourceLocation());
	return Result;
      } else {
	return TreeTransformUPC::TransformDecayedType(TLB, TL);
      }
    }
    Decl *TransformDeclarationImpl(Decl *D, DeclContext *DC) {
      if(isa<NamedDecl>(D) && cast<NamedDecl>(D)->getIdentifier() == &SemaRef.Context.Idents.get("__builtin_va_list")) {
	return SemaRef.Context.getBuiltinVaListDecl();
      } else if(TranslationUnitDecl *TUD = dyn_cast<TranslationUnitDecl>(D)) {
	return TransformTranslationUnitDecl(TUD);
      } else if(FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
	DeclarationNameInfo FnName = FD->getNameInfo();
	DeclarationName Name = FnName.getName();
	bool isMain = false;
	if(Name == &SemaRef.Context.Idents.get("main")) {
	  FnName.setName(&SemaRef.Context.Idents.get("user_main"));
	  isMain = true;
	} else if(Name == &SemaRef.Context.Idents.get("__builtin_va_start")) {
	  FnName.setName(&SemaRef.Context.Idents.get("va_start"));
	} else if(Name == &SemaRef.Context.Idents.get("__builtin_va_end")) {
	  FnName.setName(&SemaRef.Context.Idents.get("va_end"));
	} else if(Name == &SemaRef.Context.Idents.get("__builtin_va_copy")) {
	  FnName.setName(&SemaRef.Context.Idents.get("va_copy"));
	}

	TypeSourceInfo * FTSI = FD->getTypeSourceInfo()? TransformType(FD->getTypeSourceInfo()) : 0;
	FunctionDecl *result = FunctionDecl::Create(SemaRef.Context, DC, FD->getLocStart(),
				    FnName, TransformType(FD->getType()),
				    FTSI,
				    FD->isInlineSpecified()?SC_Static:FD->getStorageClass(),
				    false, FD->hasWrittenPrototype(),
				    FD->isConstexpr());
	transformedLocalDecl(D, result);
	// Copy the parameters
	SmallVector<ParmVarDecl *, 2> Parms;
	int i = 0;
	for(FunctionDecl::param_iterator iter = FD->param_begin(), end = FD->param_end(); iter != end; ++iter) {
	  ParmVarDecl *OldParam = *iter;
	  TypeSourceInfo *PTSI = OldParam->getTypeSourceInfo();
	  if(PTSI && PTSI->getType().getQualifiers().hasShared()) {
	    // Make sure that shared array parameters are decayed to pointers
	    PTSI = SemaRef.Context.getTrivialTypeSourceInfo(TransformType(SemaRef.Context.getAdjustedParameterType(PTSI->getType())));
	  } else {
	    PTSI = PTSI?TransformType(PTSI):0;
	  }
	  ParmVarDecl *Param = ParmVarDecl::Create(SemaRef.Context, result, OldParam->getLocStart(),
						   OldParam->getLocation(), OldParam->getIdentifier(),
						   TransformType(OldParam->getType()),
						   PTSI,
						   OldParam->getStorageClass(),
						   TransformExpr(OldParam->getDefaultArg()).get());
	  Param->setScopeInfo(0, i++);
	  Parms.push_back(Param);
	}
	result->setParams(Parms);

	if(FD->doesThisDeclarationHaveABody()) {
	  SemaRef.ActOnStartOfFunctionDef(0, result);
	  Sema::SynthesizedFunctionScope Scope(SemaRef, result);
	  Stmt *FnBody;
	  {
	    Sema::CompoundScopeRAII BodyScope(SemaRef);
	    Stmt *UserBody = TransformStmt(FD->getBody()).get();
	    llvm::SmallVector<Stmt*, 8> Body;
	    {
	      std::vector<Expr*> args;
	      Body.push_back(BuildUPCRCall(Decls->UPCR_BEGIN_FUNCTION, args).get());
	    }
	    // Insert all the temporary variables that we created
	    for(std::vector<VarDecl*>::const_iterator iter = LocalTemps.begin(), end = LocalTemps.end(); iter != end; ++iter) {
	      Decl *decl_arr[] = { *iter };
	      Body.push_back(SemaRef.ActOnDeclStmt(Sema::DeclGroupPtrTy::make(DeclGroupRef::Create(SemaRef.Context, decl_arr, 1)), SourceLocation(), SourceLocation()).get());
	    }
	    LocalTemps.clear();
	    // Insert the user code
	    Body.push_back(UserBody);
	    if(isMain)
	      Body.push_back(SemaRef.ActOnReturnStmt(SourceLocation(), CreateInteger(SemaRef.Context.IntTy, 0)).get());
	    FnBody = SemaRef.ActOnCompoundStmt(SourceLocation(), SourceLocation(), Body, false).get();
	  }
	  SemaRef.ActOnFinishFunctionBody(result, FnBody);
	}
	return result;
      } else if(VarDecl *VD = dyn_cast<VarDecl>(D)) {
	if(VD->getType().getQualifiers().hasShared()) {
	  TranslationUnitDecl *TU = SemaRef.Context.getTranslationUnitDecl();
	  QualType VarType = (isPhaseless(VD->getType())? Decls->upcr_pshared_ptr_t : Decls->upcr_shared_ptr_t );
	  VarDecl *result = VarDecl::Create(SemaRef.Context, TU, VD->getLocStart(),
					    VD->getLocation(), VD->getIdentifier(),
					    VarType, SemaRef.Context.getTrivialTypeSourceInfo(VarType), VD->getStorageClass());
	  transformedLocalDecl(D, result);
	  SharedGlobals.push_back(std::make_pair(result, VD));
	  Qualifiers Quals;
	  QualType RealType = TransformType(SemaRef.Context.getUnqualifiedArrayType(VD->getType(), Quals));
	  QualType Element = SemaRef.Context.getBaseElementType(RealType);
	  if(const ElaboratedType * ET = dyn_cast<ElaboratedType>(Element)) {
	    Element = ET->getNamedType();
	  }
	  // If this was declared using an anonymous struct,
	  // then we need to create a typedef, so that we
	  // can refer to it later.
	  if(const TagType *TT = dyn_cast<TagType>(Element.getTypePtr())) {
	    if(!TT->getDecl()->getIdentifier()) {
	      TypedefDecl *& NewTypedef = ExtraAnonTagDecls[TT->getDecl()];
	      if(NewTypedef == NULL) {
		std::string Name = (Twine("_bupc_anon_struct") + Twine(AnonRecordID++)).str();
		NewTypedef = TypedefDecl::Create(SemaRef.Context, TU,
						 SourceLocation(), SourceLocation(),
						 &SemaRef.Context.Idents.get(Name),
						 SemaRef.Context.getTrivialTypeSourceInfo(Element));
		LocalStatics.push_back(NewTypedef);
	      }

	      SubstituteType Sub(SemaRef, Element, SemaRef.Context.getTypedefType(NewTypedef));
	      RealType = Sub.TransformType(RealType);
	    }
	  }
	  if(Expr *Init = VD->getInit()) {
	    Qualifiers Quals;
	    SharedInitializers.push_back(std::make_pair(result, std::make_pair(TransformExpr(Init).get(), RealType)));
	  }
	  LocalStatics.push_back(result);
	  return NULL;
	} else if(needsDynamicInitializer(VD)) {
	  TranslationUnitDecl *TU = SemaRef.Context.getTranslationUnitDecl();
	  VarDecl *result = VarDecl::Create(SemaRef.Context, TU, VD->getLocStart(), VD->getLocation(), VD->getIdentifier(),
					    TransformType(VD->getType()), TransformType(VD->getTypeSourceInfo()),
					    VD->getStorageClass());
	  transformedLocalDecl(D, result);
	  Expr *Init = VD->getInit();
	  DynamicInitializers.push_back(std::make_pair(result, TransformExpr(Init).get()));
	  LocalStatics.push_back(result);
	  return NULL;
	} else {
	  VarDecl *result = VarDecl::Create(SemaRef.Context, DC, VD->getLocStart(), VD->getLocation(), VD->getIdentifier(),
					    TransformType(VD->getType()), TransformType(VD->getTypeSourceInfo()),
					    VD->getStorageClass());
	  if(Expr *Init = VD->getInit()) {
	    SemaRef.AddInitializerToDecl(result, TransformExpr(Init).get(), VD->isDirectInit(), false);
	  }
	  return result;
	}
      } else if(RecordDecl *RD = dyn_cast<RecordDecl>(D)) {
	IdentifierInfo *Name = getRecordDeclName(RD->getIdentifier());
	RecordDecl *Result = RecordDecl::Create(SemaRef.Context, RD->getTagKind(), DC,
				  RD->getLocStart(), RD->getLocation(),
				  Name, cast_or_null<RecordDecl>(TransformDecl(SourceLocation(), RD->getPreviousDecl())));
	transformedLocalDecl(D, Result);
	SmallVector<Decl *, 4> Fields;
	if(RD->isThisDeclarationADefinition()) {
	  Result->startDefinition();
          Scope CurScope(SemaRef.getCurScope(), Scope::ClassScope|Scope::DeclScope, SemaRef.getDiagnostics());
	  SemaRef.ActOnTagStartDefinition(&CurScope, Result);
	  for(RecordDecl::decl_iterator iter = RD->decls_begin(), end = RD->decls_end(); iter != end; ++iter) {
	    if(FieldDecl *FD = dyn_cast_or_null<FieldDecl>(*iter)) {
	      TypeSourceInfo *DI = FD->getTypeSourceInfo();
	      if(DI) DI = TransformType(DI);
	      FieldDecl *NewFD = SemaRef.CheckFieldDecl(FD->getDeclName(), TransformType(FD->getType()), DI, Result, FD->getLocation(), FD->isMutable(), TransformExpr(FD->getBitWidth()).get(), FD->getInClassInitStyle(), FD->getInnerLocStart(), FD->getAccess(), 0);
	      transformedLocalDecl(FD, NewFD);
	      NewFD->setImplicit(FD->isImplicit());
	      NewFD->setAccess(FD->getAccess());
	      Result->addDecl(NewFD);
	      Fields.push_back(NewFD);
	    } else {
	      // Skip tag forward declarations.  
	      // struct { shared struct A * ptr; }; used to
	      // be translated into struct { struct A; upc_pshared_ptr_t ptr; };
	      // These extra declarations are harmless elsewhere, but they
	      // cause warnings inside structs.
	      if(TagDecl *TD = dyn_cast<TagDecl>(*iter))
		if(!TD->isThisDeclarationADefinition())
		  continue;
	      Result->addDecl(TransformDecl(SourceLocation(), *iter));
	    }
	  }
	  SemaRef.ActOnFields(0, Result->getLocation(), Result, Fields, SourceLocation(), SourceLocation(), 0);
	  SemaRef.ActOnTagFinishDefinition(&CurScope, Result, RD->getRBraceLoc());
	}
	return Result;
      } else if(TypedefDecl *TD = dyn_cast<TypedefDecl>(D)) {
	TypeSourceInfo *Ty;
	if(TD->getUnderlyingType().getQualifiers().hasShared()) {
	  return 0;
	} else {
	  Ty = TransformType(TD->getTypeSourceInfo());
	}
	return TypedefDecl::Create(SemaRef.Context, DC, TD->getLocStart(), TD->getLocation(), TD->getIdentifier(), Ty);
      } else if(EnumDecl *ED = dyn_cast<EnumDecl>(D)) {
	EnumDecl * PrevDecl = 0;
	if(EnumDecl * OrigPrevDecl = ED->getPreviousDecl()) {
	  PrevDecl = cast<EnumDecl>(TransformDecl(SourceLocation(), OrigPrevDecl));
	}

	EnumDecl *Result = EnumDecl::Create(SemaRef.Context, DC, ED->getLocStart(), ED->getLocation(),
					    ED->getIdentifier(), PrevDecl, ED->isScoped(),
					    ED->isScopedUsingClassTag(), ED->isFixed());
	transformedLocalDecl(D, Result);

	if(ED->isThisDeclarationADefinition()) {

	  Result->startDefinition();

	  SmallVector<Decl *, 4> Enumerators;

	  EnumConstantDecl *PrevEnumConstant = 0;
	  for(EnumDecl::enumerator_iterator iter = ED->enumerator_begin(), end = ED->enumerator_end(); iter != end; ++iter) {
	    Expr *Value = 0;
	    if(Expr *OrigValue = iter->getInitExpr()) {
	      Value = TransformExpr(OrigValue).get();
	    }
	    EnumConstantDecl *EnumConstant = SemaRef.CheckEnumConstant(Result, PrevEnumConstant, iter->getLocation(), iter->getIdentifier(), Value);
	    transformedLocalDecl(*iter, EnumConstant);

	    EnumConstant->setAccess(Result->getAccess());
	    Result->addDecl(EnumConstant);
	    Enumerators.push_back(EnumConstant);
	    PrevEnumConstant = EnumConstant;

	  }

	  SemaRef.ActOnEnumBody(Result->getLocation(), SourceLocation(), Result->getRBraceLoc(), Result, Enumerators, 0, 0);
	}

	return Result;
      } else if(LabelDecl *LD = dyn_cast<LabelDecl>(D)) {
	LabelDecl *Result;
	if(LD->isGnuLocal()) {
	  Result = LabelDecl::Create(SemaRef.Context, DC, LD->getLocation(), LD->getIdentifier(), LD->getLocStart());
	} else {
	  Result = LabelDecl::Create(SemaRef.Context, DC, LD->getLocation(), LD->getIdentifier());
	}
	// FIXME: What to do about the statement?
        return Result;
      } else if(isa<EmptyDecl>(D)) {
	return EmptyDecl::Create(SemaRef.Context, DC, D->getLocation());
      } else {
	assert(!"Unknown Decl");
      }
      // Should not get here
      return NULL;
    }
    std::set<StringRef> CollectedIncludes;
    void PrintIncludes(llvm::raw_ostream& OS) {
      for(std::set<StringRef>::iterator iter = CollectedIncludes.begin(), end = CollectedIncludes.end(); iter != end; ++iter) {
	StringRef relativeFilePath = *iter;
	// Test successively larger paths until we
	// find where the header comes from.
	for(StringRef Parent = *iter; !Parent.empty(); Parent = llvm::sys::path::parent_path(Parent)) {
	  const char * start = llvm::sys::path::filename(Parent).begin();
	  StringRef TestFile = StringRef(start, iter->end() - start);
	  const DirectoryLookup *CurDir = NULL;
	  const FileEntry *found = SemaRef.PP.getHeaderSearchInfo().LookupFile(TestFile, true, NULL, CurDir, NULL, NULL, NULL, NULL);
	  if(found) {
	    if(found == SemaRef.SourceMgr.getFileManager().getFile(*iter)) {
	      relativeFilePath = TestFile;
	      break;
	    }
	  }
	}
	// Check whether this header has a special name
	std::map<StringRef, StringRef>::const_iterator pos = UPCHeaderRenames.find(relativeFilePath);
	if(pos != UPCHeaderRenames.end())
	  relativeFilePath = pos->second;
	OS << "#include <" << relativeFilePath << ">\n";
      }
    }
    bool TreatAsCHeader(SourceLocation Loc) {
      if(Loc.isInvalid()) return false;
      SourceManager& SrcManager = SemaRef.Context.getSourceManager();
      if(SrcManager.getFileID(Loc) == SrcManager.getMainFileID()) return false;
      StringRef Name = llvm::sys::path::filename(SrcManager.getFilename(Loc));
      return UPCSystemHeaders.find(Name) == UPCSystemHeaders.end() &&
	SrcManager.isInSystemHeader(Loc);
    }
    std::set<StringRef> UPCSystemHeaders;
    std::map<StringRef, StringRef> UPCHeaderRenames;
    Decl *TransformTranslationUnitDecl(TranslationUnitDecl *D) {
      TranslationUnitDecl *result = SemaRef.Context.getTranslationUnitDecl();
      Scope CurScope(0, Scope::DeclScope, SemaRef.getDiagnostics());
      SemaRef.setCurScope(&CurScope);
      SemaRef.PushDeclContext(&CurScope, result);

      // Process all Decls
      for(DeclContext::decl_iterator iter = D->decls_begin(),
          end = D->decls_end(); iter != end; ++iter) {
	Decl *decl = TransformDeclaration(*iter, result);
	SourceManager& SrcManager = SemaRef.Context.getSourceManager();
	SourceLocation Loc = SrcManager.getExpansionLoc((*iter)->getLocation());
	// Don't output Decls declared in system headers
	if(Loc.isInvalid() || !SrcManager.isInSystemHeader(Loc)) {
	  for(std::vector<Decl*>::const_iterator locals_iter = LocalStatics.begin(), locals_end = LocalStatics.end(); locals_iter != locals_end; ++locals_iter) {
	    if(!(*locals_iter)->isImplicit())
	      result->addDecl(*locals_iter);
	  }
	  if(decl && !decl->isImplicit())
	    result->addDecl(decl);
        } else {
	  if(TreatAsCHeader(Loc)) {
	    // Record the system headers included by user code
	    SourceLocation HeaderLoc;
	    SourceLocation IncludeLoc = Loc;
	    do {
	      HeaderLoc = IncludeLoc;
	      IncludeLoc = SrcManager.getIncludeLoc(SrcManager.getFileID(HeaderLoc));
	    } while(TreatAsCHeader(IncludeLoc));

	    StringRef Name = SrcManager.getFilename(HeaderLoc);
	    if(!Name.empty()) {
	      CollectedIncludes.insert(Name);
	    }
          }
	}
	LocalStatics.clear();
      }

      if(FunctionDecl *Alloc = GetSharedAllocationFunction()) {
	result->addDecl(Alloc);
      }
      if(FunctionDecl *Init = GetSharedInitializationFunction()) {
	result->addDecl(Init);
      }
      SemaRef.setCurScope(0);
      return result;
    }
    std::map<Decl*, TypedefDecl*> ExtraAnonTagDecls;
    std::vector<Stmt*> SplitDecls;
    std::vector<Decl*> LocalStatics;
    UPCRDecls *Decls;
    std::string FileString;
    std::vector<VarDecl*> LocalTemps;
    // The shared variables that need to be initialized
    // all must have type upcr_shared_ptr_t
    // first = upcr_shared_ptr_t, second = original declaration
    // This must be called at the end of the transformation
    // after all variables with static storage duration
    // have been processed
    typedef std::vector<std::pair<VarDecl*, VarDecl*> > SharedGlobalsType;
    std::vector<std::pair<VarDecl*, VarDecl*> > SharedGlobals;
    FunctionDecl* GetSharedAllocationFunction() {
      FunctionDecl *Result = Decls->CreateFunction(SemaRef.Context, "UPCRI_ALLOC_" + FileString, SemaRef.Context.VoidTy, 0, 0);
      SemaRef.ActOnStartOfFunctionDef(0, Result);
      Sema::SynthesizedFunctionScope Scope(SemaRef, Result);
      StmtResult Body;
      {
	Sema::CompoundScopeRAII BodyScope(SemaRef);
	SmallVector<Stmt*, 8> Statements;
	{
	  std::vector<Expr*> args;
	  Statements.push_back(BuildUPCRCall(Decls->UPCR_BEGIN_FUNCTION, args).get());
	}
	int SizeTypeSize = SemaRef.Context.getTypeSize(SemaRef.Context.getSizeType());
	QualType _bupc_info_type = SemaRef.Context.getIncompleteArrayType(Decls->upcr_startup_shalloc_t, ArrayType::Normal, 0);
	QualType _bupc_pinfo_type = SemaRef.Context.getIncompleteArrayType(Decls->upcr_startup_pshalloc_t, ArrayType::Normal, 0);
	SmallVector<Expr*, 8> Initializers;
	SmallVector<Expr*, 8> PInitializers;
	for(SharedGlobalsType::const_iterator iter = SharedGlobals.begin(), end = SharedGlobals.end();
	    iter != end; ++iter) {
	  std::vector<Expr*> args;
	  bool Phaseless = (iter->first->getType() == Decls->upcr_pshared_ptr_t);
	  args.push_back(SemaRef.BuildDeclRefExpr(iter->first, iter->first->getType(), VK_LValue, SourceLocation()).get());
	  int LayoutQualifier = iter->second->getType().getQualifiers().getLayoutQualifier();
	  llvm::APInt ArrayDimension(SizeTypeSize, 1);
	  bool hasThread = false;
	  QualType ElemTy = iter->second->getType().getCanonicalType();
	  while(const ArrayType *AT = dyn_cast<ArrayType>(ElemTy.getTypePtr())) {
	    if(const ConstantArrayType *CAT = dyn_cast<ConstantArrayType>(AT)) {
	      ArrayDimension *= CAT->getSize();
	    } else if(const UPCThreadArrayType *TAT = dyn_cast<UPCThreadArrayType>(AT)) {
	      if(TAT->getThread()) {
		hasThread = true;
	      }
	      ArrayDimension *= TAT->getSize();
	    } else {
	      assert(!"Other array types should not syntax check");
	    }
	    ElemTy = AT->getElementType();
	  }
	  llvm::APInt ElementSize(SizeTypeSize, SemaRef.Context.getTypeSizeInChars(ElemTy).getQuantity());
	  llvm::APInt ElementsInBlock = LayoutQualifier == 0? ArrayDimension : llvm::APInt(SizeTypeSize, LayoutQualifier);
	  llvm::APInt BlockSize = ElementsInBlock * ElementSize;
	  llvm::APInt NumBlocks = LayoutQualifier == 0?
	    llvm::APInt(SizeTypeSize, 1) :
	    (ArrayDimension + LayoutQualifier - 1).udiv(ElementsInBlock);
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, BlockSize, SemaRef.Context.getSizeType(), SourceLocation()));
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, NumBlocks, SemaRef.Context.getSizeType(), SourceLocation()));
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, llvm::APInt(SizeTypeSize, hasThread), SemaRef.Context.getSizeType(), SourceLocation()));
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, ElementSize, SemaRef.Context.getSizeType(), SourceLocation()));
	  // FIXME: encode the correct mangled type
	  args.push_back(StringLiteral::Create(SemaRef.Context, "", StringLiteral::Ascii, false, SemaRef.Context.getPointerType(SemaRef.Context.getConstType(SemaRef.Context.CharTy)), SourceLocation()));
	  if(Phaseless) {
	    PInitializers.push_back(BuildUPCRCall(Decls->UPCRT_STARTUP_PSHALLOC, args).get());
	  } else {
	    Initializers.push_back(BuildUPCRCall(Decls->UPCRT_STARTUP_SHALLOC, args).get());
	  }
	}
	VarDecl *_bupc_info;
	VarDecl *_bupc_pinfo;
	if(!Initializers.empty()) {
	  _bupc_info = VarDecl::Create(SemaRef.Context, Result, SourceLocation(), SourceLocation(),
						 &SemaRef.Context.Idents.get("_bupc_info"), _bupc_info_type, SemaRef.Context.getTrivialTypeSourceInfo(_bupc_info_type), SC_None);
	  // InitializerList semantics vary depending on whether the SourceLocations are valid.
	  SemaRef.AddInitializerToDecl(_bupc_info, SemaRef.ActOnInitList(Decls->FakeLocation, Initializers, Decls->FakeLocation).get(), false, false);
	  Decl *_bupc_info_arr[] = { _bupc_info };
	  Statements.push_back(SemaRef.ActOnDeclStmt(Sema::DeclGroupPtrTy::make(DeclGroupRef::Create(SemaRef.Context, _bupc_info_arr, 1)), SourceLocation(), SourceLocation()).get());
	}
	if(!PInitializers.empty()) {
	  _bupc_pinfo = VarDecl::Create(SemaRef.Context, Result, SourceLocation(), SourceLocation(),
						 &SemaRef.Context.Idents.get("_bupc_pinfo"), _bupc_pinfo_type, SemaRef.Context.getTrivialTypeSourceInfo(_bupc_pinfo_type), SC_None);
	  // InitializerList semantics vary depending on whether the SourceLocations are valid.
	  SemaRef.AddInitializerToDecl(_bupc_pinfo, SemaRef.ActOnInitList(Decls->FakeLocation, PInitializers, Decls->FakeLocation).get(), false, false);
	  Decl *_bupc_pinfo_arr[] = { _bupc_pinfo };
	  Statements.push_back(SemaRef.ActOnDeclStmt(Sema::DeclGroupPtrTy::make(DeclGroupRef::Create(SemaRef.Context, _bupc_pinfo_arr, 1)), SourceLocation(), SourceLocation()).get());
	}
	if(!Initializers.empty()) {
	  std::vector<Expr*> args;
	  args.push_back(SemaRef.BuildDeclRefExpr(_bupc_info, _bupc_info_type, VK_LValue, SourceLocation()).get());
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, llvm::APInt(SizeTypeSize, Initializers.size()), SemaRef.Context.getSizeType(), SourceLocation()));
	  Statements.push_back(BuildUPCRCall(Decls->upcr_startup_shalloc, args).get());
	}
	if(!PInitializers.empty()) {
	  std::vector<Expr*> args;
	  args.push_back(SemaRef.BuildDeclRefExpr(_bupc_pinfo, _bupc_pinfo_type, VK_LValue, SourceLocation()).get());
	  args.push_back(IntegerLiteral::Create(SemaRef.Context, llvm::APInt(SizeTypeSize, PInitializers.size()), SemaRef.Context.getSizeType(), SourceLocation()));
	  Statements.push_back(BuildUPCRCall(Decls->upcr_startup_pshalloc, args).get());
	}
	Body = SemaRef.ActOnCompoundStmt(SourceLocation(), SourceLocation(), Statements, false);
      }
      SemaRef.ActOnFinishFunctionBody(Result, Body.get());
      return Result;
    }

    Stmt *CreateSimpleDeclStmt(Decl * D) {
      Decl *Decls[] = { D };
      return SemaRef.ActOnDeclStmt(Sema::DeclGroupPtrTy::make(DeclGroupRef::Create(SemaRef.Context, Decls, 1)), SourceLocation(), SourceLocation()).get();
    }

    bool needsDynamicInitializer(VarDecl *VD) {
      if(isPointerToShared(VD->getType()) && VD->hasGlobalStorage() && VD->hasInit()) {
	return true;
      } else {
	return false;
      }
    }

    typedef std::vector<std::pair<VarDecl *, Expr *> > DynamicInitializersType;
    DynamicInitializersType DynamicInitializers;
    typedef std::vector<std::pair<VarDecl *, std::pair<Expr *, QualType> > > SharedInitializersType;
    SharedInitializersType SharedInitializers;
    FunctionDecl * GetSharedInitializationFunction() {
      FunctionDecl *Result = Decls->CreateFunction(SemaRef.Context, "UPCRI_INIT_" + FileString, SemaRef.Context.VoidTy, 0, 0);
      SemaRef.ActOnStartOfFunctionDef(0, Result);
      Sema::SynthesizedFunctionScope Scope(SemaRef, Result);
      StmtResult Body;
      {
	Sema::CompoundScopeRAII BodyScope(SemaRef);
	SmallVector<Stmt*, 8> Statements;
	{
	  std::vector<Expr*> args;
	  Statements.push_back(BuildUPCRCall(Decls->UPCR_BEGIN_FUNCTION, args).get());
	}
	
	Expr *Cond;
	{
	  std::vector<Expr*> args;
	  Expr *mythread = BuildUPCRCall(Decls->upcr_mythread, args).get();
	  Cond = SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_EQ, mythread, CreateInteger(SemaRef.Context.IntTy, 0)).get();
	}

	std::vector<VarDecl *> Initializers;
	for(SharedInitializersType::iterator iter = SharedInitializers.begin(), end = SharedInitializers.end(); iter != end; ++iter) {
	  std::string VarName = (Twine("_bupc_") + iter->first->getIdentifier()->getName() + "_val").str();
	  VarDecl *StoredInit = VarDecl::Create(SemaRef.Context, Result, SourceLocation(), SourceLocation(), &SemaRef.Context.Idents.get(VarName),
						iter->second.second, SemaRef.Context.getTrivialTypeSourceInfo(iter->second.second),
						SC_None);
	  StoredInit->setInit(iter->second.first);
	  Initializers.push_back(StoredInit);
	  Statements.push_back(CreateSimpleDeclStmt(StoredInit));
	}
	
	{
	  SmallVector<Stmt*, 8> PutOnce;
	  for(std::size_t i = 0; i < SharedInitializers.size(); ++i) {
	    std::vector<Expr*> args;
	    args.push_back(CreateSimpleDeclRef(SharedInitializers[i].first));
	    args.push_back(CreateInteger(SemaRef.Context.IntTy, 0));
	    args.push_back(SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_AddrOf, CreateSimpleDeclRef(Initializers[i])).get());
	    args.push_back(CreateInteger(SemaRef.Context.IntTy, SemaRef.Context.getTypeSizeInChars(Initializers[i]->getType()).getQuantity()));
	    bool Phaseless = SharedInitializers[i].first->getType() == Decls->upcr_pshared_ptr_t;
	    PutOnce.push_back(BuildUPCRCall(Phaseless?Decls->upcr_put_pshared:Decls->upcr_put_shared, args).get());
	  }
	  Statements.push_back(SemaRef.ActOnIfStmt(SourceLocation(), SemaRef.MakeFullExpr(Cond), NULL, SemaRef.ActOnCompoundStmt(SourceLocation(), SourceLocation(), PutOnce, false).get(), SourceLocation(), NULL).get());
	}
	{
	  for(std::size_t i = 0; i < DynamicInitializers.size(); ++i) {
	    Expr *LHS = CreateSimpleDeclRef(DynamicInitializers[i].first);
	    Expr *RHS = DynamicInitializers[i].second;
	    Statements.push_back(SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, LHS, RHS).get());
	  }
	}

	Body = SemaRef.ActOnCompoundStmt(SourceLocation(), SourceLocation(), Statements, false);
      }
      SemaRef.ActOnFinishFunctionBody(Result, Body.get());
      return Result;
    }
  };

  class RemoveUPCConsumer : public clang::SemaConsumer {
  public:
    RemoveUPCConsumer(StringRef Output, StringRef FileString) : filename(Output), fileid(FileString) {}
    virtual void HandleTranslationUnit(clang::ASTContext &Context) {
      if(Context.getDiagnostics().hasUncompilableErrorOccurred())
	return;

      TranslationUnitDecl *top = Context.getTranslationUnitDecl();
      // Copy the ASTContext and Sema
      LangOptions LangOpts = Context.getLangOpts();
      ASTContext newContext(LangOpts, Context.getSourceManager(), &Context.getTargetInfo(),
			    Context.Idents, Context.Selectors, Context.BuiltinInfo,
			    Context.getTypes().size());
      newContext.getDiagnostics().setIgnoreAllWarnings(true);
      ASTConsumer nullConsumer;
      UPCRDecls Decls(newContext);
      Sema newSema(S->getPreprocessor(), newContext, nullConsumer);
      RemoveUPCTransform Trans(newSema, &Decls, fileid);
      Decl *Result = Trans.TransformTranslationUnitDecl(top);
      std::string error;
      llvm::raw_fd_ostream OS(filename.c_str(), error);
      OS << "#include <upcr.h>\n";
      OS << "#include <upcr_proxy.h>\n";

      Trans.PrintIncludes(OS);

      OS << "#ifndef UPCR_TRANS_EXTRA_INCL\n"
	"#define UPCR_TRANS_EXTRA_INCL\n"
	"#ifndef __builtin_va_arg\n" // subclass of Expr - cannot be renamed directly
	"#define __builtin_va_arg(_a1,_a2) va_arg(_a1,_a2)\n"
	"#endif\n"
	"int32_t UPCR_TLD_DEFINE_TENTATIVE(upcrt_forall_control, 4, 4);\n"
	"#ifndef UPCR_EXIT_FUNCTION\n"
	"#define UPCR_EXIT_FUNCTION() ((void)0)\n"
	"#endif\n"
	"#define UPCRT_STARTUP_SHALLOC(sptr, blockbytes, numblocks, mult_by_threads, elemsz, typestr) \\\n"
	"      { &(sptr), (blockbytes), (numblocks), (mult_by_threads), (elemsz), #sptr, (typestr) }\n"
	"#define UPCRT_STARTUP_PSHALLOC UPCRT_STARTUP_SHALLOC\n"
	"#endif\n";

      Result->print(OS);
    }
    void InitializeSema(Sema& SemaRef) { S = &SemaRef; }
    void ForgetSema() { S = 0; }
  private:
    Sema *S;
    std::string filename;
    std::string fileid;
  };

  class RemoveUPCAction : public clang::ASTFrontendAction {
  public:
    RemoveUPCAction(StringRef OutputFile, StringRef FileString) : filename(OutputFile), fileid(FileString) {}
    virtual clang::ASTConsumer *CreateASTConsumer(clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
      return new RemoveUPCConsumer(filename, fileid);
    }
    std::string filename;
    std::string fileid;
  };

}

int main(int argc, const char ** argv) {
  using namespace llvm::opt;
  using namespace clang::driver;

  // Parse the arguments
  OwningPtr<OptTable> Opts(createDriverOptTable());
  unsigned MissingArgIndex, MissingArgCount;
  OwningPtr<InputArgList> Args(
    Opts->ParseArgs(argv, argv + argc,MissingArgIndex, MissingArgCount));

  // Read the input and output files and adjust the arguments
  std::string InputFile = Args->getLastArgValue(options::OPT_INPUT);
  std::string DefaultOutputFile = (llvm::sys::path::stem(InputFile) + ".trans.c").str();
  std::string OutputFile = Args->getLastArgValue(options::OPT_o, DefaultOutputFile);
  Args->eraseArg(options::OPT_o);

  // Write the arguments to a vector
  ArgStringList NewOptions;
  for(ArgList::const_iterator iter = Args->begin(), end = Args->end(); iter != end; ++iter) {
    // Always parse as UPC
    if((*iter)->getOption().getID() == options::OPT_INPUT &&
       iter != Args->begin()) {
      NewOptions.push_back("-xupc");
    }
    (*iter)->renderAsInput(*Args, NewOptions);
  }
  // Disable CodeGen
  NewOptions.push_back("-fsyntax-only");

  // convert to std::string
  std::vector<std::string> options(NewOptions.begin(), NewOptions.end());

  FileManager * Files(new FileManager(FileSystemOptions()));
  ToolInvocation tool(options, new RemoveUPCAction(OutputFile, get_file_id(InputFile)), Files);
  if(tool.run()) {
    return EXIT_SUCCESS;
  } else {
    return EXIT_FAILURE;
  }
}

//===-- Frontend.cpp - frontend utility methods ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains utility methods for parsing and performing semantic
// on modules.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/Frontend.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Component.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/Module.h"
#include "swift/Parse/Lexer.h"
#include "swift/Subsystems.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"

using namespace swift;

static Identifier getModuleIdentifier(StringRef OutputName,
                                      ASTContext &Context,
                                      TranslationUnit::TUKind moduleKind) {
  StringRef moduleName = OutputName;

  // As a special case, recognize <stdin>.
  if (moduleName == "<stdin>")
    return Context.getIdentifier("stdin");

  // Find the stem of the filename.
  moduleName = llvm::sys::path::stem(moduleName);

  // Complain about non-identifier characters in the module name.
  if (!Lexer::isIdentifier(moduleName)) {
    if (moduleKind == TranslationUnit::Main) {
      moduleName = "main";
    } else {
      SourceLoc Loc;
      Context.Diags.diagnose(Loc, diag::bad_module_name, moduleName);
      moduleName = "bad";
    }
  }

  return Context.getIdentifier(moduleName);
}

/// \param SIL is non-null when we're parsing a .sil file instead of a .swift
/// file.
TranslationUnit*
swift::buildSingleTranslationUnit(ASTContext &Context,
                                  StringRef OutputName,
                                  ArrayRef<unsigned> BufferIDs,
                                  bool ParseOnly,
                                  bool AllowBuiltinModule,
                                  TranslationUnit::TUKind Kind,
                                  SILModule *SIL) {
  Component *Comp = new (Context.Allocate<Component>(1)) Component();
  Identifier ID = getModuleIdentifier(OutputName, Context, Kind);
  TranslationUnit *TU = new (Context) TranslationUnit(ID, Comp, Context, Kind);
  Context.LoadedModules[ID.str()] = TU;

  TU->HasBuiltinModuleAccess = AllowBuiltinModule;

  // If we're in SIL mode, don't auto import any libraries.
  // Also don't perform auto import if we are not going to do semantic
  // analysis.
  if (Kind != TranslationUnit::SIL && !ParseOnly)
    performAutoImport(TU);

  // If we have multiple source files, we must be building a module.  Parse each
  // file before type checking the union of them.
  if (BufferIDs.size() > 1) {
    assert(Kind == TranslationUnit::Library &&
           "Multiple file mode can't handle early returns from the parser");

    // Parse all of the files into one big translation unit.
    for (auto &BufferID : BufferIDs) {
      auto *Buffer = Context.SourceMgr.getMemoryBuffer(BufferID);

      unsigned BufferOffset = 0;
      parseIntoTranslationUnit(TU, BufferID, &BufferOffset);
      assert(BufferOffset == Buffer->getBufferSize() &&
             "Parser returned early?");
      (void)Buffer;
    }

    // Finally, if enabled, type check the whole thing in one go.
    if (!ParseOnly)
      performTypeChecking(TU);
    return TU;
  }

  // If there is only a single input file, it may be SIL or a main module,
  // which requires pumping the parser.
  assert(BufferIDs.size() == 1 && "This mode only allows one input");
  unsigned BufferID = BufferIDs[0];

  SILParserState SILContext(SIL);

  unsigned CurTUElem = 0;
  unsigned BufferOffset = 0;
  auto *Buffer = Context.SourceMgr.getMemoryBuffer(BufferID);
  do {
    // Pump the parser multiple times if necessary.  It will return early
    // after parsing any top level code in a main module, or in SIL mode when
    // there are chunks of swift decls (e.g. imports and types) interspersed
    // with 'sil' definitions.
    parseIntoTranslationUnit(TU, BufferID, &BufferOffset, 0,
                             SIL ? &SILContext : nullptr);
    if (!ParseOnly)
      performTypeChecking(TU, CurTUElem);
    CurTUElem = TU->Decls.size();
  } while (BufferOffset != Buffer->getBufferSize());

  return TU;
}


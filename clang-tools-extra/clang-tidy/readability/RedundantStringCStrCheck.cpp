//===- RedundantStringCStrCheck.cpp - Check for redundant c_str calls -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements a check for redundant calls of c_str() on strings.
//
//===----------------------------------------------------------------------===//

#include "RedundantStringCStrCheck.h"
#include "../utils/FixItHintUtils.h"
#include "../utils/Matchers.h"
#include "../utils/OptionsUtils.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/FixIt.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

namespace {

AST_MATCHER(MaterializeTemporaryExpr, isBoundToLValue) {
  return Node.isBoundToLvalueReference();
}

} // end namespace

RedundantStringCStrCheck::RedundantStringCStrCheck(StringRef Name,
                                                   ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      StringParameterFunctions(utils::options::parseStringList(
          Options.get("StringParameterFunctions", ""))) {
  if (getLangOpts().CPlusPlus20)
    StringParameterFunctions.emplace_back("::std::format");
  if (getLangOpts().CPlusPlus23)
    StringParameterFunctions.emplace_back("::std::print");
}

void RedundantStringCStrCheck::registerMatchers(
    ast_matchers::MatchFinder *Finder) {
  // Match expressions of type 'string' or 'string*'.
  const auto StringDecl = type(hasUnqualifiedDesugaredType(recordType(
      hasDeclaration(cxxRecordDecl(hasName("::std::basic_string"))))));
  const auto StringExpr =
      expr(anyOf(hasType(StringDecl), hasType(qualType(pointsTo(StringDecl)))));

  // Match string constructor.
  const auto StringConstructorExpr = expr(anyOf(
      cxxConstructExpr(argumentCountIs(1),
                       hasDeclaration(cxxMethodDecl(hasName("basic_string")))),
      cxxConstructExpr(argumentCountIs(2),
                       hasDeclaration(cxxMethodDecl(hasName("basic_string"))),
                       // If present, the second argument is the alloc object
                       // which must not be present explicitly.
                       hasArgument(1, cxxDefaultArgExpr()))));

  // Match string constructor.
  const auto StringViewConstructorExpr = cxxConstructExpr(
      argumentCountIs(1),
      hasDeclaration(cxxMethodDecl(hasName("basic_string_view"))));

  // Match a call to the string 'c_str()' method.
  const auto StringCStrCallExpr =
      cxxMemberCallExpr(on(StringExpr.bind("arg")),
                        callee(memberExpr().bind("member")),
                        callee(cxxMethodDecl(hasAnyName("c_str", "data"))))
          .bind("call");
  const auto HasRValueTempParent =
      hasParent(materializeTemporaryExpr(unless(isBoundToLValue())));
  // Detect redundant 'c_str()' calls through a string constructor.
  // If CxxConstructExpr is the part of some CallExpr we need to
  // check that matched ParamDecl of the ancestor CallExpr is not rvalue.
  Finder->addMatcher(
      traverse(
          TK_AsIs,
          cxxConstructExpr(
              anyOf(StringConstructorExpr, StringViewConstructorExpr),
              hasArgument(0, StringCStrCallExpr),
              unless(anyOf(HasRValueTempParent, hasParent(cxxBindTemporaryExpr(
                                                    HasRValueTempParent)))))),
      this);

  // Detect: 's == str.c_str()'  ->  's == str'
  Finder->addMatcher(
      cxxOperatorCallExpr(
          hasAnyOverloadedOperatorName("<", ">", ">=", "<=", "!=", "==", "+"),
          anyOf(allOf(hasArgument(0, StringExpr),
                      hasArgument(1, StringCStrCallExpr)),
                allOf(hasArgument(0, StringCStrCallExpr),
                      hasArgument(1, StringExpr)))),
      this);

  // Detect: 'dst += str.c_str()'  ->  'dst += str'
  // Detect: 's = str.c_str()'  ->  's = str'
  Finder->addMatcher(
      cxxOperatorCallExpr(hasAnyOverloadedOperatorName("=", "+="),
                          hasArgument(0, StringExpr),
                          hasArgument(1, StringCStrCallExpr)),
      this);

  // Detect: 'dst.append(str.c_str())'  ->  'dst.append(str)'
  Finder->addMatcher(
      cxxMemberCallExpr(on(StringExpr),
                        callee(decl(cxxMethodDecl(
                            hasAnyName("append", "assign", "compare")))),
                        argumentCountIs(1), hasArgument(0, StringCStrCallExpr)),
      this);

  // Detect: 'dst.compare(p, n, str.c_str())'  ->  'dst.compare(p, n, str)'
  Finder->addMatcher(
      cxxMemberCallExpr(on(StringExpr),
                        callee(decl(cxxMethodDecl(hasName("compare")))),
                        argumentCountIs(3), hasArgument(2, StringCStrCallExpr)),
      this);

  // Detect: 'dst.find(str.c_str())'  ->  'dst.find(str)'
  Finder->addMatcher(
      cxxMemberCallExpr(on(StringExpr),
                        callee(decl(cxxMethodDecl(hasAnyName(
                            "find", "find_first_not_of", "find_first_of",
                            "find_last_not_of", "find_last_of", "rfind")))),
                        anyOf(argumentCountIs(1), argumentCountIs(2)),
                        hasArgument(0, StringCStrCallExpr)),
      this);

  // Detect: 'dst.insert(pos, str.c_str())'  ->  'dst.insert(pos, str)'
  Finder->addMatcher(
      cxxMemberCallExpr(on(StringExpr),
                        callee(decl(cxxMethodDecl(hasName("insert")))),
                        argumentCountIs(2), hasArgument(1, StringCStrCallExpr)),
      this);

  // Detect redundant 'c_str()' calls through a StringRef constructor.
  Finder->addMatcher(
      traverse(
          TK_AsIs,
          cxxConstructExpr(
              // Implicit constructors of these classes are overloaded
              // wrt. string types and they internally make a StringRef
              // referring to the argument.  Passing a string directly to
              // them is preferred to passing a char pointer.
              hasDeclaration(cxxMethodDecl(hasAnyName(
                  "::llvm::StringRef::StringRef", "::llvm::Twine::Twine"))),
              argumentCountIs(1),
              // The only argument must have the form x.c_str() or p->c_str()
              // where the method is string::c_str().  StringRef also has
              // a constructor from string which is more efficient (avoids
              // strlen), so we can construct StringRef from the string
              // directly.
              hasArgument(0, StringCStrCallExpr))),
      this);

  if (!StringParameterFunctions.empty()) {
    // Detect redundant 'c_str()' calls in parameters passed to std::format in
    // C++20 onwards and std::print in C++23 onwards.
    Finder->addMatcher(
        traverse(TK_AsIs,
                 callExpr(callee(functionDecl(matchers::matchesAnyListedName(
                              StringParameterFunctions))),
                          forEachArgumentWithParam(StringCStrCallExpr,
                                                   parmVarDecl()))),
        this);
  }
}

void RedundantStringCStrCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  const auto *Arg = Result.Nodes.getNodeAs<Expr>("arg");
  const auto *Member = Result.Nodes.getNodeAs<MemberExpr>("member");
  bool Arrow = Member->isArrow();
  // Replace the "call" node with the "arg" node, prefixed with '*'
  // if the call was using '->' rather than '.'.
  std::string ArgText =
      Arrow ? utils::fixit::formatDereference(*Arg, *Result.Context)
            : tooling::fixit::getText(*Arg, *Result.Context).str();
  if (ArgText.empty())
    return;

  diag(Call->getBeginLoc(), "redundant call to %0")
      << Member->getMemberDecl()
      << FixItHint::CreateReplacement(Call->getSourceRange(), ArgText);
}

} // namespace clang::tidy::readability

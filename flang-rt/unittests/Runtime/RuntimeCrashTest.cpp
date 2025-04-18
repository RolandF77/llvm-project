//===-- unittests/Runtime/RuntimeCrashTest.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// Selected APIs are tested here to support development of unit tests for other
/// runtime components and ensure the test fixture handles crashes as we expect.
//
//===----------------------------------------------------------------------===//
#include "CrashHandlerFixture.h"
#include "tools.h"
#include "flang-rt/runtime/terminator.h"
#include "flang/Runtime/io-api.h"
#include "flang/Runtime/transformational.h"
#include <gtest/gtest.h>

using namespace Fortran::runtime;
using namespace Fortran::runtime::io;
using Fortran::common::TypeCategory;

//------------------------------------------------------------------------------
/// Test crashes through direct calls to terminator methods
//------------------------------------------------------------------------------
struct TestTerminator : CrashHandlerFixture {};

#define TEST_CRASH_HANDLER_MESSAGE \
  "Intentionally crashing runtime for unit test"

TEST(TestTerminator, CrashTest) {
  static Fortran::runtime::Terminator t;
  ASSERT_DEATH(t.Crash(TEST_CRASH_HANDLER_MESSAGE), TEST_CRASH_HANDLER_MESSAGE);
}

#undef TEST_CRASH_HANDLER_MESSAGE

TEST(TestTerminator, CheckFailedLocationTest) {
  static Fortran::runtime::Terminator t;
  ASSERT_DEATH(t.CheckFailed("predicate", "someFileName", 789),
      "RUNTIME_CHECK\\(predicate\\) failed at someFileName\\(789\\)");
}

TEST(TestTerminator, CheckFailedTest) {
  static Fortran::runtime::Terminator t("someFileName");
  ASSERT_DEATH(t.CheckFailed("predicate"),
      "RUNTIME_CHECK\\(predicate\\) failed at someFileName\\(0\\)");
}

//------------------------------------------------------------------------------
/// Test misuse of io api
//------------------------------------------------------------------------------
struct TestIOCrash : CrashHandlerFixture {};

TEST(TestIOCrash, InvalidFormatCharacterTest) {
  static constexpr int bufferSize{1};
  static char buffer[bufferSize];
  static const char *format{"(C1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  ASSERT_DEATH(IONAME(OutputInteger64)(cookie, 0xfeedface),
      "Unknown 'C' edit descriptor in FORMAT");
}

//------------------------------------------------------------------------------
/// Test buffer overwrites with Output* functions
/// Each test performs the tested IO operation correctly first, before causing
/// an overwrite to demonstrate that the failure is caused by the overwrite and
/// not a misuse of the API.
//------------------------------------------------------------------------------
TEST(TestIOCrash, OverwriteBufferAsciiTest) {
  static constexpr int bufferSize{4};
  static char buffer[bufferSize];
  static const char *format{"(A4)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputAscii)(cookie, "four", bufferSize);
  ASSERT_DEATH(IONAME(OutputAscii)(cookie, "Too many characters!", 20),
      "Internal write overran available records");
}

TEST(TestIOCrash, OverwriteBufferCharacterTest) {
  static constexpr int bufferSize{1};
  static char buffer[bufferSize];
  static const char *format{"(A1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputCharacter)(cookie, "a", 1);
  ASSERT_DEATH(IONAME(OutputCharacter)(cookie, "a", 1),
      "Internal write overran available records");
}

TEST(TestIOCrash, OverwriteBufferLogicalTest) {
  static constexpr int bufferSize{1};
  static char buffer[bufferSize];
  static const char *format{"(L1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputLogical)(cookie, true);
  ASSERT_DEATH(IONAME(OutputLogical)(cookie, true),
      "Internal write overran available records");
}

TEST(TestIOCrash, OverwriteBufferRealTest) {
  static constexpr int bufferSize{1};
  static char buffer[bufferSize];
  static const char *format{"(F1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputReal32)(cookie, 1.);
  EXPECT_DEATH(IONAME(OutputReal32)(cookie, 1.),
      "Internal write overran available records");

  std::memset(buffer, '\0', bufferSize);
  cookie = IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format));
  IONAME(OutputReal64)(cookie, 1.);
  EXPECT_DEATH(IONAME(OutputReal64)(cookie, 1.),
      "Internal write overran available records");
}

TEST(TestIOCrash, OverwriteBufferComplexTest) {
  static constexpr int bufferSize{8};
  static char buffer[bufferSize];
  static const char *format{"(Z1,Z1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputComplex32)(cookie, 1., 1.);
  EXPECT_DEATH(IONAME(OutputComplex32)(cookie, 1., 1.),
      "Internal write overran available records");

  std::memset(buffer, '\0', bufferSize);
  cookie = IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format));
  IONAME(OutputComplex64)(cookie, 1., 1.);
  EXPECT_DEATH(IONAME(OutputComplex64)(cookie, 1., 1.),
      "Internal write overran available records");
}

TEST(TestIOCrash, OverwriteBufferIntegerTest) {
  static constexpr int bufferSize{1};
  static char buffer[bufferSize];
  static const char *format{"(I1)"};
  auto *cookie{IONAME(BeginInternalFormattedOutput)(
      buffer, bufferSize, format, std::strlen(format))};
  IONAME(OutputInteger64)(cookie, 0xdeadbeef);
  ASSERT_DEATH(IONAME(OutputInteger64)(cookie, 0xdeadbeef),
      "Internal write overran available records");
}

//------------------------------------------------------------------------------
/// Test conformity issue reports in transformational intrinsics
//------------------------------------------------------------------------------
struct TestIntrinsicCrash : CrashHandlerFixture {};

TEST(TestIntrinsicCrash, ConformityErrors) {
  // ARRAY(2,3) and MASK(2,4) should trigger a runtime error.
  auto array{MakeArray<TypeCategory::Integer, 4>(
      std::vector<int>{2, 3}, std::vector<std::int32_t>{1, 2, 3, 4, 5, 6})};
  auto mask{MakeArray<TypeCategory::Logical, 1>(std::vector<int>{2, 4},
      std::vector<std::uint8_t>{
          false, true, true, false, false, true, true, true})};
  StaticDescriptor<1, true> statDesc;
  Descriptor &result{statDesc.descriptor()};

  ASSERT_DEATH(RTNAME(Pack)(result, *array, *mask, nullptr, __FILE__, __LINE__),
      "Incompatible array arguments to PACK: dimension 2 of ARRAY= has extent "
      "3 but MASK= has extent 4");
}

//===- TosaToLinalgNamed.cpp - Lowering Tosa to Linalg Named Ops ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// These rewriters lower from the Tosa to the Linalg named ops.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/TosaToLinalg/TosaToLinalg.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Dialect/Tosa/Utils/ConversionUtils.h"
#include "mlir/Dialect/Utils/ReshapeOpsUtils.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/DialectConversion.h"

#include <type_traits>

using namespace mlir;
using namespace mlir::tosa;

static mlir::Value applyPad(Location loc, Value input, ArrayRef<int64_t> pad,
                            TypedAttr padAttr, OpBuilder &rewriter) {
  // Input should be padded only if necessary.
  if (llvm::all_of(pad, [](int64_t p) { return p == 0; }))
    return input;

  ShapedType inputTy = cast<ShapedType>(input.getType());
  Type inputETy = inputTy.getElementType();
  auto inputShape = inputTy.getShape();

  assert((inputShape.size() * 2) == pad.size());

  SmallVector<int64_t, 4> paddedShape;
  SmallVector<OpFoldResult, 8> lowIndices;
  SmallVector<OpFoldResult, 8> highIndices;
  for (size_t i : llvm::seq(inputShape.size())) {
    auto lowPad = pad[i * 2];
    auto highPad = pad[i * 2 + 1];
    if (ShapedType::isDynamic(inputShape[i]))
      paddedShape.push_back(inputShape[i]);
    else
      paddedShape.push_back(inputShape[i] + highPad + lowPad);
    lowIndices.push_back(rewriter.getIndexAttr(lowPad));
    highIndices.push_back(rewriter.getIndexAttr(highPad));
  }

  Value padValue = rewriter.create<arith::ConstantOp>(loc, padAttr);

  return rewriter.create<tensor::PadOp>(
      loc, RankedTensorType::get(paddedShape, inputETy), input, lowIndices,
      highIndices, padValue);
}

static mlir::Value
linalgIntBroadcastExtSIAdd(PatternRewriter &rewriter, Location loc, Value bias,
                           Value conv, Value result,
                           ArrayRef<AffineMap> indexingMaps) {
  ShapedType resultTy = cast<ShapedType>(conv.getType());
  return rewriter
      .create<linalg::GenericOp>(
          loc, resultTy, ValueRange({bias, conv}), result, indexingMaps,
          getNParallelLoopsAttrs(resultTy.getRank()),
          [](OpBuilder &builder, Location loc, ValueRange args) {
            Value biasVal = args[0];
            Type resType = args[1].getType();
            if (resType != biasVal.getType()) {
              biasVal = builder.create<arith::ExtSIOp>(loc, resType, biasVal);
            }
            Value added = builder.create<arith::AddIOp>(loc, biasVal, args[1]);
            builder.create<linalg::YieldOp>(loc, added);
          })
      .getResult(0);
}

// Construct the affine map that a linalg generic would use to broadcast the
// source tensor into the shape of the result tensor.
static AffineMap getBroadcastingMap(PatternRewriter &rewriter, Value source,
                                    Value result) {
  ShapedType resultTy = cast<ShapedType>(result.getType());
  ShapedType sourceTy = cast<ShapedType>(source.getType());
  const int64_t resultRank = resultTy.getRank();
  const int64_t sourceRank = sourceTy.getRank();

  // The source tensor is broadcast to all the outer dimensions of the
  // result tensor.
  SmallVector<AffineExpr> sourceDims;
  // In the case of a rank one source tensor with a single element TOSA
  // specifies that the value be broadcast meaning we need an edge case for a
  // constant map.
  assert(sourceTy.hasStaticShape() &&
         "Dynamic broadcasting shapes not supported!");
  if (sourceRank == 1 && sourceTy.getDimSize(0) == 1) {
    sourceDims.push_back(rewriter.getAffineConstantExpr(0));
  } else {
    for (auto dim : llvm::seq<int64_t>(0, sourceRank)) {
      auto expr = rewriter.getAffineDimExpr(dim + resultRank - sourceRank);
      sourceDims.push_back(expr);
    }
  }

  return AffineMap::get(/*dimCount=*/resultRank,
                        /*symbolCount=*/0, sourceDims, rewriter.getContext());
}

// Broadcast the source value to all the outer dimensions of the result value.
// If required, the element type is expanded using an arith.extsi or arith.extf
// operation as appropriate.
static mlir::Value linalgBroadcastAndMaybeExt(PatternRewriter &rewriter,
                                              Location loc, Value source,
                                              Value result) {
  ShapedType resultTy = cast<ShapedType>(result.getType());
  const int64_t resultRank = resultTy.getRank();
  // Creating maps for the input and output of the broacast-like generic op.
  SmallVector<AffineMap, 2> indexingMaps;
  indexingMaps.push_back(getBroadcastingMap(rewriter, source, result));
  indexingMaps.push_back(rewriter.getMultiDimIdentityMap(resultRank));

  // Build the broadcast-like operation as a linalg.generic.
  return rewriter
      .create<linalg::GenericOp>(
          loc, resultTy, ValueRange({source}), result, indexingMaps,
          getNParallelLoopsAttrs(resultTy.getRank()),
          [&resultTy](OpBuilder &builder, Location loc, ValueRange args) {
            Value biasVal = args[0];
            Type resType = args[1].getType();
            if (resType != biasVal.getType()) {
              biasVal =
                  resultTy.getElementType().isFloat()
                      ? builder.create<arith::ExtFOp>(loc, resType, biasVal)
                            .getResult()
                      : builder.create<arith::ExtSIOp>(loc, resType, biasVal)
                            .getResult();
            }
            builder.create<linalg::YieldOp>(loc, biasVal);
          })
      .getResult(0);
}

static mlir::Value reifyConstantDim(int64_t attr,
                                    ImplicitLocOpBuilder &builder) {
  return builder.create<arith::ConstantIndexOp>(attr);
}

// Calculating the output width/height using the formula:
// H = ((IH+pad_top+pad_bottom-(dilation_y*(KH-1)+1))/stride_y)+1
// W = ((IW+pad_left+pad_right-(dilation_x*(KW-1)+1))/stride_x)+1

static mlir::Value getConvOrPoolOutputDim(Location loc, Value inputDim,
                                          int64_t padBeforeAttr,
                                          int64_t padAfterAttr, Value kernelDim,
                                          int64_t strideAttr,
                                          int64_t dilationAttr,
                                          OpBuilder &rewriter) {
  ImplicitLocOpBuilder builder(loc, rewriter);
  auto one = rewriter.create<arith::ConstantOp>(
      loc, IntegerAttr::get(inputDim.getType(), 1));
  Value padBefore = reifyConstantDim(padBeforeAttr, builder);
  Value paddedBefore = builder.create<arith::AddIOp>(inputDim, padBefore);
  Value padAfter = reifyConstantDim(padAfterAttr, builder);
  Value paddedAfter = builder.create<arith::AddIOp>(paddedBefore, padAfter);

  Value subOne = builder.create<arith::SubIOp>(kernelDim, one);
  Value dilation = reifyConstantDim(dilationAttr, builder);
  Value dilated = builder.create<arith::MulIOp>(dilation, subOne);
  Value addOne = builder.create<arith::AddIOp>(dilated, one);

  Value subtract = builder.create<arith::SubIOp>(paddedAfter, addOne);
  Value stride = reifyConstantDim(strideAttr, builder);
  Value divide = builder.create<arith::DivUIOp>(subtract, stride);
  return builder.create<arith::AddIOp>(divide, one);
}

// Creates a vector of the dynamic output dims for Conv2D and Depthwise_Conv2D
static SmallVector<Value> inferDynamicDimsForConv(
    Location loc, Value input, Value weight, ShapedType resultTy,
    ArrayRef<int64_t> padAttr, ArrayRef<int64_t> strideAttr,
    ArrayRef<int64_t> dilationAttr, ArrayRef<int64_t> inputSizeDims,
    ArrayRef<int64_t> kernelSizeDims, OpBuilder &rewriter) {
  ShapedType inputTy = cast<ShapedType>(input.getType());
  int64_t inputRank = inputTy.getRank();

  SmallVector<Value> dynDims;
  dynDims.resize(resultTy.getRank());

  for (uint32_t i = 0, s = inputSizeDims.size(); i < s; ++i) {
    int64_t inputDim = inputSizeDims[i];
    int64_t kernelDim = kernelSizeDims[i];
    if (resultTy.isDynamicDim(inputDim)) {
      auto padTop = padAttr[i * 2];
      auto padBottom = padAttr[i * 2 + 1];
      auto stride = strideAttr[i];
      auto dilation = dilationAttr[i];
      Value initDynDim = rewriter.create<tensor::DimOp>(loc, input, inputDim);
      Value kernelDynDim =
          rewriter.create<tensor::DimOp>(loc, weight, kernelDim);
      // H = F(IH, pad_top, pad_bottom, dilation_y, KH, stride_y)
      dynDims[inputDim] =
          getConvOrPoolOutputDim(loc, initDynDim, padTop, padBottom,
                                 kernelDynDim, stride, dilation, rewriter);
    }
  }

  // Get the batch/channels dimensions.
  for (int i = 0; i < inputRank; i++) {
    if (resultTy.isDynamicDim(i) && !dynDims[i])
      dynDims[i] = rewriter.create<tensor::DimOp>(loc, input, i);
  }

  SmallVector<Value> filteredDims = condenseValues(dynDims);
  return filteredDims;
}

// Creates a map to collapse the last dimension of the Depthwise convolution op
// due to a shape mismatch
static void createDepthwiseConvCollapseMap(
    int64_t outputRank, SmallVector<ReassociationExprs, 4> &reassociationMap,
    OpBuilder &rewriter) {
  reassociationMap.resize(outputRank);
  for (int i = 0; i < outputRank; i++) {
    reassociationMap[i].push_back(rewriter.getAffineDimExpr(i));
  }
  reassociationMap[outputRank - 1].push_back(
      rewriter.getAffineDimExpr(outputRank));
}

namespace {

template <typename TosaConvOp, typename LinalgConvOp, typename LinalgConvQOp>
class ConvConverter : public OpConversionPattern<TosaConvOp> {
public:
  using OpConversionPattern<TosaConvOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(TosaConvOp op, typename TosaConvOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    Value input = op->getOperand(0);
    Value weight = op->getOperand(1);
    Value bias = op->getOperand(2);

    ShapedType inputTy = cast<ShapedType>(input.getType());
    ShapedType weightTy = cast<ShapedType>(weight.getType());
    ShapedType biasTy = cast<ShapedType>(bias.getType());
    ShapedType resultTy = cast<ShapedType>(op->getResult(0).getType());

    Type inputETy = inputTy.getElementType();

    DenseI64ArrayAttr padAttr = op.getPadAttr();
    DenseI64ArrayAttr strideTosaAttr = op.getStrideAttr();
    DenseI64ArrayAttr dilationTosaAttr = op.getDilationAttr();

    Type accETy = op.getAccType();
    Type accTy = RankedTensorType::get(resultTy.getShape(), accETy);

    // Get and verify zero points.
    FailureOr<int64_t> maybeIZp = op.getInputZeroPoint();
    if (failed(maybeIZp))
      return rewriter.notifyMatchFailure(
          op, "input zero point cannot be statically determined");

    FailureOr<int64_t> maybeWZp = op.getWeightZeroPoint();
    if (failed(maybeWZp))
      return rewriter.notifyMatchFailure(
          op, "weight zero point cannot be statically determined");

    const int64_t inputZpVal = *maybeIZp;
    const int64_t weightZpVal = *maybeWZp;

    if (op.verifyInputZeroPoint(inputZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "input zero point must be zero for non-int8 integer types");

    if (op.verifyWeightZeroPoint(weightZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "weight zero point must be zero for non-int8 integer types");

    bool hasZp = (inputZpVal != 0) || (weightZpVal != 0);

    if (!weightTy.hasStaticShape() || !biasTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "tosa.conv ops require static shapes for weight and bias");

    if (inputETy.isUnsignedInteger())
      return rewriter.notifyMatchFailure(
          op, "tosa.conv ops does not support unsigned integer input");

    llvm::SmallVector<int64_t> inputSizeDims;
    llvm::SmallVector<int64_t> kernelSizeDims;
    for (int i = 1; i < resultTy.getRank() - 1; i++) {
      inputSizeDims.push_back(i);
      kernelSizeDims.push_back(i);
    }

    SmallVector<Value> filteredDims = inferDynamicDimsForConv(
        loc, input, weight, resultTy, padAttr.asArrayRef(),
        strideTosaAttr.asArrayRef(), dilationTosaAttr.asArrayRef(),
        inputSizeDims, kernelSizeDims, rewriter);

    auto weightShape = weightTy.getShape();

    // Apply padding as necessary.
    TypedAttr zeroAttr = rewriter.getZeroAttr(inputETy);
    if (hasZp) {
      int64_t intMin =
          APInt::getSignedMinValue(inputETy.getIntOrFloatBitWidth())
              .getSExtValue();
      int64_t intMax =
          APInt::getSignedMaxValue(inputETy.getIntOrFloatBitWidth())
              .getSExtValue();

      if (inputZpVal < intMin || inputZpVal > intMax)
        return rewriter.notifyMatchFailure(
            op, "tosa.conv op quantization has zp outside of input range");

      zeroAttr = rewriter.getIntegerAttr(inputETy, inputZpVal);
    }

    llvm::SmallVector<int64_t> pad;
    pad.resize(2, 0);
    llvm::append_range(pad, padAttr.asArrayRef());
    pad.resize(pad.size() + 2, 0);
    input = applyPad(loc, input, pad, zeroAttr, rewriter);

    if (4 == inputTy.getRank()) {
      // For 2D convolutions, we need to check if the target convolution op
      // wants a HWCF kernel layout.
      bool wantHwcf =
          hasZp ? std::is_same_v<LinalgConvQOp, linalg::Conv2DNhwcHwcfQOp>
                : std::is_same_v<LinalgConvOp, linalg::Conv2DNhwcHwcfOp>;
      if (wantHwcf) {
        // Transpose the kernel to match dimension ordering of the linalg
        // convolution operation.
        // TODO(suderman): See if this can be efficiently folded - check whether
        // the input is used anywhere else, if not fold the constant.
        SmallVector<int32_t> weightPerm;
        for (int i = 1; i < resultTy.getRank(); i++)
          weightPerm.push_back(i);
        weightPerm.push_back(0);

        SmallVector<int64_t> newWeightShape;
        for (auto dim : weightPerm)
          newWeightShape.push_back(weightShape[dim]);
        auto weightPermAttr = rewriter.getDenseI32ArrayAttr(weightPerm);
        Type newWeightTy =
            RankedTensorType::get(newWeightShape, weightTy.getElementType());
        weight = rewriter.create<tosa::TransposeOp>(loc, newWeightTy, weight,
                                                    weightPermAttr);
      }
    }

    // For Conv3D transpose the kernel to match dimension ordering of the linalg
    // convolution operation. Conv2D has a 1-1 mapping in linalg so better to
    // map directly and then transpose later if desired.
    if (5 == inputTy.getRank()) {
      // TODO(suderman): See if this can be efficiently folded - check whether
      // the input is used anywhere else, if not fold the constant.
      SmallVector<int32_t> weightPerm;
      for (int i = 1; i < resultTy.getRank(); i++)
        weightPerm.push_back(i);
      weightPerm.push_back(0);

      SmallVector<int64_t> newWeightShape;
      for (auto dim : weightPerm)
        newWeightShape.push_back(weightShape[dim]);
      auto weightPermAttr = rewriter.getDenseI32ArrayAttr(weightPerm);
      Type newWeightTy =
          RankedTensorType::get(newWeightShape, weightTy.getElementType());
      weight = rewriter.create<tosa::TransposeOp>(loc, newWeightTy, weight,
                                                  weightPermAttr);
    }

    // Extract the attributes for convolution.
    ArrayRef<int64_t> stride = strideTosaAttr;
    ArrayRef<int64_t> dilation = dilationTosaAttr;

    // Create the convolution op.
    auto strideAttr = rewriter.getI64TensorAttr(stride);
    auto dilationAttr = rewriter.getI64TensorAttr(dilation);

    Value biasEmptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, resultTy.getShape(), accETy, filteredDims);

    Value broadcastBias =
        linalgBroadcastAndMaybeExt(rewriter, loc, bias, biasEmptyTensor);

    if (hasZp) {
      auto iZp = rewriter.getI32IntegerAttr(inputZpVal);
      auto kZp = rewriter.getI32IntegerAttr(weightZpVal);

      auto iZpVal = rewriter.create<arith::ConstantOp>(loc, iZp);
      auto kZpVal = rewriter.create<arith::ConstantOp>(loc, kZp);

      Value conv =
          rewriter
              .create<LinalgConvQOp>(
                  loc, resultTy, ValueRange{input, weight, iZpVal, kZpVal},
                  ValueRange{broadcastBias}, strideAttr, dilationAttr)
              ->getResult(0);

      rewriter.replaceOp(op, conv);
      return success();
    }

    Value conv = rewriter
                     .create<LinalgConvOp>(
                         loc, accTy, ValueRange{input, weight},
                         ValueRange{broadcastBias}, strideAttr, dilationAttr)
                     ->getResult(0);

    // We may need to truncate back to the result type if the accumulator was
    // wider than the result.
    if (resultTy != accTy)
      conv = rewriter.create<tosa::CastOp>(loc, resultTy, conv);

    rewriter.replaceOp(op, conv);
    return success();
  }
};

class DepthwiseConvConverter
    : public OpConversionPattern<tosa::DepthwiseConv2DOp> {
public:
  using OpConversionPattern<tosa::DepthwiseConv2DOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tosa::DepthwiseConv2DOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    Value input = op->getOperand(0);
    Value weight = op->getOperand(1);
    Value bias = op->getOperand(2);

    ShapedType inputTy = cast<ShapedType>(input.getType());
    ShapedType weightTy = cast<ShapedType>(weight.getType());
    ShapedType biasTy = cast<ShapedType>(bias.getType());
    ShapedType resultTy = cast<ShapedType>(op->getResult(0).getType());
    int64_t resultRank = resultTy.getRank();

    Type inputETy = inputTy.getElementType();
    Type resultETy = resultTy.getElementType();

    auto padAttr = cast<DenseI64ArrayAttr>(op->getAttr("pad"));
    auto strideTosaAttr = cast<DenseI64ArrayAttr>(op->getAttr("stride"));
    auto dilationTosaAttr = cast<DenseI64ArrayAttr>(op->getAttr("dilation"));

    Type accETy = op.getAccType();

    if (!weightTy.hasStaticShape() || !biasTy.hasStaticShape())
      return rewriter.notifyMatchFailure(
          op, "tosa.depthwise_conv ops require static shapes");

    // Compute output dynamic dims
    SmallVector<Value> filteredDims = inferDynamicDimsForConv(
        loc, input, weight, resultTy, padAttr.asArrayRef(),
        strideTosaAttr.asArrayRef(), dilationTosaAttr.asArrayRef(),
        /*inputSizeDims=*/{1, 2},
        /*kernelSizeDims=*/{0, 1}, rewriter);

    // Get and verify zero points.

    FailureOr<int64_t> maybeIZp = op.getInputZeroPoint();
    FailureOr<int64_t> maybeWZp = op.getWeightZeroPoint();
    if (failed(maybeIZp))
      return rewriter.notifyMatchFailure(
          op, "input zero point cannot be statically determined");
    if (failed(maybeWZp))
      return rewriter.notifyMatchFailure(
          op, "weight zero point cannot be statically determined");

    const int64_t inputZpVal = *maybeIZp;
    const int64_t weightZpVal = *maybeWZp;

    if (op.verifyInputZeroPoint(inputZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "input zero point must be zero for non-int8 integer types");

    if (op.verifyWeightZeroPoint(weightZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "weight zero point must be zero for non-int8 integer types");

    bool hasNullZps = (inputZpVal == 0) && (weightZpVal == 0);
    auto weightShape = weightTy.getShape();
    auto resultShape = resultTy.getShape();

    // Apply padding as necessary.
    TypedAttr zeroAttr = rewriter.getZeroAttr(inputETy);
    if (!hasNullZps) {
      int64_t intMin =
          APInt::getSignedMinValue(inputETy.getIntOrFloatBitWidth())
              .getSExtValue();
      int64_t intMax =
          APInt::getSignedMaxValue(inputETy.getIntOrFloatBitWidth())
              .getSExtValue();

      if (inputZpVal < intMin || inputZpVal > intMax)
        return rewriter.notifyMatchFailure(
            op, "tosa.depthwise_conv op quantization has zp outside of input "
                "range");

      zeroAttr = rewriter.getIntegerAttr(inputETy, inputZpVal);
    }

    llvm::SmallVector<int64_t> pad;
    pad.resize(2, 0);
    llvm::append_range(pad, padAttr.asArrayRef());
    pad.resize(pad.size() + 2, 0);

    input = applyPad(loc, input, pad, zeroAttr, rewriter);

    // Extract the attributes for convolution.
    ArrayRef<int64_t> stride = strideTosaAttr;
    ArrayRef<int64_t> dilation = dilationTosaAttr;

    // Create the convolution op.
    auto strideAttr = rewriter.getI64TensorAttr(stride);
    auto dilationAttr = rewriter.getI64TensorAttr(dilation);
    ShapedType linalgConvTy =
        RankedTensorType::get({resultShape[0], resultShape[1], resultShape[2],
                               weightShape[2], weightShape[3]},
                              accETy);

    auto resultZeroAttr = rewriter.getZeroAttr(accETy);
    Value emptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, linalgConvTy.getShape(), accETy, filteredDims);
    Value zero = rewriter.create<arith::ConstantOp>(loc, resultZeroAttr);
    Value zeroTensor = rewriter
                           .create<linalg::FillOp>(loc, ValueRange{zero},
                                                   ValueRange{emptyTensor})
                           .result();

    Value biasEmptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, resultTy.getShape(), resultETy, filteredDims);

    // Broadcast the initial value to the output tensor before convolving.
    SmallVector<AffineMap, 4> indexingMaps;
    indexingMaps.push_back(getBroadcastingMap(rewriter, bias, biasEmptyTensor));
    indexingMaps.push_back(rewriter.getMultiDimIdentityMap(resultRank));
    indexingMaps.push_back(rewriter.getMultiDimIdentityMap(resultRank));

    if (hasNullZps) {
      Value conv = rewriter
                       .create<linalg::DepthwiseConv2DNhwcHwcmOp>(
                           loc, linalgConvTy, ValueRange{input, weight},
                           ValueRange{zeroTensor}, strideAttr, dilationAttr)
                       .getResult(0);

      // We may need to truncate back to the result type if the accumulator was
      // wider than the result.
      if (accETy != resultETy)
        conv = rewriter.create<tosa::CastOp>(
            loc,
            RankedTensorType::get(cast<ShapedType>(conv.getType()).getShape(),
                                  resultETy),
            conv);

      SmallVector<ReassociationExprs, 4> reassociationMap;
      createDepthwiseConvCollapseMap(resultRank, reassociationMap, rewriter);
      Value convReshape = rewriter.create<tensor::CollapseShapeOp>(
          loc, resultTy, conv, reassociationMap);

      Value result =
          rewriter
              .create<linalg::GenericOp>(
                  loc, resultTy, ValueRange({bias, convReshape}),
                  biasEmptyTensor, indexingMaps,
                  getNParallelLoopsAttrs(resultRank),
                  [&](OpBuilder &nestedBuilder, Location nestedLoc,
                      ValueRange args) {
                    Value added;
                    if (llvm::isa<FloatType>(inputETy))
                      added = nestedBuilder.create<arith::AddFOp>(loc, args[0],
                                                                  args[1]);
                    else
                      added = nestedBuilder.create<arith::AddIOp>(loc, args[0],
                                                                  args[1]);
                    nestedBuilder.create<linalg::YieldOp>(nestedLoc, added);
                  })
              .getResult(0);
      rewriter.replaceOp(op, result);
    } else {
      IntegerAttr iZp = rewriter.getI32IntegerAttr(inputZpVal);
      IntegerAttr wZp = rewriter.getI32IntegerAttr(weightZpVal);
      auto iZpVal = rewriter.create<arith::ConstantOp>(loc, iZp);
      auto kZpVal = rewriter.create<arith::ConstantOp>(loc, wZp);
      Value conv =
          rewriter
              .create<linalg::DepthwiseConv2DNhwcHwcmQOp>(
                  loc, linalgConvTy, ValueRange{input, weight, iZpVal, kZpVal},
                  ValueRange{zeroTensor}, strideAttr, dilationAttr)
              .getResult(0);
      SmallVector<ReassociationExprs, 4> reassociationMap;
      createDepthwiseConvCollapseMap(resultRank, reassociationMap, rewriter);
      Value convReshape = rewriter.create<tensor::CollapseShapeOp>(
          loc, resultTy, conv, reassociationMap);
      Value result = linalgIntBroadcastExtSIAdd(
          rewriter, loc, bias, convReshape, biasEmptyTensor, indexingMaps);
      rewriter.replaceOp(op, result);
    }
    return success();
  }
};

class MatMulConverter : public OpConversionPattern<tosa::MatMulOp> {
public:
  using OpConversionPattern<tosa::MatMulOp>::OpConversionPattern;
  LogicalResult
  matchAndRewrite(tosa::MatMulOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op.getLoc();

    auto outputTy = cast<ShapedType>(op.getType());
    auto outputElementTy = outputTy.getElementType();

    SmallVector<Value> dynDims;
    dynDims.resize(cast<ShapedType>(op->getResult(0).getType()).getRank());

    if (!outputTy.hasRank() || outputTy.isDynamicDim(0)) {
      dynDims[0] = rewriter.create<tensor::DimOp>(loc, op->getOperand(0), 0);
    }

    if (!outputTy.hasRank() || outputTy.isDynamicDim(1)) {
      dynDims[1] = rewriter.create<tensor::DimOp>(loc, op->getOperand(0), 1);
    }

    if (!outputTy.hasRank() || outputTy.isDynamicDim(2)) {
      dynDims[2] = rewriter.create<tensor::DimOp>(loc, op->getOperand(1), 2);
    }

    SmallVector<Value> filteredDims = condenseValues(dynDims);

    auto zeroAttr = rewriter.getZeroAttr(outputElementTy);
    Value zero = rewriter.create<arith::ConstantOp>(loc, zeroAttr);
    auto emptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, outputTy.getShape(), outputTy.getElementType(), filteredDims);
    Value zeroTensor = rewriter
                           .create<linalg::FillOp>(loc, ValueRange{zero},
                                                   ValueRange{emptyTensor})
                           .result();

    FailureOr<int64_t> maybeAZp = op.getAZeroPoint();
    FailureOr<int64_t> maybeBZp = op.getBZeroPoint();
    if (failed(maybeAZp))
      return rewriter.notifyMatchFailure(
          op, "input a zero point cannot be statically determined");
    if (failed(maybeBZp))
      return rewriter.notifyMatchFailure(
          op, "input b zero point cannot be statically determined");

    const int64_t aZpVal = *maybeAZp;
    const int64_t bZpVal = *maybeBZp;

    if (op.verifyAZeroPoint(aZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "input a zero point must be zero for non-int8 integer types");

    if (op.verifyBZeroPoint(bZpVal).failed())
      return rewriter.notifyMatchFailure(
          op, "input b zero point must be zero for non-int8 integer types");

    if (aZpVal == 0 && bZpVal == 0) {
      rewriter.replaceOpWithNewOp<linalg::BatchMatmulOp>(
          op, TypeRange{op.getType()},
          ValueRange{adaptor.getA(), adaptor.getB()}, ValueRange{zeroTensor});
      return success();
    }

    auto aZp = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getI32IntegerAttr(aZpVal));
    auto bZp = rewriter.create<arith::ConstantOp>(
        loc, rewriter.getI32IntegerAttr(bZpVal));
    rewriter.replaceOpWithNewOp<linalg::QuantizedBatchMatmulOp>(
        op, TypeRange{op.getType()},
        ValueRange{adaptor.getA(), adaptor.getB(), aZp, bZp}, zeroTensor);

    return success();
  }
};

class MaxPool2dConverter : public OpConversionPattern<tosa::MaxPool2dOp> {
public:
  using OpConversionPattern::OpConversionPattern;

  // Compute the dynamic output sizes of the maxpool operation.
  static SmallVector<Value>
  computeDynamicOutputSizes(tosa::MaxPool2dOp op, OpAdaptor adaptor,
                            ConversionPatternRewriter &rewriter) {
    TensorType resultTy = op.getType();
    Location loc = op.getLoc();

    Value input = adaptor.getInput();
    ArrayRef<int64_t> kernel = op.getKernel();
    ArrayRef<int64_t> pad = op.getPad();
    ArrayRef<int64_t> stride = op.getStride();

    SmallVector<Value> dynamicDims;

    // Batch dimension
    if (resultTy.isDynamicDim(0))
      dynamicDims.push_back(rewriter.create<tensor::DimOp>(loc, input, 0));

    // Height/width dimensions
    for (int64_t dim : {1, 2}) {
      if (!resultTy.isDynamicDim(dim))
        continue;

      // Index into the attribute arrays
      int64_t index = dim - 1;

      // Input height/width
      Value ihw = rewriter.create<tensor::DimOp>(loc, input, dim);

      // Kernel height/width
      Value khw = rewriter.create<arith::ConstantIndexOp>(loc, kernel[index]);

      // Output height/width
      Value ohw = getConvOrPoolOutputDim(loc, ihw, pad[index * 2],
                                         pad[index * 2 + 1], khw, stride[index],
                                         /*dilationAttr=*/1, rewriter);
      dynamicDims.push_back(ohw);
    }

    // Channel dimension
    if (resultTy.isDynamicDim(3))
      dynamicDims.push_back(rewriter.create<tensor::DimOp>(loc, input, 3));

    return dynamicDims;
  }

  LogicalResult
  matchAndRewrite(tosa::MaxPool2dOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    Value input = adaptor.getInput();
    ShapedType inputTy = cast<ShapedType>(input.getType());

    bool isUnsigned = op.getType().getElementType().isUnsignedInteger();
    ShapedType resultTy =
        cast<ShapedType>(getTypeConverter()->convertType(op.getType()));
    if (!resultTy)
      return rewriter.notifyMatchFailure(op, "failed to convert type");
    Type resultETy = inputTy.getElementType();

    SmallVector<Value> dynamicDims =
        computeDynamicOutputSizes(op, adaptor, rewriter);

    // Determine what the initial value needs to be for the max pool op.
    TypedAttr initialAttr;
    if (resultETy.isF32() || resultETy.isBF16() || resultETy.isF16())
      initialAttr = rewriter.getFloatAttr(
          resultETy, APFloat::getLargest(
                         cast<FloatType>(resultETy).getFloatSemantics(), true));

    else if (isUnsigned)
      initialAttr = rewriter.getIntegerAttr(
          resultETy, APInt::getZero(resultETy.getIntOrFloatBitWidth()));
    else if (isa<IntegerType>(resultETy))
      initialAttr = rewriter.getIntegerAttr(
          resultETy,
          APInt::getSignedMinValue(resultETy.getIntOrFloatBitWidth()));

    if (!initialAttr)
      return rewriter.notifyMatchFailure(
          op, "Unsupported initial value for tosa.maxpool_2d op");

    // Apply padding as necessary.
    llvm::SmallVector<int64_t> pad;
    pad.resize(2, 0);
    llvm::append_range(pad, op.getPad());
    pad.resize(pad.size() + 2, 0);

    Value paddedInput = applyPad(loc, input, pad, initialAttr, rewriter);

    Value initialValue = rewriter.create<arith::ConstantOp>(loc, initialAttr);

    ArrayRef<int64_t> kernel = op.getKernel();
    ArrayRef<int64_t> stride = op.getStride();

    Attribute strideAttr = rewriter.getI64VectorAttr(stride);
    Attribute dilationAttr = rewriter.getI64VectorAttr({1, 1});

    // Create the linalg op that performs pooling.
    Value emptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, resultTy.getShape(), resultTy.getElementType(), dynamicDims);

    Value filledEmptyTensor =
        rewriter.create<linalg::FillOp>(loc, initialValue, emptyTensor)
            .result();

    Value fakeWindowDims =
        rewriter.create<tensor::EmptyOp>(loc, kernel, resultETy);

    if (isUnsigned) {
      rewriter.replaceOpWithNewOp<linalg::PoolingNhwcMaxUnsignedOp>(
          op, ArrayRef<Type>{resultTy}, ValueRange{paddedInput, fakeWindowDims},
          filledEmptyTensor, strideAttr, dilationAttr);
      return llvm::success();
    }

    auto resultOp = rewriter.create<linalg::PoolingNhwcMaxOp>(
        op->getLoc(), ArrayRef<Type>{resultTy},
        ValueRange{paddedInput, fakeWindowDims}, filledEmptyTensor, strideAttr,
        dilationAttr);

    rewriter.replaceOp(op, resultOp);

    // NaN propagation has no meaning for non floating point types.
    if (!isa<FloatType>(getElementTypeOrSelf(inputTy)))
      return success();

    // "PROPAGATE" mode matches the behaviour of the LinAlg named op, so no
    // compare and select materialization is required.
    //
    // In the case of "IGNORE" we need to insert a compare and select. Since
    // we've already produced a named op we will just take its body and modify
    // it to include the appropriate checks. If the current value is NaN the
    // old value of pool will be taken otherwise we use the result.
    if (const auto nanMode = op.getNanMode(); nanMode == "IGNORE") {
      auto genericOp = rewriter.create<linalg::GenericOp>(
          op->getLoc(), resultOp.getType(0), resultOp.getInputs(),
          resultOp.getOutputs(), resultOp.getIndexingMapsArray(),
          resultOp.getIteratorTypesArray(),
          [&](OpBuilder &opBuilder, Location loc, ValueRange blockArgs) {
            IRMapping map;
            auto oldBlock = resultOp.getRegion().begin();
            auto oldArgs = oldBlock->getArguments();
            auto &oldMaxOp = *resultOp.getBlock()->begin();
            map.map(oldArgs, blockArgs);
            auto *newOp = opBuilder.clone(oldMaxOp, map);
            Value isNaN = opBuilder.create<arith::CmpFOp>(
                op->getLoc(), arith::CmpFPredicate::UNO, blockArgs.front(),
                blockArgs.front());
            auto selectOp = opBuilder.create<arith::SelectOp>(
                op->getLoc(), isNaN, blockArgs.back(), newOp->getResult(0));
            opBuilder.create<linalg::YieldOp>(loc, selectOp.getResult());
          });
      rewriter.replaceOp(resultOp, genericOp);
    }

    return success();
  }
};

class AvgPool2dConverter : public OpRewritePattern<tosa::AvgPool2dOp> {
public:
  using OpRewritePattern<tosa::AvgPool2dOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::AvgPool2dOp op,
                                PatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    Value input = op.getInput();
    ShapedType inputTy = cast<ShapedType>(input.getType());
    Type inElementTy = inputTy.getElementType();

    ShapedType resultTy = cast<ShapedType>(op.getType());
    Type resultETy = cast<ShapedType>(op.getType()).getElementType();

    Type accETy = op.getAccType();
    ShapedType accTy = resultTy.clone(accETy);

    auto dynamicDimsOr =
        checkHasDynamicBatchDims(rewriter, op, {input, op.getOutput()});
    if (!dynamicDimsOr.has_value())
      return failure();
    SmallVector<Value> dynamicDims = *dynamicDimsOr;

    FailureOr<int64_t> maybeIZp = op.getInputZeroPoint();
    FailureOr<int64_t> maybeOZp = op.getOutputZeroPoint();
    if (failed(maybeIZp))
      return rewriter.notifyMatchFailure(
          op, "input zero point could not be statically determined");
    if (failed(maybeOZp))
      return rewriter.notifyMatchFailure(
          op, "output zero point could not be statically determined");

    const int64_t inputZpVal = *maybeIZp;
    const int64_t outputZpVal = *maybeOZp;

    // Apply padding as necessary.
    llvm::SmallVector<int64_t> pad;
    pad.resize(2, 0);
    llvm::append_range(pad, op.getPad());
    pad.resize(pad.size() + 2, 0);
    TypedAttr padAttr = rewriter.getZeroAttr(inElementTy);
    // Unsupported element type
    if (!padAttr)
      return failure();
    Value paddedInput = applyPad(loc, input, pad, padAttr, rewriter);

    auto initialAttr = rewriter.getZeroAttr(accETy);
    Value initialValue = rewriter.create<arith::ConstantOp>(loc, initialAttr);

    ArrayRef<int64_t> kernel = op.getKernel();
    ArrayRef<int64_t> stride = op.getStride();

    Attribute strideAttr = rewriter.getI64VectorAttr(stride);
    Attribute dilationAttr = rewriter.getI64VectorAttr({1, 1});

    // Create the linalg op that performs pooling.
    Value poolEmptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, accTy.getShape(), accETy, dynamicDims);

    Value filledEmptyTensor =
        rewriter
            .create<linalg::FillOp>(loc, ValueRange{initialValue},
                                    ValueRange{poolEmptyTensor})
            .result();

    Value fakeWindowDims =
        rewriter.create<tensor::EmptyOp>(loc, kernel, accETy);

    // Sum across the pooled region.
    Value poolingOp = rewriter
                          .create<linalg::PoolingNhwcSumOp>(
                              loc, ArrayRef<Type>{accTy},
                              ValueRange{paddedInput, fakeWindowDims},
                              filledEmptyTensor, strideAttr, dilationAttr)
                          .getResult(0);

    // Normalize the summed value by the number of elements grouped in each
    // pool.
    Value iH = rewriter.create<tensor::DimOp>(loc, poolingOp, 1);
    Value iW = rewriter.create<tensor::DimOp>(loc, poolingOp, 2);

    auto one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    iH = rewriter.create<arith::SubIOp>(loc, iH, one);
    iW = rewriter.create<arith::SubIOp>(loc, iW, one);

    Value genericEmptyTensor = rewriter.create<tensor::EmptyOp>(
        loc, resultTy.getShape(), resultETy, dynamicDims);

    auto affineMap = rewriter.getMultiDimIdentityMap(resultTy.getRank());
    auto genericOp = rewriter.create<linalg::GenericOp>(
        loc, ArrayRef<Type>({resultTy}), ValueRange{poolingOp},
        ValueRange{genericEmptyTensor},
        ArrayRef<AffineMap>({affineMap, affineMap}),
        getNParallelLoopsAttrs(resultTy.getRank()),
        [&](OpBuilder &b, Location loc, ValueRange args) {
          auto zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);

          // Determines what the portion of valid input is covered by the
          // kernel.
          auto padFn = [&](Value valid, Value pos, int64_t pad) -> Value {
            if (pad == 0)
              return valid;

            auto padVal = rewriter.create<arith::ConstantIndexOp>(loc, pad);
            Value dpos = rewriter.create<arith::SubIOp>(loc, pos, padVal);

            Value offset = rewriter.create<arith::MinSIOp>(loc, dpos, zero);
            return rewriter.create<arith::AddIOp>(loc, valid, offset)
                ->getResult(0);
          };

          auto coverageFn = [&](int64_t i, Value isize) -> Value {
            Value strideVal =
                rewriter.create<arith::ConstantIndexOp>(loc, stride[i - 1]);
            Value val =
                rewriter.create<arith::ConstantIndexOp>(loc, kernel[i - 1]);

            // Find the position relative to the input tensor's ends.
            Value left = rewriter.create<linalg::IndexOp>(loc, i);
            Value right = rewriter.create<arith::SubIOp>(loc, isize, left);
            left = rewriter.create<arith::MulIOp>(loc, left, strideVal);
            right = rewriter.create<arith::MulIOp>(loc, right, strideVal);

            // Determine how much padding was included.
            val = padFn(val, left, pad[i * 2]);
            val = padFn(val, right, pad[i * 2 + 1]);
            return rewriter.create<arith::MaxSIOp>(loc, one, val);
          };

          // Compute the indices from either end.
          Value kH3 = coverageFn(1, iH);
          Value kW3 = coverageFn(2, iW);

          // Compute the total number of elements and normalize.
          auto count = rewriter.create<arith::IndexCastOp>(
              loc, rewriter.getI32Type(),
              rewriter.create<arith::MulIOp>(loc, kH3, kW3));

          // Divide by the number of summed values. For floats this is just
          // a div however for quantized values input normalization had
          // to be applied.
          Value poolVal = args[0];
          if (isa<FloatType>(accETy)) {
            auto countF = rewriter.create<arith::SIToFPOp>(loc, accETy, count);
            poolVal = rewriter.create<arith::DivFOp>(loc, poolVal, countF)
                          ->getResult(0);
            if (accETy.getIntOrFloatBitWidth() >
                resultETy.getIntOrFloatBitWidth())
              poolVal =
                  rewriter.create<arith::TruncFOp>(loc, resultETy, poolVal);
          } else {

            // If we have quantization information we need to apply an offset
            // for the input zp value.
            if (inputZpVal != 0) {
              auto inputZp = rewriter.create<arith::ConstantOp>(
                  loc, b.getIntegerAttr(accETy, inputZpVal));
              Value offset =
                  rewriter.create<arith::MulIOp>(loc, accETy, count, inputZp);
              poolVal =
                  rewriter.create<arith::SubIOp>(loc, accETy, poolVal, offset);
            }

            // Compute: k = 32 - count_leading_zeros(value - 1)
            Value one32 = rewriter.create<arith::ConstantOp>(
                loc, rewriter.getI32IntegerAttr(1));
            Value thirtyTwo32 = rewriter.create<arith::ConstantOp>(
                loc, rewriter.getI32IntegerAttr(32));

            Value countSubOne =
                rewriter.create<arith::SubIOp>(loc, count, one32);
            Value leadingZeros =
                rewriter.create<math::CountLeadingZerosOp>(loc, countSubOne);
            Value k =
                rewriter.create<arith::SubIOp>(loc, thirtyTwo32, leadingZeros);

            // Compute: numerator = ((1 << 30) + 1) << k
            Value k64 =
                rewriter.create<arith::ExtUIOp>(loc, rewriter.getI64Type(), k);
            Value thirtyShiftPlusOne = rewriter.create<arith::ConstantOp>(
                loc, rewriter.getI64IntegerAttr((1 << 30) + 1));
            Value numerator =
                rewriter.create<arith::ShLIOp>(loc, thirtyShiftPlusOne, k64);

            // Compute: scale.multiplier = numerator / value;
            Value count64 = rewriter.create<arith::ExtUIOp>(
                loc, rewriter.getI64Type(), count);
            Value multiplier =
                rewriter.create<arith::DivUIOp>(loc, numerator, count64);
            multiplier = rewriter.create<arith::TruncIOp>(
                loc, rewriter.getI32Type(), multiplier);

            // Compute: scale.shift = 30 + k
            Value k8 =
                rewriter.create<arith::TruncIOp>(loc, rewriter.getI8Type(), k);
            Value thirty8 = rewriter.create<arith::ConstantOp>(
                loc, rewriter.getI8IntegerAttr(30));
            Value shift = rewriter.create<arith::AddIOp>(loc, k8, thirty8);

            auto scaled =
                rewriter
                    .create<tosa::ApplyScaleOp>(
                        loc, rewriter.getI32Type(), poolVal, multiplier, shift,
                        rewriter.getStringAttr("SINGLE_ROUND"))
                    .getResult();

            // If we have quantization information we need to apply output
            // zeropoint.
            if (outputZpVal != 0) {
              auto outputZp = rewriter.create<arith::ConstantOp>(
                  loc, b.getIntegerAttr(scaled.getType(), outputZpVal));
              scaled = rewriter.create<arith::AddIOp>(loc, scaled, outputZp)
                           .getResult();
            }

            // Apply Clip.
            int64_t outBitwidth = resultETy.getIntOrFloatBitWidth();

            auto min = rewriter.create<arith::ConstantIntOp>(
                loc, accETy,
                APInt::getSignedMinValue(outBitwidth).getSExtValue());
            auto max = rewriter.create<arith::ConstantIntOp>(
                loc, accETy,
                APInt::getSignedMaxValue(outBitwidth).getSExtValue());
            auto clamp = clampIntHelper(loc, scaled, min, max, rewriter,
                                        /*isUnsigned=*/false);

            poolVal = clamp;
            // Convert type.
            if (resultETy != clamp.getType()) {
              poolVal =
                  rewriter.create<arith::TruncIOp>(loc, resultETy, poolVal);
            }
          }

          rewriter.create<linalg::YieldOp>(loc, poolVal);
        });

    rewriter.replaceOp(op, genericOp.getResult(0));
    return success();
  }
};

class TransposeConverter : public OpRewritePattern<tosa::TransposeOp> {
public:
  using OpRewritePattern<tosa::TransposeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tosa::TransposeOp op,
                                PatternRewriter &rewriter) const final {
    const llvm::ArrayRef<int32_t> constantPerms = op.getPerms();

    Location loc = op.getLoc();
    // The verifier should have made sure we have a valid TOSA permutation
    // tensor. isPermutationVector doesn't actually check the TOSA perms we
    // expect.
    SmallVector<OpFoldResult> inputSizes =
        tensor::getMixedSizes(rewriter, loc, op.getInput1());
    auto permutedSizes =
        applyTOSAPermutation<OpFoldResult>(inputSizes, constantPerms);

    auto permutedInit = rewriter.create<tensor::EmptyOp>(
        loc, permutedSizes, op.getInput1().getType().getElementType());
    rewriter.replaceOpWithNewOp<linalg::TransposeOp>(
        op, op.getInput1(), permutedInit,
        llvm::to_vector(llvm::map_range(
            constantPerms, [](int32_t v) -> int64_t { return v; })));
    return success();
  }
};
} // namespace

void mlir::tosa::populateTosaToLinalgNamedConversionPatterns(
    const TypeConverter &converter, RewritePatternSet *patterns,
    const TosaToLinalgNamedOptions &options) {
  if (options.preferConv2DKernelLayoutHWCF) {
    patterns->add<ConvConverter<tosa::Conv2DOp, linalg::Conv2DNhwcHwcfOp,
                                linalg::Conv2DNhwcHwcfQOp>>(
        patterns->getContext());
  } else {
    patterns->add<ConvConverter<tosa::Conv2DOp, linalg::Conv2DNhwcFhwcOp,
                                linalg::Conv2DNhwcFhwcQOp>>(
        patterns->getContext());
  }
  patterns->add<
      // clang-format off
      ConvConverter<tosa::Conv3DOp, linalg::Conv3DNdhwcDhwcfOp, linalg::Conv3DNdhwcDhwcfQOp>,
      DepthwiseConvConverter,
      MatMulConverter,
      AvgPool2dConverter,
      TransposeConverter
  >(patterns->getContext());

  patterns->add<
      MaxPool2dConverter
    >(converter, patterns->getContext());
  // clang-format on
}

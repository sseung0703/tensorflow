/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This transformation pass prepares for legalization to the TFLite dialect by
// converting operations in TensorFlow dialect into operations that can be
// legalized to TensorFlow Lite dialect with simple replacements.  The newly
// created operations are in the TensorFlow dialect if the operation can be
// represented using a TensorFlow op.  Otherwise, TensorFlow Lite dialect op is
// used.  For example, Conv2D in TFLite which uses OHWI data format for filters
// is not supported in TensorFlow because TensorFlow requires filters in the
// HWIO data format.
//
// Motivation to prepare for the TFLite legalization before the actual
// legalization is to exploit constant folding opportunities in any newly
// created ops by leveraging constant folding support for the TensorFlow ops.
// This way TFLite can be used as a serialization format only and does not
// require access to the TFLite runtime for optimizations as required by the
// TFLite team.

#include <climits>
#include <cstdint>

#include "absl/memory/memory.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "mlir/Analysis/LoopAnalysis.h"  // from @llvm-project
#include "mlir/Dialect/Quant/FakeQuantSupport.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/UniformSupport.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_utils.h"
#include "tensorflow/compiler/mlir/lite/transforms/dilated_conv.h"
#include "tensorflow/compiler/mlir/lite/transforms/passes.h"
#include "tensorflow/compiler/mlir/lite/utils/attribute_utils.h"
#include "tensorflow/compiler/mlir/lite/utils/constant_utils.h"
#include "tensorflow/compiler/mlir/lite/utils/validators.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/einsum.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/unroll_batch_matmul.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"

#define DEBUG_TYPE "tf-tfl-legalization"

namespace mlir {
namespace TFL {
//===----------------------------------------------------------------------===//
// The actual PrepareTF Pass.
//
// TODO(hinsu): Add and use TensorFlow dialect ops for the ops created in this
// pass.
namespace {

// Prepare TF operations in functions for subsequent legalization.
class PrepareTFPass : public PassWrapper<PrepareTFPass, FunctionPass> {
 public:
  PrepareTFPass() = default;
  PrepareTFPass(const PrepareTFPass &) {}
  explicit PrepareTFPass(bool unfold_batch_matmul) {
    unfold_batch_matmul_ = unfold_batch_matmul;
  }
  void runOnFunction() override;

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<mhlo::MhloDialect, quant::QuantizationDialect,
                    TFL::TensorFlowLiteDialect>();
  }

 private:
  Option<bool> unfold_batch_matmul_{
      *this, "tfl-unfold-batch-matmul",
      llvm::cl::desc("Unfold BatchMatMul into individual MatMul ops."),
      llvm::cl::init(true)};
};

template <class TFFakeQuantOp>
struct FetchConstantMinMaxInputs {
  using AttrType = DenseFPElementsAttr;
  bool operator()(TFFakeQuantOp tf_op, AttrType &min_value,
                  AttrType &max_value) const {
    Value min = tf_op.min(), max = tf_op.max();

    // TODO: incomplete  neither IdentityN ops
    // nor chains of Identity* (not rare) are handled
    if (auto id1 = dyn_cast_or_null<TF::IdentityOp>(min.getDefiningOp()))
      min = id1.input();
    if (auto id2 = dyn_cast_or_null<TF::IdentityOp>(max.getDefiningOp()))
      max = id2.input();
    if (!matchPattern(min, m_Constant(&min_value))) {
      return false;
    }
    if (!matchPattern(max, m_Constant(&max_value))) {
      return false;
    }
    return true;  // Succesfully matched and fetched.
  }
};

template <class TFFakeQuantOp>
struct FetchMinMaxAttrs {
  using AttrType = FloatAttr;
  bool operator()(TFFakeQuantOp tf_op, AttrType &min_value,
                  AttrType &max_value) const {
    min_value = tf_op.minAttr();
    max_value = tf_op.maxAttr();
    return true;  // Succesfully matched and fetched.
  }
};

// TODO(fengliuai): move this rule to PreparePatterns.td
// TODO(fengliuai): reuse the quantization/tensorflow/tf_to_quant pass.
// TODO(b/140968741): propagate the sign from the command line. Currently all
// the FakeQuant is assumed to targeting UIN8, but per-channel kernel is
// actually INT8.
// Inserts a "tfl.quantize" and "tfl.dequantize" op pair (QDQs) after the
// tf.FakeQyantWithMinMax{Vars|VarsPerChannel|Args}Op
// to be constant folded. Since the constant
// folding logic will use a "std.constant" op to replace the
// "tf.FakeQuantWithMinMaxVarsOp", the "tfl.quantize" op is used to preserve
// the quantization parameters as a TypeAttr and "tfl.dequantize" op used to
// convert the output type to the next op. Here are the transformations:
//
// input   min cst       max cst          input   min cst       max cst
//  \       |             |                \       |             |
//   \  (tf.Identity) (tf.Identity)   =>    \  (tf.Identity) (tf.Identity)
//    \     |             |                  \     |             |
//       tf.FakeQuantWithMinMaxVars       tf.FakeQuantWithMinMaxVars
//                   |                                 |
//                                                tf.quantize
//                                                     |
//                                                tf.dequantize
//                                                     |
// If the input is a constant, the result pattern will eventually converted to
//
//            quant-emulated input
//                   |
//               tf.quantize
//                   |
//              tf.dequantize
//                   |
//
//
// Warns if the (most likely unwanted, currently not quite correctly handled)
// case of back-to-back tf.FakeQuant occurs
//
//             tf.FakeQuant*
//                   |
//             tf.FakeQuant*
//
// tf.identity / tf.IdentityN between the tf.FakeQuant* ops
// need no special treatment are already eliminated before the rewrites / check
// is applied.
//

template <typename TFFakeQuantOp, bool PerAxis, class FetchMinMax>
struct InsertTFLQuantOpsAfterTFFakeQuantOp
    : public OpRewritePattern<TFFakeQuantOp> {
  using BaseType =
      InsertTFLQuantOpsAfterTFFakeQuantOp<TFFakeQuantOp, PerAxis, FetchMinMax>;

  explicit InsertTFLQuantOpsAfterTFFakeQuantOp<TFFakeQuantOp, PerAxis,
                                               FetchMinMax>(MLIRContext *ctx)
      : OpRewritePattern<TFFakeQuantOp>(ctx) {}

  FetchMinMax fetchMinMax;

  using FetchAttrType = typename FetchMinMax::AttrType;
  LogicalResult matchAndRewrite(TFFakeQuantOp tf_op,
                                PatternRewriter &rewriter) const override {
    // We don't want to insert quantize/dequantize if the quantize op exists.
    auto res = tf_op.outputs();
    if (!res.hasOneUse() || isa<QuantizeOp>(*res.user_begin())) {
      return failure();
    }

    // Extract the min/max constant values from the operands. We also consider
    // a special case that there are tf.Identity ops between the min/max
    // constants and the tf.FakeQuantWithMinMaxVarsOp.

    FetchAttrType min_value, max_value;
    if (!fetchMinMax(tf_op, min_value, max_value)) {
      return failure();
    }

    int quant_dim = -1;
    if (PerAxis) {
      // This is a special case that the quant_dim is the last dimensions.
      quant_dim = res.getType().template cast<ShapedType>().getRank() - 1;
    }
    // Use the min/max from the operands and the num_bits and narrow_range
    // attribute to create the quantization parameter for the new quantize op.
    rewriter.setInsertionPointAfter(tf_op.getOperation());
    IntegerAttr num_bits = rewriter.getI64IntegerAttr(tf_op.num_bits());
    BoolAttr narrow_range = rewriter.getBoolAttr(tf_op.narrow_range());
    Type res_type = tf_op.getType();
    TypeAttr qtype = quant::GetQuantizedTypeAttr(
        rewriter, res_type, min_value, max_value, quant_dim, num_bits,
        narrow_range, /*is_signed=*/false);
    if (!qtype) {
      return failure();
    }

    // Finally, use the quantization parameter to create the quantize and
    // dequantize ops, and insert them between the tf.FakeQuantWithMinMaxVarsOp
    // and its users.
    Value value = tf_op.outputs();
    auto quantize = rewriter.create<TFL::QuantizeOp>(
        tf_op.getLoc(), qtype.getValue(), value, qtype);
    auto dequantize = rewriter.create<TFL::DequantizeOp>(
        tf_op.getLoc(), res_type, quantize.output());
    value.replaceAllUsesWith(dequantize);
    quantize.getOperation()->replaceUsesOfWith(dequantize, value);

    return success();
  }
};

//
// Three instances of the rule to cover the three different types of
// TF::FakeQuant operators
//
using PreparePerTensorFakeQuant = InsertTFLQuantOpsAfterTFFakeQuantOp<
    TF::FakeQuantWithMinMaxVarsOp, /*PerAxis=*/false,
    FetchConstantMinMaxInputs<TF::FakeQuantWithMinMaxVarsOp>>;

using PreparePerChannelFakeQuant = InsertTFLQuantOpsAfterTFFakeQuantOp<
    TF::FakeQuantWithMinMaxVarsPerChannelOp, /*PerAxis=*/true,
    FetchConstantMinMaxInputs<TF::FakeQuantWithMinMaxVarsPerChannelOp>>;

using PreparePerTensorFakeQuantWithMinMaxArgs =
    InsertTFLQuantOpsAfterTFFakeQuantOp<
        TF::FakeQuantWithMinMaxArgsOp, /*PerAxis=*/false,
        FetchMinMaxAttrs<TF::FakeQuantWithMinMaxArgsOp>>;

// Templated class for declaring a converter from some TensorFlow convolution
// op into its counterpart in TensorFlow Lite.
//
// The `ConcreteType` deriving from this template must provide the following
// method for constructing TensorFlow Lite op:
//
//   TFL::[op] createTFLOp(ConvertTFConvOpMatchState *state,
//                         PatternRewriter &rewriter, Location loc,
//                         Type result_type, Value input,
//                         Value filter, Value bias) const;
//
// And also the following method for getting the dimension for bias tensor:
//
//  int64_t getBiasDim(ArrayRef<int64_t> filterShape) const;
template <typename ConcreteType, typename TFConvOpType>
struct ConvertTFConvOp : public RewritePattern {
  // Transient state for preserving data from match to rewrite
  struct ConvertTFConvOpMatchState {
    IntegerAttr dilation_height_factor;
    IntegerAttr dilation_width_factor;
    StringAttr padding;
    IntegerAttr stride_height;
    IntegerAttr stride_width;
  };

  ConvertTFConvOp(MLIRContext *context)
      : RewritePattern(TFConvOpType::getOperationName(), 1, context),
        intAttrOne(Builder(context).getI32IntegerAttr(1)) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    // Assumes TensorFlow convolution op is already verified to be
    // in valid form.

    // Match a TFConvOpType under the following conditions:
    // * The 'T' attribute must exist and be of value DT_FLOAT.
    // * The 'data_format' attribute must exist and be of value "NHWC".
    // * The 'strides' attribute must exist and is of the form [1, X, Y, 1].
    // * The 'dilations' attribute is optional, but it must be of the form
    //   [1, X, Y, 1] if exists.

    TFConvOpType tf_op = cast<TFConvOpType>(op);

    if (!TFTypeIsFloatTensor(tf_op.input()) || !TFDataFormatIsNHWC(op))
      return failure();

    IntegerAttr height, width;
    if (!TFIntListIs1XY1(op, "strides", &height, &width)) return failure();

    ConvertTFConvOpMatchState state;
    state.stride_height = height;
    state.stride_width = width;

    if (TFIntListIs1XY1(op, "dilations", &height, &width)) {
      state.dilation_height_factor = height;
      state.dilation_width_factor = width;
    } else {
      // If the 'dilations' attribute is missing, we use the default value (1)
      // for both dilation height and width factor.
      state.dilation_height_factor = intAttrOne;
      state.dilation_width_factor = intAttrOne;
    }

    if (!TFPaddingIsSameOrValid(op, &state.padding)) return failure();

    // Additionally, we require the filter operand to be of 4-D tensor type so
    // that we can extract info from the shape (e.g., for constructing bias
    // tensor, for setting depth_multiplier attribute, etc.).
    auto filter = tf_op.filter();
    auto filter_type = filter.getType().template dyn_cast<RankedTensorType>();
    if (!filter_type || filter_type.getRank() != 4) return failure();

    // TensorFlow convolution op only has two inputs, while the TFLite one has
    // three, with the bias vector marked as optional. However, TOCO has a
    // dedicated pass, EnsureBiasVectors, to create default bias vectors for all
    // those missing. So we model TFLite convolution op as requiring three
    // inputs to achieve the legalization task of EnsureBiasVector. this
    // requires the filter tensor to have static shape.

    // TODO(antiagainst): also handle the case of tf.Add(tf.[op], <bias>)

    // Get a splat zero tensor with the expected dimension for the bias tensor
    auto elem_type = filter_type.getElementType();
    auto bias_dim = static_cast<const ConcreteType *>(this)->getBiasDim(
        filter_type.getShape());
    auto bias_type = RankedTensorType::get({bias_dim}, elem_type);
    auto bias_attr = rewriter.getZeroAttr(bias_type);
    auto bias =
        rewriter.create<TF::ConstOp>(op->getLoc(), bias_type, bias_attr);

    auto conv_op = static_cast<const ConcreteType *>(this)->createTFLOp(
        &state, rewriter, op->getLoc(), tf_op.getType(), tf_op.input(), filter,
        bias);

    rewriter.replaceOp(op, conv_op.getResult());
    return success();
  }

  const IntegerAttr intAttrOne;
};

class ConvertTFConv2D : public ConvertTFConvOp<ConvertTFConv2D, TF::Conv2DOp> {
 public:
  using BaseType = ConvertTFConvOp<ConvertTFConv2D, TF::Conv2DOp>;

  ConvertTFConv2D(MLIRContext *context) : BaseType(context) {}

  int64_t getBiasDim(ArrayRef<int64_t> filterShape) const {
    return filterShape.back();
  }

  TFL::Conv2DOp createTFLOp(ConvertTFConvOpMatchState *state,
                            PatternRewriter &rewriter, Location loc,
                            Type result_type, Value input, Value filter,
                            Value bias) const {
    filter = legalizeFilter(rewriter, loc, filter);
    return rewriter.create<TFL::Conv2DOp>(
        loc, result_type, input, filter, bias,
        /*dilation_h_factor=*/state->dilation_height_factor,
        /*dilation_w_factor=*/state->dilation_width_factor,
        /*fused_activation_function=*/rewriter.getStringAttr("NONE"),
        /*padding=*/state->padding,
        /*stride_h=*/state->stride_height,
        /*stride_w=*/state->stride_width);
  }

 private:
  // Legalize the given filter by converting it from TensorFlow filter data
  // format HWIO to TFLite Conv2D op filter data format OHWI and return Value
  // for the converted filter.  Requires that filter is verified by the match
  // method that it is a 4-D RankedTensorType.
  Value legalizeFilter(PatternRewriter &rewriter, Location loc,
                       Value filter) const {
    // Create a constant op for HWIO to OHWI transpose permutation.
    SmallVector<int, 4> perm = {3, 0, 1, 2};
    auto perm_type = RankedTensorType::get({static_cast<int>(perm.size())},
                                           rewriter.getIntegerType(32));
    auto perm_attr =
        DenseElementsAttr::get(perm_type, llvm::makeArrayRef<int>(perm));
    auto perm_op = rewriter.create<TF::ConstOp>(loc, perm_type, perm_attr);

    // Create tensor type for the transpose result.
    auto filter_type = filter.getType().cast<RankedTensorType>();
    auto result_shape =
        llvm::to_vector<4>(llvm::map_range(perm, [filter_type](int64_t dim) {
          return filter_type.getDimSize(dim);
        }));
    auto elem_type = filter_type.getElementType();
    auto result_type = RankedTensorType::get(result_shape, elem_type);

    return rewriter.create<TF::TransposeOp>(loc, result_type, filter, perm_op);
  }
};

class ConvertTFDepthwiseConv2dNative
    : public ConvertTFConvOp<ConvertTFDepthwiseConv2dNative,
                             TF::DepthwiseConv2dNativeOp> {
 public:
  using BaseType = ConvertTFConvOp<ConvertTFDepthwiseConv2dNative,
                                   TF::DepthwiseConv2dNativeOp>;

  ConvertTFDepthwiseConv2dNative(MLIRContext *context) : BaseType(context) {}

  int64_t getBiasDim(ArrayRef<int64_t> filterShape) const {
    return filterShape[2] * filterShape[3];
  }

  TFL::DepthwiseConv2DOp createTFLOp(ConvertTFConvOpMatchState *state,
                                     PatternRewriter &rewriter, Location loc,
                                     Type result_type, Value input,
                                     Value filter, Value bias) const {
    // Compared to tfl.conv_2d, tfl.depthwise_conv_2d has an additional
    // 'depth_multiplier' attribute. However, tf.DepthwiseConv2dNative does not
    // have a corresponding 'depth_multiplier' attribute; the multiplier is the
    // fourth dimension in the 4-D filter tensor. We query the multiplier from
    // tf.DepthwiseConv2dNative and set it as the attribute value accordingly.
    auto multiplier = filter.getType().cast<RankedTensorType>().getDimSize(3);

    filter = legalizeFilter(rewriter, loc, filter);
    return rewriter.create<TFL::DepthwiseConv2DOp>(
        loc, result_type, input, filter, bias,
        /*dilation_h_factor=*/state->dilation_height_factor,
        /*dilation_w_factor=*/state->dilation_width_factor,
        /*fused_activation_function=*/rewriter.getStringAttr("NONE"),
        /*padding=*/state->padding,
        /*stride_h=*/state->stride_height,
        /*stride_w=*/state->stride_width,
        /*depth_multiplier=*/rewriter.getI32IntegerAttr(multiplier));
  }

 private:
  /// Legalize the given filter by converting it from TensorFlow filter data
  /// format to TFLite DepthwiseConv2D op filter data format and return Value
  /// for the converted filter.  TensorFlow filter data format is
  /// [filter_height, filter_width, in_channels, channel_multiplier] and TFLite
  /// filter data format is [1, filter_height, filter_width, out_channels].
  /// Requires that filter is verified by the match method that it is a 4-D
  /// RankedTensorType.
  Value legalizeFilter(PatternRewriter &rewriter, Location loc,
                       Value filter) const {
    auto filter_type = filter.getType().cast<RankedTensorType>();
    auto filterShape = filter_type.getShape();
    SmallVector<int64_t, 4> result_shape = {1, filterShape[0], filterShape[1],
                                            filterShape[2] * filterShape[3]};
    auto elem_type = filter_type.getElementType();
    auto result_type = RankedTensorType::get(result_shape, elem_type);
    // TensorFlow Lite `Reshape` op only support int32 shape tensor currently.
    auto shape_type = RankedTensorType::get({4}, rewriter.getIntegerType(32));
    SmallVector<Attribute, 4> result_shape_data(4);
    for (int i = 0; i < 4; ++i) {
      result_shape_data[i] =
          rewriter.getI32IntegerAttr(static_cast<int32_t>(result_shape[i]));
    }
    auto shape_attr = DenseElementsAttr::get(shape_type, result_shape_data);
    auto shape = rewriter.create<TF::ConstOp>(loc, shape_type, shape_attr);

    return rewriter.create<TF::ReshapeOp>(loc, result_type, filter, shape);
  }
};

// StridedSlice can have complicated attributes like begin_axis_mask,
// end_axis_mask, ellipsis_axis_mask, new_axis_mask, shrink_axis_mask. These
// masks will complicate the strided_slice computation logic, we can simplify
// the logic by inserting a reshape op to pad the inputs so strided_slice can
// be easier to handle.
//
// So the graph may looks like below:
//   original_input -> strided_slice -> output
//      (transforms)
//   original_input -> reshape -> strided_slice -> output
//
// And the new shape is computed based on the masks.
//
// An example for new_axis_mask. say the new_axis_mask is 9 which represents
// [1 0 0 1], and that means we're inserting two new axes at 0 & 3 dim, so
// if original shape is [2, 3], now we reshape that into [1, 2, 3, 1].
struct ConvertTFStridedSlice : public RewritePattern {
  explicit ConvertTFStridedSlice(MLIRContext *context)
      : RewritePattern(TF::StridedSliceOp::getOperationName(), 2, context) {}

  LogicalResult RewriteNewAxisMask(Operation *op, uint64_t new_axis_mask,
                                   PatternRewriter &rewriter) const {
    TF::StridedSliceOp strided_slice_op = llvm::cast<TF::StridedSliceOp>(op);

    // Insert a new reshape op.
    Value original_input = strided_slice_op.input();
    RankedTensorType original_input_type =
        original_input.getType().cast<RankedTensorType>();
    const ArrayRef<int64_t> &original_input_shape =
        original_input_type.getShape();
    SmallVector<int64_t, 4> new_shape;
    int index = 0;
    const int original_input_rank = original_input_shape.size();
    while (index < original_input_rank || new_axis_mask) {
      if (new_axis_mask & 1) {
        new_shape.emplace_back(1);
      } else {
        new_shape.emplace_back(original_input_shape[index++]);
      }
      new_axis_mask >>= 1;
    }

    const int dim_size = new_shape.size();
    Location loc = strided_slice_op.getLoc();
    auto shape_type =
        RankedTensorType::get({dim_size}, rewriter.getIntegerType(32));
    SmallVector<Attribute, 4> result_shape_data(dim_size);
    for (int i = 0; i < dim_size; ++i) {
      result_shape_data[i] =
          rewriter.getI32IntegerAttr(static_cast<int32_t>(new_shape[i]));
    }
    auto shape_attr = DenseElementsAttr::get(shape_type, result_shape_data);
    auto shape = rewriter.create<ConstantOp>(loc, shape_type, shape_attr);
    auto new_output_type =
        RankedTensorType::get(new_shape, original_input_type.getElementType());
    TF::ReshapeOp reshape = rewriter.create<TF::ReshapeOp>(
        loc, new_output_type, original_input, shape);

    // Replace the original strided_slice.
    uint64_t new_begin_mask = strided_slice_op.begin_mask();
    uint64_t new_end_mask = strided_slice_op.end_mask();
    // Since we expand the dims, we need to apply them to the begin_mask &
    // end_mask.
    new_begin_mask |= strided_slice_op.new_axis_mask();
    new_end_mask |= strided_slice_op.new_axis_mask();

    auto attribute_type = rewriter.getIntegerType(64);
    rewriter.replaceOpWithNewOp<TF::StridedSliceOp>(
        op, strided_slice_op.getType(), reshape, strided_slice_op.begin(),
        strided_slice_op.end(), strided_slice_op.strides(),
        rewriter.getIntegerAttr(attribute_type, new_begin_mask),
        rewriter.getIntegerAttr(attribute_type, new_end_mask),
        rewriter.getIntegerAttr(attribute_type,
                                strided_slice_op.ellipsis_mask()),
        rewriter.getI64IntegerAttr(0),
        rewriter.getIntegerAttr(attribute_type,
                                strided_slice_op.shrink_axis_mask()));
    return success();
  }

  LogicalResult RewriteEllipsisMask(Operation *op, uint64_t ellipsis_mask,
                                    PatternRewriter &rewriter) const {
    TF::StridedSliceOp strided_slice_op = llvm::cast<TF::StridedSliceOp>(op);

    DenseIntElementsAttr begin_dense_elem_attr;
    Value begin = strided_slice_op.begin();
    auto begin_ranked_attr_type = begin.getType().dyn_cast<RankedTensorType>();
    if (!begin_ranked_attr_type ||
        !matchPattern(begin, m_Constant(&begin_dense_elem_attr))) {
      return failure();
    }

    DenseIntElementsAttr end_dense_elem_attr;
    Value end = strided_slice_op.end();
    auto end_ranked_attr_type = end.getType().dyn_cast<RankedTensorType>();
    if (!end_ranked_attr_type ||
        !matchPattern(end, m_Constant(&end_dense_elem_attr))) {
      return failure();
    }

    DenseIntElementsAttr stride_dense_elem_attr;
    Value stride = strided_slice_op.strides();
    auto stride_ranked_attr_type =
        stride.getType().dyn_cast<RankedTensorType>();
    if (!stride_ranked_attr_type ||
        !matchPattern(stride, m_Constant(&stride_dense_elem_attr))) {
      return failure();
    }

    Value input = strided_slice_op.input();
    RankedTensorType input_type = input.getType().cast<RankedTensorType>();
    const ArrayRef<int64_t> input_shape = input_type.getShape();

    const int input_size = input_shape.size();

    RankedTensorType begin_type = begin.getType().cast<RankedTensorType>();
    const ArrayRef<int64_t> begin_shape = begin_type.getShape();
    const int begin_dim = begin_shape.size();

    if (begin_dim != 1) return failure();

    const int ellipsis_filled_dim_size = input_size - begin_shape[0] + 1;

    int64_t begin_mask = strided_slice_op.begin_mask();
    int64_t end_mask = strided_slice_op.end_mask();
    int64_t new_begin_mask = 0;
    int64_t new_end_mask = 0;

    SmallVector<int32_t, 4> padded_begin;
    SmallVector<int32_t, 4> padded_end;
    SmallVector<int32_t, 4> padded_stride;

    // Before the ellipsis.
    int index = 0;
    int new_index = 0;
    while (((ellipsis_mask >> index) & 1) == 0) {
      padded_begin.push_back(begin_dense_elem_attr.getValue<int32_t>(index));
      padded_end.push_back(end_dense_elem_attr.getValue<int32_t>(index));
      padded_stride.push_back(stride_dense_elem_attr.getValue<int32_t>(index));
      if ((begin_mask >> index) & 1) new_begin_mask |= (1 << new_index);
      if ((end_mask >> index) & 1) new_end_mask |= (1 << new_index);
      ++index;
      ++new_index;
    }

    // Ellipsis.
    for (; new_index < index + ellipsis_filled_dim_size; ++new_index) {
      new_begin_mask |= (1 << new_index);
      new_end_mask |= (1 << new_index);

      // Mimic the begin/end/strides mask behavior.
      padded_begin.push_back(0);
      padded_end.push_back(0);
      padded_stride.push_back(1);
    }

    // Account for ellipsis mask.
    ++index;

    // After the ellipsis.
    for (; index < begin_shape[0];) {
      padded_begin.push_back(begin_dense_elem_attr.getValue<int32_t>(index));
      padded_end.push_back(end_dense_elem_attr.getValue<int32_t>(index));
      padded_stride.push_back(stride_dense_elem_attr.getValue<int32_t>(index));

      if ((begin_mask >> index) & 1) new_begin_mask |= (1 << new_index);
      if ((end_mask >> index) & 1) new_end_mask |= (1 << new_index);

      ++index;
      ++new_index;
    }

    auto attribute_type = rewriter.getIntegerType(64);

    int full_dim_count = padded_begin.size();
    auto type =
        RankedTensorType::get({full_dim_count}, rewriter.getIntegerType(32));

    auto begin_attr = DenseElementsAttr::get<int32_t>(type, padded_begin);
    auto begin_op = rewriter.create<ConstantOp>(op->getLoc(), type, begin_attr);
    auto end_attr = DenseElementsAttr::get<int32_t>(type, padded_end);
    auto end_op = rewriter.create<ConstantOp>(op->getLoc(), type, end_attr);
    auto stride_attr = DenseElementsAttr::get<int32_t>(type, padded_stride);
    auto stride_op =
        rewriter.create<ConstantOp>(op->getLoc(), type, stride_attr);

    rewriter.replaceOpWithNewOp<TF::StridedSliceOp>(
        op, strided_slice_op.getType(), input, begin_op.getResult(),
        end_op.getResult(), stride_op.getResult(),
        rewriter.getIntegerAttr(attribute_type, new_begin_mask),
        rewriter.getIntegerAttr(attribute_type, new_end_mask),
        /*ellipsis_maks=*/rewriter.getI64IntegerAttr(0),
        rewriter.getIntegerAttr(attribute_type,
                                strided_slice_op.new_axis_mask()),
        rewriter.getIntegerAttr(attribute_type,
                                strided_slice_op.shrink_axis_mask()));
    return success();
  }

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    TF::StridedSliceOp strided_slice_op = llvm::cast<TF::StridedSliceOp>(op);

    // TODO(renjieliu): Consider expand the transformation for shrink mask as
    // well.
    if (strided_slice_op.shrink_axis_mask()) return failure();

    // Handle new axis mask.
    uint64_t new_axis_mask = strided_slice_op.new_axis_mask();
    if (new_axis_mask != 0) {
      return RewriteNewAxisMask(strided_slice_op, new_axis_mask, rewriter);
    }

    // Handle ellipsis mask.
    uint64_t ellipsis_mask = strided_slice_op.ellipsis_mask();
    if (ellipsis_mask != 0) {
      return RewriteEllipsisMask(strided_slice_op, ellipsis_mask, rewriter);
    }
    return failure();
  }
};

struct ConvertTFBroadcastTo : public RewritePattern {
  explicit ConvertTFBroadcastTo(MLIRContext *context)
      : RewritePattern(TF::BroadcastToOp::getOperationName(), 1, context) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    auto tf_broadcast_to_op = cast<TF::BroadcastToOp>(op);
    auto input_type = tf_broadcast_to_op.input().getType().cast<ShapedType>();
    auto output_type = tf_broadcast_to_op.output().getType().cast<ShapedType>();
    auto shape_type = tf_broadcast_to_op.shape().getType().cast<ShapedType>();
    Type element_type = input_type.getElementType();

    // Allow lowering when low dimension inputs are given and its type is F32 or
    // I32.
    if (!((output_type.hasRank() && output_type.getRank() <= 5) ||
          (shape_type.hasStaticShape() && shape_type.getRank() == 1 &&
           shape_type.getDimSize(0) <= 5)))
      return failure();

    if (!(element_type.isa<BFloat16Type, Float32Type>() ||
          element_type.isInteger(32)))
      return failure();

    auto status_or_const_op =
        CreateConstOpWithSingleValue(&rewriter, op->getLoc(), input_type, 1);
    if (!status_or_const_op.ok()) {
      return failure();
    }

    auto tf_fill_op = rewriter.create<TF::FillOp>(
        op->getLoc(), output_type, tf_broadcast_to_op.shape(),
        status_or_const_op.ValueOrDie());

    auto mul_op = rewriter.create<TF::MulOp>(
        op->getLoc(), output_type, tf_broadcast_to_op.input(), tf_fill_op);
    rewriter.replaceOp(op, mul_op.getResult());
    return success();
  }
};

// The below pattern is equivalent to the DRR rule below
// The checks are dependent on generated values, so we can't add
// the checks on intermediate values, ideally we should find equivalent
// checks that guarantees the resultant ops are valid.
// The extra conditions are the broadcasting conditions.
//
// The pattern lower FusedBatchNormV3 to arithmetic ops.
// Specifically, performs the following calculation:
//
//   (x - mean) * scale / sqrt(variance + epsilon) + offset
//
// Let multiplier = scale / sqrt(variance + epsilon),
// to compute
//   (x - mean) * scale / sqrt(variance + epsilon) + offset,
// is then to compute
//   (x * multiplier) + (offset - mean * multiplier).
//
// def : Pattern<
//     (TF_FusedBatchNormV3Op:$root
//         $x, $scale, $offset, $mean, $variance,
//         F32Attr:$epsilon, $exponential_avg_factor,
//         $data_format, FalseBoolAttr:$is_training),
//     [(TF_AddOp
//         (TF_MulOp
//             $x,
//             (TF_MulOp:$multiplier
//                 $scale,
//                 (TF_RsqrtOp
//                     (TF_AddOp $variance,
//                               (TF_ConstOp $epsilon))))),
//         (TF_SubOp $offset, (TF_MulOp $mean, $multiplier))),
//    // We already guaranteed that the last five results have no use so it does
//    // not matter what value we provide here for replacement.
//      /*batch_mean=*/(replaceWithValue $x),
//      /*batch_variance=*/(replaceWithValue $x),
//      /*reserve_space_1=*/(replaceWithValue $x),
//      /*reserve_space_2=*/(replaceWithValue $x),
//      /*reserve_space_3=*/(replaceWithValue $x)],
//     [(HasNoUseOf:$root__1), (HasNoUseOf:$root__2),
//      (HasNoUseOf:$root__3), (HasNoUseOf:$root__4),
//      (HasNoUseOf:$root__5), (AreBroadcastableTypes $multiplier, $x)]>;

struct FusedBatchNormV3Pat : public ::mlir::RewritePattern {
  explicit FusedBatchNormV3Pat(::mlir::MLIRContext *context)
      : ::mlir::RewritePattern(
            "tf.FusedBatchNormV3",
            {"tf.Add", "tf.Const", "tf.Mul", "tf.Rsqrt", "tf.Sub"}, 1,
            context) {}

  ::mlir::LogicalResult matchAndRewrite(
      ::mlir::Operation *fused_batch_norm,
      ::mlir::PatternRewriter &rewriter) const override {
    // Variables for capturing values and attributes used for creating ops
    Operation::operand_range mean(fused_batch_norm->getOperands());
    ::mlir::FloatAttr exponential_avg_factor;
    ::mlir::StringAttr data_format;
    ::mlir::TF::FusedBatchNormV3Op root;
    Operation::operand_range offset(fused_batch_norm->getOperands());
    Operation::operand_range x(fused_batch_norm->getOperands());
    Operation::operand_range scale(fused_batch_norm->getOperands());
    Operation::operand_range variance(fused_batch_norm->getOperands());
    ::mlir::FloatAttr epsilon;
    ::mlir::BoolAttr is_training;

    // Match
    auto fused_batch_norm_op =
        dyn_cast_or_null<::mlir::TF::FusedBatchNormV3Op>(fused_batch_norm);
    root = fused_batch_norm_op;
    x = fused_batch_norm_op.getODSOperands(0);
    scale = fused_batch_norm_op.getODSOperands(1);
    offset = fused_batch_norm_op.getODSOperands(2);
    mean = fused_batch_norm_op.getODSOperands(3);
    variance = fused_batch_norm_op.getODSOperands(4);
    {
      epsilon = fused_batch_norm_op.getAttrOfType<::mlir::FloatAttr>("epsilon");
      if (!epsilon)
        epsilon = rewriter.getFloatAttr(rewriter.getF32Type(), 0.0001f);

      if (!(((epsilon.isa<::mlir::FloatAttr>())) &&
            ((epsilon.cast<::mlir::FloatAttr>().getType().isF32())))) {
        return rewriter.notifyMatchFailure(
            fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
              diag << "op 'tf.FusedBatchNormV3' attribute 'epsilon' failed to "
                      "satisfy constraint: 32-bit float attribute";
            });
      }
    }
    {
      exponential_avg_factor =
          fused_batch_norm_op.getAttrOfType<::mlir::FloatAttr>(
              "exponential_avg_factor");
      if (!exponential_avg_factor)
        exponential_avg_factor =
            rewriter.getFloatAttr(rewriter.getF32Type(), 1.0f);
    }
    {
      data_format =
          fused_batch_norm_op.getAttrOfType<::mlir::StringAttr>("data_format");
      if (!data_format) data_format = rewriter.getStringAttr("NHWC");
    }
    {
      is_training =
          fused_batch_norm_op.getAttrOfType<::mlir::BoolAttr>("is_training");
      if (!is_training) is_training = rewriter.getBoolAttr(true);

      if (!((!is_training.getValue()))) {
        return rewriter.notifyMatchFailure(
            fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
              diag << "op 'tf.FusedBatchNormV3' attribute 'is_training' failed "
                      "to "
                      "satisfy constraint: FalseBoolAttr";
            });
      }
    }

    if (!(((*root.getODSResults(1).begin()).use_empty()))) {
      return rewriter.notifyMatchFailure(
          fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
            diag << "entities '' failed to satisfy constraint: has no use";
          });
    }

    if (!(((*root.getODSResults(2).begin()).use_empty()))) {
      return rewriter.notifyMatchFailure(
          fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
            diag << "entities '' failed to satisfy constraint: has no use";
          });
    }

    if (!(((*root.getODSResults(3).begin()).use_empty()))) {
      return rewriter.notifyMatchFailure(
          fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
            diag << "entities '' failed to satisfy constraint: has no use";
          });
    }

    if (!(((*root.getODSResults(4).begin()).use_empty()))) {
      return rewriter.notifyMatchFailure(
          fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
            diag << "entities '' failed to satisfy constraint: has no use";
          });
    }

    if (!(((*root.getODSResults(5).begin()).use_empty()))) {
      return rewriter.notifyMatchFailure(
          fused_batch_norm_op, [&](::mlir::Diagnostic &diag) {
            diag << "entities '' failed to satisfy constraint: has no use";
          });
    }
    // Rewrite
    auto odsLoc = rewriter.getFusedLoc({fused_batch_norm->getLoc()});
    ::llvm::SmallVector<::mlir::Value, 4> replace_values;
    ::mlir::TF::ConstOp epsilon_const_op;
    {
      epsilon_const_op =
          rewriter.create<::mlir::TF::ConstOp>(odsLoc,
                                               /*value=*/epsilon);
    }
    ::mlir::TF::AddOp add_op_1;
    {
      ::mlir::Value tblgen_value_0 = (*variance.begin());
      ::mlir::Value tblgen_value_1 =
          (*epsilon_const_op.getODSResults(0).begin());
      add_op_1 = rewriter.create<::mlir::TF::AddOp>(odsLoc,
                                                    /*x=*/tblgen_value_0,
                                                    /*y=*/tblgen_value_1);
      // We need to make sure the Add operands are broadcastable.
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(add_op_1)
              .value == LogicalResult::Failure) {
        return failure();
      }
    }
    ::mlir::TF::RsqrtOp rsqrt_op;
    {
      ::mlir::SmallVector<::mlir::Value, 4> tblgen_values;
      ::mlir::SmallVector<::mlir::NamedAttribute, 4> tblgen_attrs;
      tblgen_values.push_back((*add_op_1.getODSResults(0).begin()));
      rsqrt_op = rewriter.create<::mlir::TF::RsqrtOp>(odsLoc, tblgen_values,
                                                      tblgen_attrs);
    }
    ::mlir::TF::MulOp multiplier;
    {
      ::mlir::Value tblgen_value_0 = (*scale.begin());
      ::mlir::Value tblgen_value_1 = (*rsqrt_op.getODSResults(0).begin());
      // We need to make sure the Add operands are broadcastable.
      multiplier = rewriter.create<::mlir::TF::MulOp>(odsLoc,
                                                      /*x=*/tblgen_value_0,
                                                      /*y=*/tblgen_value_1);
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(multiplier)
              .value == LogicalResult::Failure) {
        return failure();
      }
    }
    ::mlir::TF::MulOp mul_op_1;
    {
      ::mlir::Value tblgen_value_0 = (*x.begin());
      ::mlir::Value tblgen_value_1 = (*multiplier.getODSResults(0).begin());
      mul_op_1 = rewriter.create<::mlir::TF::MulOp>(odsLoc,
                                                    /*x=*/tblgen_value_0,
                                                    /*y=*/tblgen_value_1);
      // We need to make sure the Mul operands are broadcastable.
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(mul_op_1)
              .value == LogicalResult::Failure) {
        return failure();
      }
    }
    ::mlir::TF::MulOp mul_op_2;
    {
      ::mlir::Value tblgen_value_0 = (*mean.begin());
      ::mlir::Value tblgen_value_1 = (*multiplier.getODSResults(0).begin());
      mul_op_2 = rewriter.create<::mlir::TF::MulOp>(odsLoc,
                                                    /*x=*/tblgen_value_0,
                                                    /*y=*/tblgen_value_1);
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(mul_op_2)
              .value == LogicalResult::Failure) {
        return failure();
      }
    }
    ::mlir::TF::SubOp sub_op;
    {
      ::mlir::Value tblgen_value_0 = (*offset.begin());
      ::mlir::Value tblgen_value_1 = (*mul_op_2.getODSResults(0).begin());
      sub_op = rewriter.create<::mlir::TF::SubOp>(odsLoc,
                                                  /*x=*/tblgen_value_0,
                                                  /*y=*/tblgen_value_1);
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(sub_op).value ==
          LogicalResult::Failure) {
        return failure();
      }
    }
    ::mlir::TF::AddOp add_op_2;
    {
      ::mlir::SmallVector<::mlir::Value, 4> tblgen_values;
      ::mlir::SmallVector<::mlir::NamedAttribute, 4> tblgen_attrs;
      tblgen_values.push_back((*mul_op_1.getODSResults(0).begin()));
      tblgen_values.push_back((*sub_op.getODSResults(0).begin()));
      ::mlir::SmallVector<::mlir::Type, 4> tblgen_types;
      for (auto v : fused_batch_norm_op.getODSResults(0)) {
        tblgen_types.push_back(v.getType());
      }
      add_op_2 = rewriter.create<::mlir::TF::AddOp>(
          odsLoc, tblgen_types, tblgen_values, tblgen_attrs);
      // We need to make sure the Add operands are broadcastable.
      if (mlir::OpTrait::impl::verifyCompatibleOperandBroadcast(add_op_2)
              .value == LogicalResult::Failure) {
        return failure();
      }
    }
    for (auto v :
         ::llvm::SmallVector<::mlir::Value, 4>{add_op_2.getODSResults(0)}) {
      replace_values.push_back(v);
    }
    for (auto v : ::llvm::SmallVector<::mlir::Value, 4>{x}) {
      replace_values.push_back(v);
    }
    for (auto v : ::llvm::SmallVector<::mlir::Value, 4>{x}) {
      replace_values.push_back(v);
    }
    for (auto v : ::llvm::SmallVector<::mlir::Value, 4>{x}) {
      replace_values.push_back(v);
    }
    for (auto v : ::llvm::SmallVector<::mlir::Value, 4>{x}) {
      replace_values.push_back(v);
    }
    for (auto v : ::llvm::SmallVector<::mlir::Value, 4>{x}) {
      replace_values.push_back(v);
    }
    rewriter.replaceOp(fused_batch_norm, replace_values);
    return success();
  };
};

#include "tensorflow/compiler/mlir/lite/transforms/generated_prepare_tf.inc"

// Returns success if all the operations in the `op`'s regions including `op`
// itself are legal in a TFLite pipeline.
LogicalResult ValidateOp(Operation *op) {
  bool has_illegal_ops = false;
  op->walk([&](Operation *op) {
    if (isa<TF::VariableV2Op>(op)) {
      has_illegal_ops = true;
      op->emitOpError() << "is illegal in a TFLite pipeline";
    }
  });

  return failure(has_illegal_ops);
}

// Converts a set of TF2XLA ops into pure TF ops for future legalizations as
// TF2XLA ops aren't supported by later stages.
LogicalResult ConvertTf2XlaOps(FuncOp func, MLIRContext *context) {
  ConversionTarget target(*context);
  target.addLegalDialect<StandardOpsDialect>();
  target.addLegalDialect<TF::TensorFlowDialect>();
  target.addLegalOp<ModuleOp>();
  target.addLegalOp<FuncOp>();
  target.addIllegalOp<TF::XlaConvOp>();
  target.addIllegalOp<TF::XlaGatherOp>();

  OwningRewritePatternList patterns;
  mhlo::PopulateLegalizeTfWithTf2XlaPatterns("XLA_CPU_JIT", patterns);
  mhlo::PopulateLegalizeTfPatterns(context, &patterns);
  TF::PopulateLegalizeHloToTfPatterns(&patterns, context);
  mhlo::GatherOp::getCanonicalizationPatterns(patterns, context);

  return applyPartialConversion(func, target, std::move(patterns));
}

// Convert rfft to rfft2d.
// The transformation pattern looks like below:
//
//    input     fft_len
//     \      /
//     rfft
//
//     ||
//     \/
//
//   input       fft_len
//    \            /
//   expand_dim    concat with [1] at the front
//      \         /
//     rfft_2d
//       |
//     squeeze
struct ConvertRfftToRfft2d : public RewritePattern {
  explicit ConvertRfftToRfft2d(MLIRContext *context)
      : RewritePattern(TF::RFFTOp::getOperationName(), 1, context) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    auto rfft_op = dyn_cast<TF::RFFTOp>(op);

    auto input = rfft_op.input();
    auto input_type = input.getType().dyn_cast_or_null<RankedTensorType>();
    if (!input_type) return failure();
    auto fft_len = rfft_op.fft_length();
    auto fft_len_type = fft_len.getType().dyn_cast_or_null<ShapedType>();
    if (!fft_len_type) return failure();

    auto output_type =
        rfft_op.getResult().getType().dyn_cast_or_null<RankedTensorType>();
    if (!output_type) return failure();

    // Expanded inputs.
    // Insert at -2 location.
    auto one_ele_type =
        mlir::RankedTensorType::get({1}, rewriter.getIntegerType(32));
    auto minus_two = CreateConstOpWithSingleValue(&rewriter, rfft_op.getLoc(),
                                                  one_ele_type, -2);

    SmallVector<int64_t, 4> expanded_input_shape;
    SmallVector<int64_t, 4> expanded_output_shape;
    int expanded_rank = input_type.getRank() + 1;
    int r = 0;
    for (int i = 0; i < expanded_rank; ++i) {
      if (i == expanded_rank - 2) {
        expanded_input_shape.push_back(1);
        expanded_output_shape.push_back(1);
      } else {
        expanded_input_shape.push_back(input_type.getDimSize(r));
        expanded_output_shape.push_back(output_type.getDimSize(r));
        r++;
      }
    }

    auto expaned_input_type = mlir::RankedTensorType::get(
        expanded_input_shape, input_type.getElementType());
    TF::ExpandDimsOp expanded_input = rewriter.create<TF::ExpandDimsOp>(
        rfft_op.getLoc(), expaned_input_type, input, minus_two->getResult());

    // Expanded fft_len.
    auto one_attr = mlir::DenseIntElementsAttr::get(one_ele_type, {1});

    auto one = rewriter.create<TF::ConstOp>(rfft_op.getLoc(), one_attr);

    auto zero = CreateConstOpWithSingleValue(&rewriter, rfft_op.getLoc(),
                                             one_ele_type, 0);

    auto expanded_fft_len_type =
        mlir::RankedTensorType::get({2}, fft_len_type.getElementType());

    TF::ConcatV2Op expanded_fft_len = rewriter.create<TF::ConcatV2Op>(
        rfft_op.getLoc(), expanded_fft_len_type,
        SmallVector<Value, 2>({one.getResult(), fft_len}), zero->getResult());

    // Insert the rfft_2d.
    auto rfft2d_out_type = mlir::RankedTensorType::get(
        expanded_output_shape, output_type.getElementType());
    TF::RFFT2DOp rfft2d = rewriter.create<TF::RFFT2DOp>(
        rfft_op.getLoc(), rfft2d_out_type, expanded_input.getResult(),
        expanded_fft_len.getResult());

    // Insert the squeeze op.
    auto squeeze_dim = rewriter.getI64ArrayAttr({-2});
    TF::SqueezeOp squeeze = rewriter.create<TF::SqueezeOp>(
        rfft_op.getLoc(), output_type, rfft2d.getResult(), squeeze_dim);

    rewriter.replaceOp(op, squeeze.getResult());

    return success();
  }
};

void PrepareTFPass::runOnFunction() {
  OwningRewritePatternList patterns, phase_2_patterns;
  auto func = getFunction();
  MLIRContext *ctx = &getContext();

  // Check illegal ops in a TFLite pipeline (e.g. trainning only ops) , since
  // PrepareTFPass is the very first TFLite pass in the pipeline.
  // TODO(jingpu): It might be better to split this check into its own pass
  // to make things more modular.
  if (failed(ValidateOp(func))) {
    func.emitError() << "tfl-prepare-tf pass failed.";
    signalPassFailure();
    return;
  }

  if (failed(ConvertTf2XlaOps(func, ctx))) {
    signalPassFailure();
    return;
  }

  // This pattern was intented to uses TFL QDQs to preserve the quantization
  // parameters from the TF Quant ops, thus this pattern should run with the
  // first `applyPatternsGreedily` method, which would otherwise removes the
  // TF FakeQuant ops by the constant folding.
  patterns.insert<PreparePerTensorFakeQuant, PreparePerChannelFakeQuant,
                  PreparePerTensorFakeQuantWithMinMaxArgs>(ctx);

  // This pattern will try to identify and optimize for dilated convolution.
  // e.g. Patterns like "SpaceToBatchND -> Conv2D -> BatchToSpaceND" will be
  // replaced with a single Conv op with dilation parameter.
  patterns.insert<ConvertTFDilatedConvOp<TF::Conv2DOp>, FusedBatchNormV3Pat,
                  ConvertTFDilatedConvOp<TF::DepthwiseConv2dNativeOp>>(ctx);

  TFL::populateWithGenerated(ctx, patterns);
  // TODO(karimnosseir): Split to separate pass probably after
  // deciding on long term plan for this optimization.
  // This will allow optimizing any TF_Mul->TF_Conv in the graph
  // and any expanded from FusedBatchNorm. We need to do this
  // before converting TF_Conv to TFL_Conv
  applyPatternsAndFoldGreedily(func, std::move(patterns));

  // Load the generated pattern again, so new quantization pass-through
  // will be applied.
  TFL::populateWithGenerated(ctx, phase_2_patterns);
  if (unfold_batch_matmul_) {
    phase_2_patterns.insert<TF::ConvertTFBatchMatMulOp<TF::BatchMatMulOp>,
                            TF::ConvertTFBatchMatMulOp<TF::BatchMatMulV2Op>>(
        ctx);
  }
  phase_2_patterns.insert<TF::ConvertTFEinsumOp, ConvertTFBroadcastTo,
                          ConvertTFConv2D, ConvertTFDepthwiseConv2dNative,
                          ConvertTFStridedSlice, ConvertRfftToRfft2d>(ctx);
  applyPatternsAndFoldGreedily(func, std::move(phase_2_patterns));
}

}  // namespace

// Creates an instance of the TensorFlow Lite dialect PrepareTF pass.
std::unique_ptr<OperationPass<FuncOp>> CreatePrepareTFPass(
    bool unfold_batch_matmul) {
  return std::make_unique<PrepareTFPass>(unfold_batch_matmul);
}

static PassRegistration<PrepareTFPass> pass(
    "tfl-prepare-tf", "Prepare TF for legalization to TensorFlow Lite dialect");

}  // namespace TFL
}  // namespace mlir

/**
 * ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 * ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 * ******************************************************************************
 *
 * Compatibility layer for glslang spirv.hpp11 migration
 * This header provides backward compatibility for code written against the old
 * spirv.hpp
 */

#pragma once

#include "third_party/glslang/SPIRV/spirv.hpp11"
#include "third_party/glslang/SPIRV/spvUtil.h"

namespace spv {

// Backward compatibility for Op codes - old style was spv::OpXXX, new is
// spv::Op::OpXXX We provide the old style as direct aliases in the spv
// namespace
// Op code compatibility macros
#define OpNop Op::OpNop
#define OpUndef Op::OpUndef
#define OpSourceContinued Op::OpSourceContinued
#define OpSource Op::OpSource
#define OpSourceExtension Op::OpSourceExtension
#define OpName Op::OpName
#define OpMemberName Op::OpMemberName
#define OpString Op::OpString
#define OpExtension Op::OpExtension
#define OpExtInstImport Op::OpExtInstImport
#define OpExtInst Op::OpExtInst
#define OpMemoryModel Op::OpMemoryModel
#define OpEntryPoint Op::OpEntryPoint
#define OpExecutionMode Op::OpExecutionMode
#define OpCapability Op::OpCapability
#define OpTypeVoid Op::OpTypeVoid
#define OpTypeBool Op::OpTypeBool
#define OpTypeInt Op::OpTypeInt
#define OpTypeFloat Op::OpTypeFloat
#define OpTypeVector Op::OpTypeVector
#define OpTypeMatrix Op::OpTypeMatrix
#define OpTypeImage Op::OpTypeImage
#define OpTypeSampler Op::OpTypeSampler
#define OpTypeSampledImage Op::OpTypeSampledImage
#define OpTypeArray Op::OpTypeArray
#define OpTypeRuntimeArray Op::OpTypeRuntimeArray
#define OpTypeStruct Op::OpTypeStruct
#define OpTypeOpaque Op::OpTypeOpaque
#define OpTypePointer Op::OpTypePointer
#define OpTypeFunction Op::OpTypeFunction
#define OpTypeEvent Op::OpTypeEvent
#define OpTypeDeviceEvent Op::OpTypeDeviceEvent
#define OpTypeReserveId Op::OpTypeReserveId
#define OpTypeQueue Op::OpTypeQueue
#define OpTypePipe Op::OpTypePipe
#define OpTypeForwardPointer Op::OpTypeForwardPointer
#define OpConstantTrue Op::OpConstantTrue
#define OpConstantFalse Op::OpConstantFalse
#define OpConstant Op::OpConstant
#define OpConstantComposite Op::OpConstantComposite
#define OpConstantSampler Op::OpConstantSampler
#define OpConstantNull Op::OpConstantNull
#define OpSpecConstantTrue Op::OpSpecConstantTrue
#define OpSpecConstantFalse Op::OpSpecConstantFalse
#define OpSpecConstant Op::OpSpecConstant
#define OpSpecConstantComposite Op::OpSpecConstantComposite
#define OpSpecConstantOp Op::OpSpecConstantOp
#define OpVariable Op::OpVariable
#define OpImageTexelPointer Op::OpImageTexelPointer
#define OpLoad Op::OpLoad
#define OpStore Op::OpStore
#define OpCopyMemory Op::OpCopyMemory
#define OpCopyMemorySized Op::OpCopyMemorySized
#define OpAccessChain Op::OpAccessChain
#define OpInBoundsAccessChain Op::OpInBoundsAccessChain
#define OpPtrAccessChain Op::OpPtrAccessChain
#define OpArrayLength Op::OpArrayLength
#define OpGenericPtrMemSemantics Op::OpGenericPtrMemSemantics
#define OpInBoundsPtrAccessChain Op::OpInBoundsPtrAccessChain
#define OpDecorate Op::OpDecorate
#define OpMemberDecorate Op::OpMemberDecorate
#define OpDecorationGroup Op::OpDecorationGroup
#define OpGroupDecorate Op::OpGroupDecorate
#define OpGroupMemberDecorate Op::OpGroupMemberDecorate
#define OpVectorExtractDynamic Op::OpVectorExtractDynamic
#define OpVectorInsertDynamic Op::OpVectorInsertDynamic
#define OpVectorShuffle Op::OpVectorShuffle
#define OpCompositeConstruct Op::OpCompositeConstruct
#define OpCompositeExtract Op::OpCompositeExtract
#define OpCompositeInsert Op::OpCompositeInsert
#define OpCopyObject Op::OpCopyObject
#define OpTranspose Op::OpTranspose
#define OpSampledImage Op::OpSampledImage
#define OpImageSampleImplicitLod Op::OpImageSampleImplicitLod
#define OpImageSampleExplicitLod Op::OpImageSampleExplicitLod
#define OpImageSampleDrefImplicitLod Op::OpImageSampleDrefImplicitLod
#define OpImageSampleDrefExplicitLod Op::OpImageSampleDrefExplicitLod
#define OpImageSampleProjImplicitLod Op::OpImageSampleProjImplicitLod
#define OpImageSampleProjExplicitLod Op::OpImageSampleProjExplicitLod
#define OpImageSampleProjDrefImplicitLod Op::OpImageSampleProjDrefImplicitLod
#define OpImageSampleProjDrefExplicitLod Op::OpImageSampleProjDrefExplicitLod
#define OpImageFetch Op::OpImageFetch
#define OpImageGather Op::OpImageGather
#define OpImageDrefGather Op::OpImageDrefGather
#define OpImageRead Op::OpImageRead
#define OpImageWrite Op::OpImageWrite
#define OpImage Op::OpImage
#define OpImageQueryFormat Op::OpImageQueryFormat
#define OpImageQueryOrder Op::OpImageQueryOrder
#define OpImageQuerySizeLod Op::OpImageQuerySizeLod
#define OpImageQuerySize Op::OpImageQuerySize
#define OpImageQueryLod Op::OpImageQueryLod
#define OpImageQueryLevels Op::OpImageQueryLevels
#define OpImageQuerySamples Op::OpImageQuerySamples
#define OpConvertFToU Op::OpConvertFToU
#define OpConvertFToS Op::OpConvertFToS
#define OpFConvertSTo Op::OpFConvertSTo
#define OpConvertSToF Op::OpConvertSToF
#define OpConvertUToF Op::OpConvertUToF
#define OpUConvert Op::OpUConvert
#define OpSConvert Op::OpSConvert
#define OpFConvert Op::OpFConvert
#define OpQuantizeToF16 Op::OpQuantizeToF16
#define OpConvertPtrToU Op::OpConvertPtrToU
#define OpConvertUToPtr Op::OpConvertUToPtr
#define OpPtrCastToGeneric Op::OpPtrCastToGeneric
#define OpGenericCastToPtr Op::OpGenericCastToPtr
#define OpGenericCastToPtrExplicit Op::OpGenericCastToPtrExplicit
#define OpBitcast Op::OpBitcast
#define OpSNegate Op::OpSNegate
#define OpFNegate Op::OpFNegate
#define OpIAdd Op::OpIAdd
#define OpFAdd Op::OpFAdd
#define OpISub Op::OpISub
#define OpFSub Op::OpFSub
#define OpIMul Op::OpIMul
#define OpFMul Op::OpFMul
#define OpUDiv Op::OpUDiv
#define OpSDiv Op::OpSDiv
#define OpFDiv Op::OpFDiv
#define OpUMod Op::OpUMod
#define OpSRem Op::OpSRem
#define OpSMod Op::OpSMod
#define OpFRem Op::OpFRem
#define OpFMod Op::OpFMod
#define OpVectorTimesScalar Op::OpVectorTimesScalar
#define OpMatrixTimesScalar Op::OpMatrixTimesScalar
#define OpVectorTimesMatrix Op::OpVectorTimesMatrix
#define OpMatrixTimesVector Op::OpMatrixTimesVector
#define OpMatrixTimesMatrix Op::OpMatrixTimesMatrix
#define OpOuterProduct Op::OpOuterProduct
#define OpDot Op::OpDot
#define OpIAddCarry Op::OpIAddCarry
#define OpISubBorrow Op::OpISubBorrow
#define OpUMulExtended Op::OpUMulExtended
#define OpSMulExtended Op::OpSMulExtended
#define OpShiftRightLogical Op::OpShiftRightLogical
#define OpShiftRightArithmetic Op::OpShiftRightArithmetic
#define OpShiftLeftLogical Op::OpShiftLeftLogical
#define OpBitwiseOr Op::OpBitwiseOr
#define OpBitwiseXor Op::OpBitwiseXor
#define OpBitwiseAnd Op::OpBitwiseAnd
#define OpNot Op::OpNot
#define OpBitFieldInsert Op::OpBitFieldInsert
#define OpBitFieldSExtract Op::OpBitFieldSExtract
#define OpBitFieldUExtract Op::OpBitFieldUExtract
#define OpBitReverse Op::OpBitReverse
#define OpBitCount Op::OpBitCount
#define OpAny Op::OpAny
#define OpAll Op::OpAll
#define OpIsNan Op::OpIsNan
#define OpIsInf Op::OpIsInf
#define OpIsFinite Op::OpIsFinite
#define OpIsNormal Op::OpIsNormal
#define OpSignBitSet Op::OpSignBitSet
#define OpLessOrGreater Op::OpLessOrGreater
#define OpOrdered Op::OpOrdered
#define OpUnordered Op::OpUnordered
#define OpLogicalEqual Op::OpLogicalEqual
#define OpLogicalNotEqual Op::OpLogicalNotEqual
#define OpLogicalOr Op::OpLogicalOr
#define OpLogicalAnd Op::OpLogicalAnd
#define OpLogicalNot Op::OpLogicalNot
#define OpSelect Op::OpSelect
#define OpIEqual Op::OpIEqual
#define OpINotEqual Op::OpINotEqual
#define OpUGreaterThan Op::OpUGreaterThan
#define OpUGreaterThanEqual Op::OpUGreaterThanEqual
#define OpULessThan Op::OpULessThan
#define OpULessThanEqual Op::OpULessThanEqual
#define OpSGreaterThan Op::OpSGreaterThan
#define OpSGreaterThanEqual Op::OpSGreaterThanEqual
#define OpSLessThan Op::OpSLessThan
#define OpSLessThanEqual Op::OpSLessThanEqual
#define OpFOrdEqual Op::OpFOrdEqual
#define OpFOrdNotEqual Op::OpFOrdNotEqual
#define OpFOrdLessThan Op::OpFOrdLessThan
#define OpFOrdGreaterThan Op::OpFOrdGreaterThan
#define OpFOrdLessThanEqual Op::OpFOrdLessThanEqual
#define OpFOrdGreaterThanEqual Op::OpFOrdGreaterThanEqual
#define OpFUnordEqual Op::OpFUnordEqual
#define OpFUnordNotEqual Op::OpFUnordNotEqual
#define OpFUnordLessThan Op::OpFUnordLessThan
#define OpFUnordGreaterThan Op::OpFUnordGreaterThan
#define OpFUnordLessThanEqual Op::OpFUnordLessThanEqual
#define OpFUnordGreaterThanEqual Op::OpFUnordGreaterThanEqual
#define OpDPdx Op::OpDPdx
#define OpDPdy Op::OpDPdy
#define OpFwidth Op::OpFwidth
#define OpDPdxFine Op::OpDPdxFine
#define OpDPdyFine Op::OpDPdyFine
#define OpFwidthFine Op::OpFwidthFine
#define OpDPdxCoarse Op::OpDPdxCoarse
#define OpDPdyCoarse Op::OpDPdyCoarse
#define OpFwidthCoarse Op::OpFwidthCoarse
#define OpEmitVertex Op::OpEmitVertex
#define OpEndPrimitive Op::OpEndPrimitive
#define OpEmitStreamVertex Op::OpEmitStreamVertex
#define OpEndStreamPrimitive Op::OpEndStreamPrimitive
#define OpControlBarrier Op::OpControlBarrier
#define OpMemoryBarrier Op::OpMemoryBarrier
#define OpAtomicLoad Op::OpAtomicLoad
#define OpAtomicStore Op::OpAtomicStore
#define OpAtomicExchange Op::OpAtomicExchange
#define OpAtomicCompareExchange Op::OpAtomicCompareExchange
#define OpAtomicCompareExchangeWeak Op::OpAtomicCompareExchangeWeak
#define OpAtomicIIncrement Op::OpAtomicIIncrement
#define OpAtomicIDecrement Op::OpAtomicIDecrement
#define OpAtomicIAdd Op::OpAtomicIAdd
#define OpAtomicISub Op::OpAtomicISub
#define OpAtomicSMin Op::OpAtomicSMin
#define OpAtomicUMin Op::OpAtomicUMin
#define OpAtomicSMax Op::OpAtomicSMax
#define OpAtomicUMax Op::OpAtomicUMax
#define OpAtomicAnd Op::OpAtomicAnd
#define OpAtomicOr Op::OpAtomicOr
#define OpAtomicXor Op::OpAtomicXor
#define OpPhi Op::OpPhi
#define OpLoopMerge Op::OpLoopMerge
#define OpSelectionMerge Op::OpSelectionMerge
#define OpLabel Op::OpLabel
#define OpBranch Op::OpBranch
#define OpBranchConditional Op::OpBranchConditional
#define OpSwitch Op::OpSwitch
#define OpKill Op::OpKill
#define OpReturn Op::OpReturn
#define OpReturnValue Op::OpReturnValue
#define OpUnreachable Op::OpUnreachable
#define OpLifetimeStart Op::OpLifetimeStart
#define OpLifetimeStop Op::OpLifetimeStop
#define OpGroupAsyncCopy Op::OpGroupAsyncCopy
#define OpGroupWaitEvents Op::OpGroupWaitEvents
#define OpGroupAll Op::OpGroupAll
#define OpGroupAny Op::OpGroupAny
#define OpGroupBroadcast Op::OpGroupBroadcast
#define OpGroupIAdd Op::OpGroupIAdd
#define OpGroupFAdd Op::OpGroupFAdd
#define OpGroupFMin Op::OpGroupFMin
#define OpGroupUMin Op::OpGroupUMin
#define OpGroupSMin Op::OpGroupSMin
#define OpGroupFMax Op::OpGroupFMax
#define OpGroupUMax Op::OpGroupUMax
#define OpGroupSMax Op::OpGroupSMax
#define OpReadPipe Op::OpReadPipe
#define OpWritePipe Op::OpWritePipe
#define OpReservedReadPipe Op::OpReservedReadPipe
#define OpReservedWritePipe Op::OpReservedWritePipe
#define OpReserveReadPipePackets Op::OpReserveReadPipePackets
#define OpReserveWritePipePackets Op::OpReserveWritePipePackets
#define OpCommitReadPipe Op::OpCommitReadPipe
#define OpCommitWritePipe Op::OpCommitWritePipe
#define OpIsValidReserveId Op::OpIsValidReserveId
#define OpGetNumPipePackets Op::OpGetNumPipePackets
#define OpGetMaxPipePackets Op::OpGetMaxPipePackets
#define OpGroupReserveReadPipePackets Op::OpGroupReserveReadPipePackets
#define OpGroupReserveWritePipePackets Op::OpGroupReserveWritePipePackets
#define OpGroupCommitReadPipe Op::OpGroupCommitReadPipe
#define OpGroupCommitWritePipe Op::OpGroupCommitWritePipe
#define OpEnqueueMarker Op::OpEnqueueMarker
#define OpEnqueueKernel Op::OpEnqueueKernel
#define OpGetKernelNDrangeSubGroupCount Op::OpGetKernelNDrangeSubGroupCount
#define OpGetKernelNDrangeMaxSubGroupSize Op::OpGetKernelNDrangeMaxSubGroupSize
#define OpGetKernelWorkGroupSize Op::OpGetKernelWorkGroupSize
#define OpGetKernelPreferredWorkGroupSizeMultiple \
  Op::OpGetKernelPreferredWorkGroupSizeMultiple
#define OpRetainEvent Op::OpRetainEvent
#define OpReleaseEvent Op::OpReleaseEvent
#define OpCreateUserEvent Op::OpCreateUserEvent
#define OpIsValidEvent Op::OpIsValidEvent
#define OpSetUserEventStatus Op::OpSetUserEventStatus
#define OpCaptureEventProfilingInfo Op::OpCaptureEventProfilingInfo
#define OpGetDefaultQueue Op::OpGetDefaultQueue
#define OpBuildNDRange Op::OpBuildNDRange
#define OpImageSparseSampleImplicitLod Op::OpImageSparseSampleImplicitLod
#define OpImageSparseSampleExplicitLod Op::OpImageSparseSampleExplicitLod
#define OpImageSparseSampleDrefImplicitLod \
  Op::OpImageSparseSampleDrefImplicitLod
#define OpImageSparseSampleDrefExplicitLod \
  Op::OpImageSparseSampleDrefExplicitLod
#define OpImageSparseSampleProjImplicitLod \
  Op::OpImageSparseSampleProjImplicitLod
#define OpImageSparseSampleProjExplicitLod \
  Op::OpImageSparseSampleProjExplicitLod
#define OpImageSparseSampleProjDrefImplicitLod \
  Op::OpImageSparseSampleProjDrefImplicitLod
#define OpImageSparseSampleProjDrefExplicitLod \
  Op::OpImageSparseSampleProjDrefExplicitLod
#define OpImageSparseFetch Op::OpImageSparseFetch
#define OpImageSparseGather Op::OpImageSparseGather
#define OpImageSparseDrefGather Op::OpImageSparseDrefGather
#define OpImageSparseTexelsResident Op::OpImageSparseTexelsResident
#define OpNoLine Op::OpNoLine
#define OpAtomicFlagTestAndSet Op::OpAtomicFlagTestAndSet
#define OpAtomicFlagClear Op::OpAtomicFlagClear
#define OpImageSampleFootprintNV Op::OpImageSampleFootprintNV

// Direct aliases for Op codes in the main spv namespace
// This allows code like spv::OpFAdd to work without qualification

// Inject old-style OpXXX names into spv namespace

// Backward compatibility for Decoration names
// Only include decorations that actually exist
#define DecorationArrayStride Decoration::ArrayStride
#define DecorationBlock Decoration::Block
#define DecorationOffset Decoration::Offset
#define DecorationDescriptorSet Decoration::DescriptorSet
#define DecorationBinding Decoration::Binding
#define DecorationBuiltIn Decoration::BuiltIn
#define DecorationFlat Decoration::Flat
#define DecorationInvariant Decoration::Invariant
#define DecorationUniform Decoration::Uniform
#define DecorationSaturatedConversion Decoration::SaturatedConversion
#define DecorationStream Decoration::Stream
#define DecorationLocation Decoration::Location
#define DecorationComponent Decoration::Component
#define DecorationIndex Decoration::Index
#define DecorationAlignment Decoration::Alignment
#define DecorationXfbBuffer Decoration::XfbBuffer
#define DecorationXfbStride Decoration::XfbStride
#define DecorationFuncParamAttr Decoration::FuncParamAttr
#define DecorationFPRoundingMode Decoration::FPRoundingMode
#define DecorationFPFastMathMode Decoration::FPFastMathMode
#define DecorationLinkageAttributes Decoration::LinkageAttributes
#define DecorationNoContraction Decoration::NoContraction
#define DecorationInputAttachmentIndex Decoration::InputAttachmentIndex
#define DecorationAlignmentId Decoration::AlignmentId
#define DecorationMaxByteOffsetId Decoration::MaxByteOffsetId

// Backward compatibility for ImageOperands
#define ImageOperandsGradMask ImageOperandsMask::Grad
#define ImageOperandsLodMask ImageOperandsMask::Lod
#define ImageOperandsMaskNone ImageOperandsMask::MaskNone

// Backward compatibility for StorageClass
#define StorageClassUniformConstant StorageClass::UniformConstant
#define StorageClassInput StorageClass::Input
#define StorageClassUniform StorageClass::Uniform
#define StorageClassOutput StorageClass::Output
#define StorageClassFunction StorageClass::Function

// Backward compatibility for SelectionControl
#define SelectionControlMaskNone SelectionControlMask::MaskNone
#define SelectionControlDontFlattenMask SelectionControlMask::DontFlatten

// Backward compatibility for Capability
#define CapabilityShader Capability::Shader
#define CapabilityGeometry Capability::Geometry
#define CapabilityTessellation Capability::Tessellation
#define CapabilityClipDistance Capability::ClipDistance
#define CapabilityCullDistance Capability::CullDistance
#define CapabilityDerivativeControl Capability::DerivativeControl
#define CapabilityDenormFlushToZero Capability::DenormFlushToZero
#define CapabilitySignedZeroInfNanPreserve Capability::SignedZeroInfNanPreserve
#define CapabilityRoundingModeRTE Capability::RoundingModeRTE
#define CapabilityFragmentShaderSampleInterlockEXT \
  Capability::FragmentShaderSampleInterlockEXT
#define CapabilityFragmentShaderPixelInterlockEXT \
  Capability::FragmentShaderPixelInterlockEXT
#define CapabilitySampleRateShading Capability::SampleRateShading
#define CapabilityDemoteToHelperInvocationEXT \
  Capability::DemoteToHelperInvocationEXT
#define CapabilityStencilExportEXT Capability::StencilExportEXT
#define CapabilityFragmentBarycentricKHR Capability::FragmentBarycentricKHR

// Backward compatibility for AddressingModel
#define AddressingModelLogical AddressingModel::Logical

// Backward compatibility for MemoryModel
#define MemoryModelGLSL450 MemoryModel::GLSL450

// Backward compatibility for SourceLanguage
#define SourceLanguageUnknown SourceLanguage::Unknown

// Backward compatibility for ExecutionModel
#define ExecutionModelVertex ExecutionModel::Vertex
#define ExecutionModelTessellationEvaluation \
  ExecutionModel::TessellationEvaluation
#define ExecutionModelGeometry ExecutionModel::Geometry
#define ExecutionModelFragment ExecutionModel::Fragment
#define ExecutionModelGLCompute ExecutionModel::GLCompute

// Backward compatibility for ExecutionMode
#define ExecutionModeOriginUpperLeft ExecutionMode::OriginUpperLeft
#define ExecutionModeEarlyFragmentTests ExecutionMode::EarlyFragmentTests
#define ExecutionModeDenormFlushToZero ExecutionMode::DenormFlushToZero
#define ExecutionModeSignedZeroInfNanPreserve \
  ExecutionMode::SignedZeroInfNanPreserve
#define ExecutionModeRoundingModeRTE ExecutionMode::RoundingModeRTE
#define ExecutionModePixelInterlockOrderedEXT \
  ExecutionMode::PixelInterlockOrderedEXT
#define ExecutionModeSampleInterlockOrderedEXT \
  ExecutionMode::SampleInterlockOrderedEXT
#define ExecutionModeInputPoints ExecutionMode::InputPoints
#define ExecutionModeTriangles ExecutionMode::Triangles
#define ExecutionModeQuads ExecutionMode::Quads
#define ExecutionModeIsolines ExecutionMode::Isolines
#define ExecutionModeInputLinesAdjacency ExecutionMode::InputLinesAdjacency
#define ExecutionModeOutputTriangleStrip ExecutionMode::OutputTriangleStrip
#define ExecutionModeInvocations ExecutionMode::Invocations
#define ExecutionModeOutputVertices ExecutionMode::OutputVertices
#define ExecutionModeSpacingEqual ExecutionMode::SpacingEqual
#define ExecutionModeSpacingFractionalEven ExecutionMode::SpacingFractionalEven
#define ExecutionModeSpacingFractionalOdd ExecutionMode::SpacingFractionalOdd
#define ExecutionModeVertexOrderCw ExecutionMode::VertexOrderCw
#define ExecutionModeVertexOrderCcw ExecutionMode::VertexOrderCcw
#define ExecutionModeDepthReplacing ExecutionMode::DepthReplacing
#define ExecutionModeDepthGreater ExecutionMode::DepthGreater
#define ExecutionModeDepthLess ExecutionMode::DepthLess
#define ExecutionModeStencilRefReplacingEXT \
  ExecutionMode::StencilRefReplacingEXT
#define ExecutionModeLocalSize ExecutionMode::LocalSize

// Backward compatibility for Decoration
#define DecorationPerVertexKHR Decoration::PerVertexKHR
#define DecorationRestrict Decoration::Restrict
#define DecorationNonWritable Decoration::NonWritable
#define DecorationBufferBlock Decoration::BufferBlock
#define DecorationCoherent Decoration::Coherent
#define DecorationCentroid Decoration::Centroid
#define DecorationSample Decoration::Sample
#define DecorationSpecId Decoration::SpecId
#define DecorationNonReadable Decoration::NonReadable

// Backward compatibility for LoopControl
#define LoopControlDontUnrollMask LoopControlMask::DontUnroll

// Backward compatibility for StorageClass (additional)
#define StorageClassStorageBuffer StorageClass::StorageBuffer
#define StorageClassPushConstant StorageClass::PushConstant

// Backward compatibility for Dim
#define Dim2D Dim::Dim2D
#define Dim3D Dim::Dim3D
#define DimCube Dim::Cube

// Backward compatibility for ImageFormat
#define ImageFormatUnknown ImageFormat::Unknown

// Backward compatibility for OpDemoteToHelperInvocationEXT
#define OpDemoteToHelperInvocationEXT Op::OpDemoteToHelperInvocationEXT
#define OpBeginInvocationInterlockEXT Op::OpBeginInvocationInterlockEXT
#define OpEndInvocationInterlockEXT Op::OpEndInvocationInterlockEXT
#define OpIsHelperInvocationEXT Op::OpIsHelperInvocationEXT

// Backward compatibility for Scope
#define ScopeDevice Scope::Device

// Fragment barycentric BuiltIn values
#define BuiltInBaryCoordKHR BuiltIn::BaryCoordKHR
#define BuiltInBaryCoordNoPerspKHR BuiltIn::BaryCoordNoPerspKHR

}  // namespace spv

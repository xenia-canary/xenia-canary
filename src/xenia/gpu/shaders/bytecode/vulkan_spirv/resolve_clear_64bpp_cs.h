// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 25245
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %5663 "main" %gl_GlobalInvocationID
               OpExecutionMode %5663 LocalSize 8 8 1
               OpDecorate %_struct_1014 Block
               OpMemberDecorate %_struct_1014 0 Offset 0
               OpMemberDecorate %_struct_1014 1 Offset 8
               OpMemberDecorate %_struct_1014 2 Offset 12
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_runtimearr_v4uint ArrayStride 16
               OpDecorate %_struct_1972 BufferBlock
               OpMemberDecorate %_struct_1972 0 NonReadable
               OpMemberDecorate %_struct_1972 0 Offset 0
               OpDecorate %3271 NonReadable
               OpDecorate %3271 Binding 0
               OpDecorate %3271 DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
%_ptr_Function_uint = OpTypePointer Function %uint
     %v2uint = OpTypeVector %uint 2
        %209 = OpTypeFunction %v2uint %_ptr_Function_uint
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
       %bool = OpTypeBool
%_ptr_Function_bool = OpTypePointer Function %bool
       %2523 = OpTypeFunction %uint %_ptr_Function_v2uint %_ptr_Function_uint %_ptr_Function_bool %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_bool %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_v2uint
%_struct_1432 = OpTypeStruct %uint %uint %bool %uint %uint %uint %v2uint %v2uint %v2uint %uint %v2uint
       %1119 = OpTypeFunction %_struct_1432
     %uint_2 = OpConstant %uint 2
     %uint_1 = OpConstant %uint 1
       %1837 = OpConstantComposite %v2uint %uint_2 %uint_1
     %v2bool = OpTypeVector %bool 2
     %uint_0 = OpConstant %uint 0
       %1807 = OpConstantComposite %v2uint %uint_0 %uint_0
       %1828 = OpConstantComposite %v2uint %uint_1 %uint_1
       %1816 = OpConstantComposite %v2uint %uint_1 %uint_0
    %uint_80 = OpConstant %uint 80
    %uint_16 = OpConstant %uint 16
       %2719 = OpConstantComposite %v2uint %uint_80 %uint_16
        %int = OpTypeInt 32 1
%_ptr_Function_int = OpTypePointer Function %int
  %uint_2048 = OpConstant %uint 2048
%_struct_1014 = OpTypeStruct %v2uint %uint %uint
%_ptr_PushConstant__struct_1014 = OpTypePointer PushConstant %_struct_1014
       %4495 = OpVariable %_ptr_PushConstant__struct_1014 PushConstant
      %int_1 = OpConstant %int 1
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
      %int_2 = OpConstant %int 2
%_ptr_Function__struct_1432 = OpTypePointer Function %_struct_1432
      %int_0 = OpConstant %int 0
  %uint_1023 = OpConstant %uint 1023
    %uint_10 = OpConstant %uint 10
     %uint_3 = OpConstant %uint 3
  %uint_4096 = OpConstant %uint 4096
      %int_3 = OpConstant %int 3
    %uint_13 = OpConstant %uint 13
  %uint_2047 = OpConstant %uint 2047
      %int_4 = OpConstant %int 4
    %uint_24 = OpConstant %uint 24
    %uint_15 = OpConstant %uint 15
      %int_5 = OpConstant %int 5
    %uint_28 = OpConstant %uint 28
      %int_6 = OpConstant %int 6
      %int_7 = OpConstant %int 7
      %int_8 = OpConstant %int 8
     %uint_4 = OpConstant %uint 4
       %1855 = OpConstantComposite %v2uint %uint_0 %uint_4
       %1856 = OpConstantComposite %v2uint %uint_4 %uint_1
      %int_9 = OpConstant %int 9
     %uint_5 = OpConstant %uint 5
     %int_10 = OpConstant %int 10
%_ptr_PushConstant_v2uint = OpTypePointer PushConstant %v2uint
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
%_ptr_Input_uint = OpTypePointer Input %uint
       %1834 = OpConstantComposite %v2uint %uint_3 %uint_0
       %true = OpConstantTrue %bool
      %false = OpConstantFalse %bool
     %v4uint = OpTypeVector %uint 4
%_runtimearr_v4uint = OpTypeRuntimeArray %v4uint
%_struct_1972 = OpTypeStruct %_runtimearr_v4uint
%_ptr_Uniform__struct_1972 = OpTypePointer Uniform %_struct_1972
       %3271 = OpVariable %_ptr_Uniform__struct_1972 Uniform
%_ptr_Uniform_v4uint = OpTypePointer Uniform %v4uint
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_8 %uint_8 %uint_1
       %5663 = OpFunction %void None %1282
      %24915 = OpLabel
       %4442 = OpVariable %_ptr_Function__struct_1432 Function
       %6024 = OpVariable %_ptr_Function_v2uint Function
      %14447 = OpVariable %_ptr_Function_uint Function
       %3166 = OpVariable %_ptr_Function_uint Function
      %14448 = OpVariable %_ptr_Function_v2uint Function
      %14449 = OpVariable %_ptr_Function_uint Function
      %14450 = OpVariable %_ptr_Function_bool Function
      %14451 = OpVariable %_ptr_Function_uint Function
      %14452 = OpVariable %_ptr_Function_uint Function
      %14453 = OpVariable %_ptr_Function_bool Function
      %14454 = OpVariable %_ptr_Function_uint Function
      %14455 = OpVariable %_ptr_Function_uint Function
       %5786 = OpVariable %_ptr_Function_v2uint Function
       %4283 = OpVariable %_ptr_Function_uint Function
      %11357 = OpFunctionCall %_struct_1432 %3667
               OpStore %4442 %11357
      %13396 = OpAccessChain %_ptr_Function_uint %4442 %int_1
       %7815 = OpLoad %uint %13396
               OpStore %14447 %7815
       %7437 = OpFunctionCall %v2uint %5657 %14447
       %6518 = OpUGreaterThanEqual %v2bool %7437 %1837
      %20800 = OpSelect %v2uint %6518 %1828 %1807
               OpStore %6024 %20800
      %13683 = OpAccessChain %_ptr_Input_uint %gl_GlobalInvocationID %uint_0
       %7372 = OpLoad %uint %13683
      %20154 = OpAccessChain %_ptr_Function_uint %4442 %int_9
      %22408 = OpLoad %uint %20154
      %22643 = OpAccessChain %_ptr_Function_uint %6024 %uint_0
      %20963 = OpLoad %uint %22643
      %11370 = OpShiftLeftLogical %uint %22408 %20963
      %12163 = OpUGreaterThanEqual %bool %7372 %11370
               OpSelectionMerge %9994 DontFlatten
               OpBranchConditional %12163 %21300 %9994
      %21300 = OpLabel
               OpReturn
       %9994 = OpLabel
      %11406 = OpLoad %v3uint %gl_GlobalInvocationID
      %18201 = OpVectorShuffle %v2uint %11406 %11406 0 1
      %12228 = OpShiftLeftLogical %v2uint %18201 %1834
       %7368 = OpAccessChain %_ptr_Function_v2uint %4442 %int_8
       %9231 = OpLoad %v2uint %7368
      %11697 = OpLoad %v2uint %6024
      %12587 = OpShiftLeftLogical %v2uint %9231 %11697
      %17040 = OpIAdd %v2uint %12228 %12587
               OpStore %14448 %17040
       %9616 = OpAccessChain %_ptr_Function_uint %4442 %int_3
       %7910 = OpLoad %uint %9616
               OpStore %14449 %7910
               OpStore %14450 %true
      %14200 = OpAccessChain %_ptr_Function_uint %4442 %int_0
       %7911 = OpLoad %uint %14200
               OpStore %14451 %7911
               OpStore %14452 %uint_0
               OpStore %14453 %false
               OpStore %14454 %uint_1
               OpStore %14455 %uint_0
      %14201 = OpAccessChain %_ptr_Function_v2uint %4442 %int_6
       %7816 = OpLoad %v2uint %14201
               OpStore %5786 %7816
      %21015 = OpFunctionCall %uint %4615 %14448 %14449 %14450 %14451 %14452 %14453 %14454 %14455 %5786
       %7280 = OpShiftRightLogical %uint %21015 %uint_2
               OpStore %3166 %7280
               OpStore %4283 %uint_0
               OpBranch %18559
      %18559 = OpLabel
               OpLoopMerge %19578 %10123 Unroll
               OpBranch %17677
      %17677 = OpLabel
      %17953 = OpLoad %uint %4283
      %18597 = OpULessThan %bool %17953 %uint_4
               OpBranchConditional %18597 %23058 %19578
      %23058 = OpLabel
      %14037 = OpLoad %uint %3166
      %12257 = OpLoad %uint %4283
      %14918 = OpIAdd %uint %14037 %12257
      %24539 = OpAccessChain %_ptr_Function_v2uint %4442 %int_10
      %13056 = OpLoad %v2uint %24539
      %19129 = OpVectorShuffle %v4uint %13056 %13056 0 1 0 1
       %9012 = OpAccessChain %_ptr_Uniform_v4uint %3271 %int_0 %14918
               OpStore %9012 %19129
               OpBranch %10123
      %10123 = OpLabel
      %22685 = OpLoad %uint %4283
      %24842 = OpIAdd %uint %22685 %int_1
               OpStore %4283 %24842
               OpBranch %18559
      %19578 = OpLabel
               OpReturn
               OpFunctionEnd
       %5657 = OpFunction %v2uint None %209
       %3189 = OpFunctionParameter %_ptr_Function_uint
      %12273 = OpLabel
      %22752 = OpLoad %uint %3189
      %10674 = OpCompositeConstruct %v2uint %22752 %22752
               OpReturnValue %10674
               OpFunctionEnd
       %4615 = OpFunction %uint None %2523
       %3356 = OpFunctionParameter %_ptr_Function_v2uint
       %5318 = OpFunctionParameter %_ptr_Function_uint
       %3206 = OpFunctionParameter %_ptr_Function_bool
       %4011 = OpFunctionParameter %_ptr_Function_uint
       %5551 = OpFunctionParameter %_ptr_Function_uint
       %4574 = OpFunctionParameter %_ptr_Function_bool
       %5508 = OpFunctionParameter %_ptr_Function_uint
       %5693 = OpFunctionParameter %_ptr_Function_uint
       %3226 = OpFunctionParameter %_ptr_Function_v2uint
       %6291 = OpLabel
       %3512 = OpVariable %_ptr_Function_v2uint Function
      %19693 = OpVariable %_ptr_Function_uint Function
      %12247 = OpVariable %_ptr_Function_uint Function
       %3238 = OpVariable %_ptr_Function_v2uint Function
       %5948 = OpVariable %_ptr_Function_v2uint Function
       %3820 = OpVariable %_ptr_Function_v2uint Function
       %4739 = OpVariable %_ptr_Function_uint Function
      %12285 = OpVariable %_ptr_Function_int Function
       %3559 = OpVariable %_ptr_Function_uint Function
      %14643 = OpLoad %v2uint %3356
      %13877 = OpLoad %uint %5551
               OpStore %19693 %13877
      %24308 = OpFunctionCall %v2uint %5657 %19693
       %6864 = OpUGreaterThanEqual %v2bool %24308 %1837
      %14432 = OpSelect %v2uint %6864 %1828 %1807
       %9373 = OpShiftLeftLogical %v2uint %14643 %14432
               OpStore %3512 %9373
      %11421 = OpLoad %uint %5693
               OpStore %12247 %11421
      %19157 = OpFunctionCall %v2uint %5657 %12247
      %11026 = OpShiftRightLogical %v2uint %19157 %1816
       %6636 = OpCompositeConstruct %v2uint %uint_1 %uint_1
       %7260 = OpBitwiseAnd %v2uint %11026 %6636
      %23199 = OpLoad %v2uint %3512
      %15065 = OpIAdd %v2uint %23199 %7260
               OpStore %3512 %15065
       %8834 = OpLoad %v2uint %3226
      %16185 = OpIMul %v2uint %2719 %8834
               OpStore %3238 %16185
      %18228 = OpLoad %v2uint %3238
      %12557 = OpLoad %uint %5508
      %21335 = OpCompositeConstruct %v2uint %12557 %uint_0
       %7613 = OpShiftRightLogical %v2uint %18228 %21335
               OpStore %5948 %7613
      %11444 = OpLoad %v2uint %3512
      %19778 = OpLoad %v2uint %5948
      %21368 = OpUDiv %v2uint %11444 %19778
               OpStore %3820 %21368
      %24850 = OpAccessChain %_ptr_Function_uint %3820 %uint_1
       %7213 = OpLoad %uint %24850
      %19372 = OpLoad %uint %4011
      %18088 = OpIMul %uint %7213 %19372
      %15817 = OpAccessChain %_ptr_Function_uint %3820 %uint_0
      %24600 = OpLoad %uint %15817
      %11452 = OpIAdd %uint %18088 %24600
      %24126 = OpLoad %uint %5318
       %6231 = OpIAdd %uint %24126 %11452
               OpStore %5318 %6231
      %22447 = OpLoad %v2uint %3820
      %20105 = OpLoad %v2uint %5948
      %20091 = OpIMul %v2uint %22447 %20105
      %16327 = OpLoad %v2uint %3512
      %24756 = OpISub %v2uint %16327 %20091
               OpStore %3512 %24756
      %13195 = OpLoad %bool %4574
               OpSelectionMerge %6726 None
               OpBranchConditional %13195 %9929 %6726
       %9929 = OpLabel
      %18461 = OpAccessChain %_ptr_Function_uint %5948 %uint_0
      %10200 = OpLoad %uint %18461
      %22403 = OpShiftRightLogical %uint %10200 %uint_1
               OpStore %4739 %22403
      %16624 = OpAccessChain %_ptr_Function_uint %3512 %uint_0
      %14112 = OpLoad %uint %16624
      %10574 = OpBitcast %int %14112
      %10379 = OpAccessChain %_ptr_Function_uint %3512 %uint_0
      %19272 = OpLoad %uint %10379
      %15564 = OpLoad %uint %4739
      %21050 = OpUGreaterThanEqual %bool %19272 %15564
               OpSelectionMerge %7923 None
               OpBranchConditional %21050 %11818 %7847
      %11818 = OpLabel
      %19971 = OpLoad %uint %4739
      %21718 = OpBitcast %int %19971
      %22465 = OpSNegate %int %21718
               OpStore %12285 %22465
               OpBranch %7923
       %7847 = OpLabel
      %21177 = OpLoad %uint %4739
      %17272 = OpBitcast %int %21177
               OpStore %12285 %17272
               OpBranch %7923
       %7923 = OpLabel
      %21663 = OpLoad %int %12285
       %8420 = OpIAdd %int %10574 %21663
       %8255 = OpBitcast %uint %8420
      %19386 = OpAccessChain %_ptr_Function_uint %3512 %uint_0
               OpStore %19386 %8255
               OpBranch %6726
       %6726 = OpLabel
      %12294 = OpLoad %uint %5318
       %7947 = OpAccessChain %_ptr_Function_uint %3238 %uint_0
       %7594 = OpLoad %uint %7947
      %19227 = OpAccessChain %_ptr_Function_uint %3238 %uint_1
      %16423 = OpLoad %uint %19227
      %22413 = OpIMul %uint %7594 %16423
      %11523 = OpIMul %uint %12294 %22413
      %23454 = OpAccessChain %_ptr_Function_uint %3512 %uint_1
      %16428 = OpLoad %uint %23454
      %19228 = OpAccessChain %_ptr_Function_uint %5948 %uint_0
      %15150 = OpLoad %uint %19228
      %14857 = OpIMul %uint %16428 %15150
      %15818 = OpAccessChain %_ptr_Function_uint %3512 %uint_0
      %24601 = OpLoad %uint %15818
      %12744 = OpIAdd %uint %14857 %24601
      %13616 = OpLoad %uint %5508
      %22452 = OpShiftLeftLogical %uint %12744 %13616
      %14764 = OpIAdd %uint %11523 %22452
               OpStore %3559 %14764
      %11582 = OpLoad %bool %3206
               OpSelectionMerge %10317 None
               OpBranchConditional %11582 %9930 %10317
       %9930 = OpLabel
      %16010 = OpAccessChain %_ptr_Function_uint %3238 %uint_0
      %13427 = OpLoad %uint %16010
      %19229 = OpAccessChain %_ptr_Function_uint %3238 %uint_1
      %16424 = OpLoad %uint %19229
      %22337 = OpIMul %uint %13427 %16424
      %13659 = OpIMul %uint %22337 %uint_2048
      %23950 = OpLoad %uint %3559
       %9119 = OpUMod %uint %23950 %13659
               OpStore %3559 %9119
               OpBranch %10317
      %10317 = OpLabel
      %11326 = OpLoad %uint %3559
               OpReturnValue %11326
               OpFunctionEnd
       %3667 = OpFunction %_struct_1432 None %1119
      %13312 = OpLabel
      %22085 = OpVariable %_ptr_Function__struct_1432 Function
      %22228 = OpVariable %_ptr_Function_uint Function
      %14782 = OpVariable %_ptr_Function_uint Function
      %14896 = OpVariable %_ptr_Function_uint Function
      %13782 = OpVariable %_ptr_Function_uint Function
      %22605 = OpAccessChain %_ptr_PushConstant_uint %4495 %int_1
      %21130 = OpLoad %uint %22605
      %23035 = OpAccessChain %_ptr_PushConstant_uint %4495 %int_2
      %18708 = OpLoad %uint %23035
      %15082 = OpBitwiseAnd %uint %21130 %uint_1023
      %20321 = OpAccessChain %_ptr_Function_uint %22085 %int_0
               OpStore %20321 %15082
      %13602 = OpShiftRightLogical %uint %21130 %uint_10
      %24373 = OpBitwiseAnd %uint %13602 %uint_3
      %14983 = OpAccessChain %_ptr_Function_uint %22085 %int_1
               OpStore %14983 %24373
      %15623 = OpBitwiseAnd %uint %21130 %uint_4096
      %17223 = OpINotEqual %bool %15623 %uint_0
      %10650 = OpAccessChain %_ptr_Function_bool %22085 %int_2
               OpStore %10650 %17223
      %13603 = OpShiftRightLogical %uint %21130 %uint_13
      %24374 = OpBitwiseAnd %uint %13603 %uint_2047
      %14888 = OpAccessChain %_ptr_Function_uint %22085 %int_3
               OpStore %14888 %24374
      %13604 = OpShiftRightLogical %uint %21130 %uint_24
      %24375 = OpBitwiseAnd %uint %13604 %uint_15
      %14889 = OpAccessChain %_ptr_Function_uint %22085 %int_4
               OpStore %14889 %24375
      %13605 = OpShiftRightLogical %uint %21130 %uint_28
      %24376 = OpBitwiseAnd %uint %13605 %uint_1
      %12380 = OpAccessChain %_ptr_Function_uint %22085 %int_5
               OpStore %12380 %24376
               OpStore %22228 %uint_1
      %22174 = OpFunctionCall %v2uint %5657 %22228
      %22064 = OpAccessChain %_ptr_Function_v2uint %22085 %int_6
               OpStore %22064 %22174
               OpStore %14782 %uint_0
      %22175 = OpFunctionCall %v2uint %5657 %14782
      %22065 = OpAccessChain %_ptr_Function_v2uint %22085 %int_7
               OpStore %22065 %22175
               OpStore %14896 %18708
      %18928 = OpFunctionCall %v2uint %5657 %14896
       %7520 = OpShiftRightLogical %v2uint %18928 %1855
               OpStore %13782 %uint_1
      %22370 = OpFunctionCall %v2uint %5657 %13782
       %8478 = OpShiftLeftLogical %v2uint %22370 %1856
      %23232 = OpCompositeConstruct %v2uint %uint_1 %uint_1
      %17951 = OpISub %v2uint %8478 %23232
      %17835 = OpBitwiseAnd %v2uint %7520 %17951
      %12511 = OpCompositeConstruct %v2uint %uint_3 %uint_3
       %7685 = OpShiftLeftLogical %v2uint %17835 %12511
       %6630 = OpAccessChain %_ptr_Function_v2uint %22085 %int_6
      %13865 = OpLoad %v2uint %6630
      %17411 = OpIMul %v2uint %7685 %13865
      %12976 = OpAccessChain %_ptr_Function_v2uint %22085 %int_8
               OpStore %12976 %17411
      %13606 = OpShiftRightLogical %uint %18708 %uint_5
      %24354 = OpBitwiseAnd %uint %13606 %uint_2047
      %13883 = OpAccessChain %_ptr_Function_uint %22085 %int_6 %uint_0
      %13057 = OpLoad %uint %13883
      %17412 = OpIMul %uint %24354 %13057
      %10525 = OpAccessChain %_ptr_Function_uint %22085 %int_9
               OpStore %10525 %17412
      %13783 = OpAccessChain %_ptr_PushConstant_v2uint %4495 %int_0
       %7726 = OpLoad %v2uint %13783
      %20240 = OpAccessChain %_ptr_Function_v2uint %22085 %int_10
               OpStore %20240 %7726
      %25244 = OpLoad %_struct_1432 %22085
               OpReturnValue %25244
               OpFunctionEnd
#endif

const uint32_t resolve_clear_64bpp_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x0000629D, 0x00000000, 0x00020011,
    0x00000001, 0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E,
    0x00000000, 0x0003000E, 0x00000000, 0x00000001, 0x0006000F, 0x00000005,
    0x0000161F, 0x6E69616D, 0x00000000, 0x00000F48, 0x00060010, 0x0000161F,
    0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030047, 0x000003F6,
    0x00000002, 0x00050048, 0x000003F6, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x000003F6, 0x00000001, 0x00000023, 0x00000008, 0x00050048,
    0x000003F6, 0x00000002, 0x00000023, 0x0000000C, 0x00040047, 0x00000F48,
    0x0000000B, 0x0000001C, 0x00040047, 0x000007DC, 0x00000006, 0x00000010,
    0x00030047, 0x000007B4, 0x00000003, 0x00040048, 0x000007B4, 0x00000000,
    0x00000019, 0x00050048, 0x000007B4, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000CC7, 0x00000019, 0x00040047, 0x00000CC7, 0x00000021,
    0x00000000, 0x00040047, 0x00000CC7, 0x00000022, 0x00000000, 0x00040047,
    0x00000AC7, 0x0000000B, 0x00000019, 0x00020013, 0x00000008, 0x00030021,
    0x00000502, 0x00000008, 0x00040015, 0x0000000B, 0x00000020, 0x00000000,
    0x00040020, 0x00000288, 0x00000007, 0x0000000B, 0x00040017, 0x00000011,
    0x0000000B, 0x00000002, 0x00040021, 0x000000D1, 0x00000011, 0x00000288,
    0x00040020, 0x0000028E, 0x00000007, 0x00000011, 0x00020014, 0x00000009,
    0x00040020, 0x00000286, 0x00000007, 0x00000009, 0x000C0021, 0x000009DB,
    0x0000000B, 0x0000028E, 0x00000288, 0x00000286, 0x00000288, 0x00000288,
    0x00000286, 0x00000288, 0x00000288, 0x0000028E, 0x000D001E, 0x00000598,
    0x0000000B, 0x0000000B, 0x00000009, 0x0000000B, 0x0000000B, 0x0000000B,
    0x00000011, 0x00000011, 0x00000011, 0x0000000B, 0x00000011, 0x00030021,
    0x0000045F, 0x00000598, 0x0004002B, 0x0000000B, 0x00000A10, 0x00000002,
    0x0004002B, 0x0000000B, 0x00000A0D, 0x00000001, 0x0005002C, 0x00000011,
    0x0000072D, 0x00000A10, 0x00000A0D, 0x00040017, 0x0000000F, 0x00000009,
    0x00000002, 0x0004002B, 0x0000000B, 0x00000A0A, 0x00000000, 0x0005002C,
    0x00000011, 0x0000070F, 0x00000A0A, 0x00000A0A, 0x0005002C, 0x00000011,
    0x00000724, 0x00000A0D, 0x00000A0D, 0x0005002C, 0x00000011, 0x00000718,
    0x00000A0D, 0x00000A0A, 0x0004002B, 0x0000000B, 0x00000AFA, 0x00000050,
    0x0004002B, 0x0000000B, 0x00000A3A, 0x00000010, 0x0005002C, 0x00000011,
    0x00000A9F, 0x00000AFA, 0x00000A3A, 0x00040015, 0x0000000C, 0x00000020,
    0x00000001, 0x00040020, 0x00000289, 0x00000007, 0x0000000C, 0x0004002B,
    0x0000000B, 0x00000A84, 0x00000800, 0x0005001E, 0x000003F6, 0x00000011,
    0x0000000B, 0x0000000B, 0x00040020, 0x00000673, 0x00000009, 0x000003F6,
    0x0004003B, 0x00000673, 0x0000118F, 0x00000009, 0x0004002B, 0x0000000C,
    0x00000A0E, 0x00000001, 0x00040020, 0x0000028A, 0x00000009, 0x0000000B,
    0x0004002B, 0x0000000C, 0x00000A11, 0x00000002, 0x00040020, 0x00000815,
    0x00000007, 0x00000598, 0x0004002B, 0x0000000C, 0x00000A0B, 0x00000000,
    0x0004002B, 0x0000000B, 0x00000A44, 0x000003FF, 0x0004002B, 0x0000000B,
    0x00000A28, 0x0000000A, 0x0004002B, 0x0000000B, 0x00000A13, 0x00000003,
    0x0004002B, 0x0000000B, 0x00000AFE, 0x00001000, 0x0004002B, 0x0000000C,
    0x00000A14, 0x00000003, 0x0004002B, 0x0000000B, 0x00000A31, 0x0000000D,
    0x0004002B, 0x0000000B, 0x00000A81, 0x000007FF, 0x0004002B, 0x0000000C,
    0x00000A17, 0x00000004, 0x0004002B, 0x0000000B, 0x00000A52, 0x00000018,
    0x0004002B, 0x0000000B, 0x00000A37, 0x0000000F, 0x0004002B, 0x0000000C,
    0x00000A1A, 0x00000005, 0x0004002B, 0x0000000B, 0x00000A5E, 0x0000001C,
    0x0004002B, 0x0000000C, 0x00000A1D, 0x00000006, 0x0004002B, 0x0000000C,
    0x00000A20, 0x00000007, 0x0004002B, 0x0000000C, 0x00000A23, 0x00000008,
    0x0004002B, 0x0000000B, 0x00000A16, 0x00000004, 0x0005002C, 0x00000011,
    0x0000073F, 0x00000A0A, 0x00000A16, 0x0005002C, 0x00000011, 0x00000740,
    0x00000A16, 0x00000A0D, 0x0004002B, 0x0000000C, 0x00000A26, 0x00000009,
    0x0004002B, 0x0000000B, 0x00000A19, 0x00000005, 0x0004002B, 0x0000000C,
    0x00000A29, 0x0000000A, 0x00040020, 0x0000028F, 0x00000009, 0x00000011,
    0x00040017, 0x00000014, 0x0000000B, 0x00000003, 0x00040020, 0x00000291,
    0x00000001, 0x00000014, 0x0004003B, 0x00000291, 0x00000F48, 0x00000001,
    0x00040020, 0x0000028B, 0x00000001, 0x0000000B, 0x0005002C, 0x00000011,
    0x0000072A, 0x00000A13, 0x00000A0A, 0x00030029, 0x00000009, 0x00000786,
    0x0003002A, 0x00000009, 0x00000787, 0x00040017, 0x00000017, 0x0000000B,
    0x00000004, 0x0003001D, 0x000007DC, 0x00000017, 0x0003001E, 0x000007B4,
    0x000007DC, 0x00040020, 0x00000A32, 0x00000002, 0x000007B4, 0x0004003B,
    0x00000A32, 0x00000CC7, 0x00000002, 0x00040020, 0x00000294, 0x00000002,
    0x00000017, 0x0004002B, 0x0000000B, 0x00000A22, 0x00000008, 0x0006002C,
    0x00000014, 0x00000AC7, 0x00000A22, 0x00000A22, 0x00000A0D, 0x00050036,
    0x00000008, 0x0000161F, 0x00000000, 0x00000502, 0x000200F8, 0x00006153,
    0x0004003B, 0x00000815, 0x0000115A, 0x00000007, 0x0004003B, 0x0000028E,
    0x00001788, 0x00000007, 0x0004003B, 0x00000288, 0x0000386F, 0x00000007,
    0x0004003B, 0x00000288, 0x00000C5E, 0x00000007, 0x0004003B, 0x0000028E,
    0x00003870, 0x00000007, 0x0004003B, 0x00000288, 0x00003871, 0x00000007,
    0x0004003B, 0x00000286, 0x00003872, 0x00000007, 0x0004003B, 0x00000288,
    0x00003873, 0x00000007, 0x0004003B, 0x00000288, 0x00003874, 0x00000007,
    0x0004003B, 0x00000286, 0x00003875, 0x00000007, 0x0004003B, 0x00000288,
    0x00003876, 0x00000007, 0x0004003B, 0x00000288, 0x00003877, 0x00000007,
    0x0004003B, 0x0000028E, 0x0000169A, 0x00000007, 0x0004003B, 0x00000288,
    0x000010BB, 0x00000007, 0x00040039, 0x00000598, 0x00002C5D, 0x00000E53,
    0x0003003E, 0x0000115A, 0x00002C5D, 0x00050041, 0x00000288, 0x00003454,
    0x0000115A, 0x00000A0E, 0x0004003D, 0x0000000B, 0x00001E87, 0x00003454,
    0x0003003E, 0x0000386F, 0x00001E87, 0x00050039, 0x00000011, 0x00001D0D,
    0x00001619, 0x0000386F, 0x000500AE, 0x0000000F, 0x00001976, 0x00001D0D,
    0x0000072D, 0x000600A9, 0x00000011, 0x00005140, 0x00001976, 0x00000724,
    0x0000070F, 0x0003003E, 0x00001788, 0x00005140, 0x00050041, 0x0000028B,
    0x00003573, 0x00000F48, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00001CCC,
    0x00003573, 0x00050041, 0x00000288, 0x00004EBA, 0x0000115A, 0x00000A26,
    0x0004003D, 0x0000000B, 0x00005788, 0x00004EBA, 0x00050041, 0x00000288,
    0x00005873, 0x00001788, 0x00000A0A, 0x0004003D, 0x0000000B, 0x000051E3,
    0x00005873, 0x000500C4, 0x0000000B, 0x00002C6A, 0x00005788, 0x000051E3,
    0x000500AE, 0x00000009, 0x00002F83, 0x00001CCC, 0x00002C6A, 0x000300F7,
    0x0000270A, 0x00000002, 0x000400FA, 0x00002F83, 0x00005334, 0x0000270A,
    0x000200F8, 0x00005334, 0x000100FD, 0x000200F8, 0x0000270A, 0x0004003D,
    0x00000014, 0x00002C8E, 0x00000F48, 0x0007004F, 0x00000011, 0x00004719,
    0x00002C8E, 0x00002C8E, 0x00000000, 0x00000001, 0x000500C4, 0x00000011,
    0x00002FC4, 0x00004719, 0x0000072A, 0x00050041, 0x0000028E, 0x00001CC8,
    0x0000115A, 0x00000A23, 0x0004003D, 0x00000011, 0x0000240F, 0x00001CC8,
    0x0004003D, 0x00000011, 0x00002DB1, 0x00001788, 0x000500C4, 0x00000011,
    0x0000312B, 0x0000240F, 0x00002DB1, 0x00050080, 0x00000011, 0x00004290,
    0x00002FC4, 0x0000312B, 0x0003003E, 0x00003870, 0x00004290, 0x00050041,
    0x00000288, 0x00002590, 0x0000115A, 0x00000A14, 0x0004003D, 0x0000000B,
    0x00001EE6, 0x00002590, 0x0003003E, 0x00003871, 0x00001EE6, 0x0003003E,
    0x00003872, 0x00000786, 0x00050041, 0x00000288, 0x00003778, 0x0000115A,
    0x00000A0B, 0x0004003D, 0x0000000B, 0x00001EE7, 0x00003778, 0x0003003E,
    0x00003873, 0x00001EE7, 0x0003003E, 0x00003874, 0x00000A0A, 0x0003003E,
    0x00003875, 0x00000787, 0x0003003E, 0x00003876, 0x00000A0D, 0x0003003E,
    0x00003877, 0x00000A0A, 0x00050041, 0x0000028E, 0x00003779, 0x0000115A,
    0x00000A1D, 0x0004003D, 0x00000011, 0x00001E88, 0x00003779, 0x0003003E,
    0x0000169A, 0x00001E88, 0x000D0039, 0x0000000B, 0x00005217, 0x00001207,
    0x00003870, 0x00003871, 0x00003872, 0x00003873, 0x00003874, 0x00003875,
    0x00003876, 0x00003877, 0x0000169A, 0x000500C2, 0x0000000B, 0x00001C70,
    0x00005217, 0x00000A10, 0x0003003E, 0x00000C5E, 0x00001C70, 0x0003003E,
    0x000010BB, 0x00000A0A, 0x000200F9, 0x0000487F, 0x000200F8, 0x0000487F,
    0x000400F6, 0x00004C7A, 0x0000278B, 0x00000001, 0x000200F9, 0x0000450D,
    0x000200F8, 0x0000450D, 0x0004003D, 0x0000000B, 0x00004621, 0x000010BB,
    0x000500B0, 0x00000009, 0x000048A5, 0x00004621, 0x00000A16, 0x000400FA,
    0x000048A5, 0x00005A12, 0x00004C7A, 0x000200F8, 0x00005A12, 0x0004003D,
    0x0000000B, 0x000036D5, 0x00000C5E, 0x0004003D, 0x0000000B, 0x00002FE1,
    0x000010BB, 0x00050080, 0x0000000B, 0x00003A46, 0x000036D5, 0x00002FE1,
    0x00050041, 0x0000028E, 0x00005FDB, 0x0000115A, 0x00000A29, 0x0004003D,
    0x00000011, 0x00003300, 0x00005FDB, 0x0009004F, 0x00000017, 0x00004AB9,
    0x00003300, 0x00003300, 0x00000000, 0x00000001, 0x00000000, 0x00000001,
    0x00060041, 0x00000294, 0x00002334, 0x00000CC7, 0x00000A0B, 0x00003A46,
    0x0003003E, 0x00002334, 0x00004AB9, 0x000200F9, 0x0000278B, 0x000200F8,
    0x0000278B, 0x0004003D, 0x0000000B, 0x0000589D, 0x000010BB, 0x00050080,
    0x0000000B, 0x0000610A, 0x0000589D, 0x00000A0E, 0x0003003E, 0x000010BB,
    0x0000610A, 0x000200F9, 0x0000487F, 0x000200F8, 0x00004C7A, 0x000100FD,
    0x00010038, 0x00050036, 0x00000011, 0x00001619, 0x00000000, 0x000000D1,
    0x00030037, 0x00000288, 0x00000C75, 0x000200F8, 0x00002FF1, 0x0004003D,
    0x0000000B, 0x000058E0, 0x00000C75, 0x00050050, 0x00000011, 0x000029B2,
    0x000058E0, 0x000058E0, 0x000200FE, 0x000029B2, 0x00010038, 0x00050036,
    0x0000000B, 0x00001207, 0x00000000, 0x000009DB, 0x00030037, 0x0000028E,
    0x00000D1C, 0x00030037, 0x00000288, 0x000014C6, 0x00030037, 0x00000286,
    0x00000C86, 0x00030037, 0x00000288, 0x00000FAB, 0x00030037, 0x00000288,
    0x000015AF, 0x00030037, 0x00000286, 0x000011DE, 0x00030037, 0x00000288,
    0x00001584, 0x00030037, 0x00000288, 0x0000163D, 0x00030037, 0x0000028E,
    0x00000C9A, 0x000200F8, 0x00001893, 0x0004003B, 0x0000028E, 0x00000DB8,
    0x00000007, 0x0004003B, 0x00000288, 0x00004CED, 0x00000007, 0x0004003B,
    0x00000288, 0x00002FD7, 0x00000007, 0x0004003B, 0x0000028E, 0x00000CA6,
    0x00000007, 0x0004003B, 0x0000028E, 0x0000173C, 0x00000007, 0x0004003B,
    0x0000028E, 0x00000EEC, 0x00000007, 0x0004003B, 0x00000288, 0x00001283,
    0x00000007, 0x0004003B, 0x00000289, 0x00002FFD, 0x00000007, 0x0004003B,
    0x00000288, 0x00000DE7, 0x00000007, 0x0004003D, 0x00000011, 0x00003933,
    0x00000D1C, 0x0004003D, 0x0000000B, 0x00003635, 0x000015AF, 0x0003003E,
    0x00004CED, 0x00003635, 0x00050039, 0x00000011, 0x00005EF4, 0x00001619,
    0x00004CED, 0x000500AE, 0x0000000F, 0x00001AD0, 0x00005EF4, 0x0000072D,
    0x000600A9, 0x00000011, 0x00003860, 0x00001AD0, 0x00000724, 0x0000070F,
    0x000500C4, 0x00000011, 0x0000249D, 0x00003933, 0x00003860, 0x0003003E,
    0x00000DB8, 0x0000249D, 0x0004003D, 0x0000000B, 0x00002C9D, 0x0000163D,
    0x0003003E, 0x00002FD7, 0x00002C9D, 0x00050039, 0x00000011, 0x00004AD5,
    0x00001619, 0x00002FD7, 0x000500C2, 0x00000011, 0x00002B12, 0x00004AD5,
    0x00000718, 0x00050050, 0x00000011, 0x000019EC, 0x00000A0D, 0x00000A0D,
    0x000500C7, 0x00000011, 0x00001C5C, 0x00002B12, 0x000019EC, 0x0004003D,
    0x00000011, 0x00005A9F, 0x00000DB8, 0x00050080, 0x00000011, 0x00003AD9,
    0x00005A9F, 0x00001C5C, 0x0003003E, 0x00000DB8, 0x00003AD9, 0x0004003D,
    0x00000011, 0x00002282, 0x00000C9A, 0x00050084, 0x00000011, 0x00003F39,
    0x00000A9F, 0x00002282, 0x0003003E, 0x00000CA6, 0x00003F39, 0x0004003D,
    0x00000011, 0x00004734, 0x00000CA6, 0x0004003D, 0x0000000B, 0x0000310D,
    0x00001584, 0x00050050, 0x00000011, 0x00005357, 0x0000310D, 0x00000A0A,
    0x000500C2, 0x00000011, 0x00001DBD, 0x00004734, 0x00005357, 0x0003003E,
    0x0000173C, 0x00001DBD, 0x0004003D, 0x00000011, 0x00002CB4, 0x00000DB8,
    0x0004003D, 0x00000011, 0x00004D42, 0x0000173C, 0x00050086, 0x00000011,
    0x00005378, 0x00002CB4, 0x00004D42, 0x0003003E, 0x00000EEC, 0x00005378,
    0x00050041, 0x00000288, 0x00006112, 0x00000EEC, 0x00000A0D, 0x0004003D,
    0x0000000B, 0x00001C2D, 0x00006112, 0x0004003D, 0x0000000B, 0x00004BAC,
    0x00000FAB, 0x00050084, 0x0000000B, 0x000046A8, 0x00001C2D, 0x00004BAC,
    0x00050041, 0x00000288, 0x00003DC9, 0x00000EEC, 0x00000A0A, 0x0004003D,
    0x0000000B, 0x00006018, 0x00003DC9, 0x00050080, 0x0000000B, 0x00002CBC,
    0x000046A8, 0x00006018, 0x0004003D, 0x0000000B, 0x00005E3E, 0x000014C6,
    0x00050080, 0x0000000B, 0x00001857, 0x00005E3E, 0x00002CBC, 0x0003003E,
    0x000014C6, 0x00001857, 0x0004003D, 0x00000011, 0x000057AF, 0x00000EEC,
    0x0004003D, 0x00000011, 0x00004E89, 0x0000173C, 0x00050084, 0x00000011,
    0x00004E7B, 0x000057AF, 0x00004E89, 0x0004003D, 0x00000011, 0x00003FC7,
    0x00000DB8, 0x00050082, 0x00000011, 0x000060B4, 0x00003FC7, 0x00004E7B,
    0x0003003E, 0x00000DB8, 0x000060B4, 0x0004003D, 0x00000009, 0x0000338B,
    0x000011DE, 0x000300F7, 0x00001A46, 0x00000000, 0x000400FA, 0x0000338B,
    0x000026C9, 0x00001A46, 0x000200F8, 0x000026C9, 0x00050041, 0x00000288,
    0x0000481D, 0x0000173C, 0x00000A0A, 0x0004003D, 0x0000000B, 0x000027D8,
    0x0000481D, 0x000500C2, 0x0000000B, 0x00005783, 0x000027D8, 0x00000A0D,
    0x0003003E, 0x00001283, 0x00005783, 0x00050041, 0x00000288, 0x000040F0,
    0x00000DB8, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00003720, 0x000040F0,
    0x0004007C, 0x0000000C, 0x0000294E, 0x00003720, 0x00050041, 0x00000288,
    0x0000288B, 0x00000DB8, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00004B48,
    0x0000288B, 0x0004003D, 0x0000000B, 0x00003CCC, 0x00001283, 0x000500AE,
    0x00000009, 0x0000523A, 0x00004B48, 0x00003CCC, 0x000300F7, 0x00001EF3,
    0x00000000, 0x000400FA, 0x0000523A, 0x00002E2A, 0x00001EA7, 0x000200F8,
    0x00002E2A, 0x0004003D, 0x0000000B, 0x00004E03, 0x00001283, 0x0004007C,
    0x0000000C, 0x000054D6, 0x00004E03, 0x0004007E, 0x0000000C, 0x000057C1,
    0x000054D6, 0x0003003E, 0x00002FFD, 0x000057C1, 0x000200F9, 0x00001EF3,
    0x000200F8, 0x00001EA7, 0x0004003D, 0x0000000B, 0x000052B9, 0x00001283,
    0x0004007C, 0x0000000C, 0x00004378, 0x000052B9, 0x0003003E, 0x00002FFD,
    0x00004378, 0x000200F9, 0x00001EF3, 0x000200F8, 0x00001EF3, 0x0004003D,
    0x0000000C, 0x0000549F, 0x00002FFD, 0x00050080, 0x0000000C, 0x000020E4,
    0x0000294E, 0x0000549F, 0x0004007C, 0x0000000B, 0x0000203F, 0x000020E4,
    0x00050041, 0x00000288, 0x00004BBA, 0x00000DB8, 0x00000A0A, 0x0003003E,
    0x00004BBA, 0x0000203F, 0x000200F9, 0x00001A46, 0x000200F8, 0x00001A46,
    0x0004003D, 0x0000000B, 0x00003006, 0x000014C6, 0x00050041, 0x00000288,
    0x00001F0B, 0x00000CA6, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00001DAA,
    0x00001F0B, 0x00050041, 0x00000288, 0x00004B1B, 0x00000CA6, 0x00000A0D,
    0x0004003D, 0x0000000B, 0x00004027, 0x00004B1B, 0x00050084, 0x0000000B,
    0x0000578D, 0x00001DAA, 0x00004027, 0x00050084, 0x0000000B, 0x00002D03,
    0x00003006, 0x0000578D, 0x00050041, 0x00000288, 0x00005B9E, 0x00000DB8,
    0x00000A0D, 0x0004003D, 0x0000000B, 0x0000402C, 0x00005B9E, 0x00050041,
    0x00000288, 0x00004B1C, 0x0000173C, 0x00000A0A, 0x0004003D, 0x0000000B,
    0x00003B2E, 0x00004B1C, 0x00050084, 0x0000000B, 0x00003A09, 0x0000402C,
    0x00003B2E, 0x00050041, 0x00000288, 0x00003DCA, 0x00000DB8, 0x00000A0A,
    0x0004003D, 0x0000000B, 0x00006019, 0x00003DCA, 0x00050080, 0x0000000B,
    0x000031C8, 0x00003A09, 0x00006019, 0x0004003D, 0x0000000B, 0x00003530,
    0x00001584, 0x000500C4, 0x0000000B, 0x000057B4, 0x000031C8, 0x00003530,
    0x00050080, 0x0000000B, 0x000039AC, 0x00002D03, 0x000057B4, 0x0003003E,
    0x00000DE7, 0x000039AC, 0x0004003D, 0x00000009, 0x00002D3E, 0x00000C86,
    0x000300F7, 0x0000284D, 0x00000000, 0x000400FA, 0x00002D3E, 0x000026CA,
    0x0000284D, 0x000200F8, 0x000026CA, 0x00050041, 0x00000288, 0x00003E8A,
    0x00000CA6, 0x00000A0A, 0x0004003D, 0x0000000B, 0x00003473, 0x00003E8A,
    0x00050041, 0x00000288, 0x00004B1D, 0x00000CA6, 0x00000A0D, 0x0004003D,
    0x0000000B, 0x00004028, 0x00004B1D, 0x00050084, 0x0000000B, 0x00005741,
    0x00003473, 0x00004028, 0x00050084, 0x0000000B, 0x0000355B, 0x00005741,
    0x00000A84, 0x0004003D, 0x0000000B, 0x00005D8E, 0x00000DE7, 0x00050089,
    0x0000000B, 0x0000239F, 0x00005D8E, 0x0000355B, 0x0003003E, 0x00000DE7,
    0x0000239F, 0x000200F9, 0x0000284D, 0x000200F8, 0x0000284D, 0x0004003D,
    0x0000000B, 0x00002C3E, 0x00000DE7, 0x000200FE, 0x00002C3E, 0x00010038,
    0x00050036, 0x00000598, 0x00000E53, 0x00000000, 0x0000045F, 0x000200F8,
    0x00003400, 0x0004003B, 0x00000815, 0x00005645, 0x00000007, 0x0004003B,
    0x00000288, 0x000056D4, 0x00000007, 0x0004003B, 0x00000288, 0x000039BE,
    0x00000007, 0x0004003B, 0x00000288, 0x00003A30, 0x00000007, 0x0004003B,
    0x00000288, 0x000035D6, 0x00000007, 0x00050041, 0x0000028A, 0x0000584D,
    0x0000118F, 0x00000A0E, 0x0004003D, 0x0000000B, 0x0000528A, 0x0000584D,
    0x00050041, 0x0000028A, 0x000059FB, 0x0000118F, 0x00000A11, 0x0004003D,
    0x0000000B, 0x00004914, 0x000059FB, 0x000500C7, 0x0000000B, 0x00003AEA,
    0x0000528A, 0x00000A44, 0x00050041, 0x00000288, 0x00004F61, 0x00005645,
    0x00000A0B, 0x0003003E, 0x00004F61, 0x00003AEA, 0x000500C2, 0x0000000B,
    0x00003522, 0x0000528A, 0x00000A28, 0x000500C7, 0x0000000B, 0x00005F35,
    0x00003522, 0x00000A13, 0x00050041, 0x00000288, 0x00003A87, 0x00005645,
    0x00000A0E, 0x0003003E, 0x00003A87, 0x00005F35, 0x000500C7, 0x0000000B,
    0x00003D07, 0x0000528A, 0x00000AFE, 0x000500AB, 0x00000009, 0x00004347,
    0x00003D07, 0x00000A0A, 0x00050041, 0x00000286, 0x0000299A, 0x00005645,
    0x00000A11, 0x0003003E, 0x0000299A, 0x00004347, 0x000500C2, 0x0000000B,
    0x00003523, 0x0000528A, 0x00000A31, 0x000500C7, 0x0000000B, 0x00005F36,
    0x00003523, 0x00000A81, 0x00050041, 0x00000288, 0x00003A28, 0x00005645,
    0x00000A14, 0x0003003E, 0x00003A28, 0x00005F36, 0x000500C2, 0x0000000B,
    0x00003524, 0x0000528A, 0x00000A52, 0x000500C7, 0x0000000B, 0x00005F37,
    0x00003524, 0x00000A37, 0x00050041, 0x00000288, 0x00003A29, 0x00005645,
    0x00000A17, 0x0003003E, 0x00003A29, 0x00005F37, 0x000500C2, 0x0000000B,
    0x00003525, 0x0000528A, 0x00000A5E, 0x000500C7, 0x0000000B, 0x00005F38,
    0x00003525, 0x00000A0D, 0x00050041, 0x00000288, 0x0000305C, 0x00005645,
    0x00000A1A, 0x0003003E, 0x0000305C, 0x00005F38, 0x0003003E, 0x000056D4,
    0x00000A0D, 0x00050039, 0x00000011, 0x0000569E, 0x00001619, 0x000056D4,
    0x00050041, 0x0000028E, 0x00005630, 0x00005645, 0x00000A1D, 0x0003003E,
    0x00005630, 0x0000569E, 0x0003003E, 0x000039BE, 0x00000A0A, 0x00050039,
    0x00000011, 0x0000569F, 0x00001619, 0x000039BE, 0x00050041, 0x0000028E,
    0x00005631, 0x00005645, 0x00000A20, 0x0003003E, 0x00005631, 0x0000569F,
    0x0003003E, 0x00003A30, 0x00004914, 0x00050039, 0x00000011, 0x000049F0,
    0x00001619, 0x00003A30, 0x000500C2, 0x00000011, 0x00001D60, 0x000049F0,
    0x0000073F, 0x0003003E, 0x000035D6, 0x00000A0D, 0x00050039, 0x00000011,
    0x00005762, 0x00001619, 0x000035D6, 0x000500C4, 0x00000011, 0x0000211E,
    0x00005762, 0x00000740, 0x00050050, 0x00000011, 0x00005AC0, 0x00000A0D,
    0x00000A0D, 0x00050082, 0x00000011, 0x0000461F, 0x0000211E, 0x00005AC0,
    0x000500C7, 0x00000011, 0x000045AB, 0x00001D60, 0x0000461F, 0x00050050,
    0x00000011, 0x000030DF, 0x00000A13, 0x00000A13, 0x000500C4, 0x00000011,
    0x00001E05, 0x000045AB, 0x000030DF, 0x00050041, 0x0000028E, 0x000019E6,
    0x00005645, 0x00000A1D, 0x0004003D, 0x00000011, 0x00003629, 0x000019E6,
    0x00050084, 0x00000011, 0x00004403, 0x00001E05, 0x00003629, 0x00050041,
    0x0000028E, 0x000032B0, 0x00005645, 0x00000A23, 0x0003003E, 0x000032B0,
    0x00004403, 0x000500C2, 0x0000000B, 0x00003526, 0x00004914, 0x00000A19,
    0x000500C7, 0x0000000B, 0x00005F22, 0x00003526, 0x00000A81, 0x00060041,
    0x00000288, 0x0000363B, 0x00005645, 0x00000A1D, 0x00000A0A, 0x0004003D,
    0x0000000B, 0x00003301, 0x0000363B, 0x00050084, 0x0000000B, 0x00004404,
    0x00005F22, 0x00003301, 0x00050041, 0x00000288, 0x0000291D, 0x00005645,
    0x00000A26, 0x0003003E, 0x0000291D, 0x00004404, 0x00050041, 0x0000028F,
    0x000035D7, 0x0000118F, 0x00000A0B, 0x0004003D, 0x00000011, 0x00001E2E,
    0x000035D7, 0x00050041, 0x0000028E, 0x00004F10, 0x00005645, 0x00000A29,
    0x0003003E, 0x00004F10, 0x00001E2E, 0x0004003D, 0x00000598, 0x0000629C,
    0x00005645, 0x000200FE, 0x0000629C, 0x00010038,
};

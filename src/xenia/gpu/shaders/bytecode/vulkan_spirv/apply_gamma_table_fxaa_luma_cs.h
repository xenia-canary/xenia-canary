// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 24916
; Schema: 0
               OpCapability Shader
               OpCapability SampledBuffer
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %5663 "main" %gl_GlobalInvocationID
               OpExecutionMode %5663 LocalSize 16 8 1
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_struct_993 Block
               OpMemberDecorate %_struct_993 0 Offset 0
               OpDecorate %5759 Binding 0
               OpDecorate %5759 DescriptorSet 1
               OpDecorate %5945 Binding 0
               OpDecorate %5945 DescriptorSet 0
               OpDecorate %3258 NonReadable
               OpDecorate %3258 Binding 0
               OpDecorate %3258 DescriptorSet 2
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
%_struct_993 = OpTypeStruct %v2uint
%_ptr_PushConstant__struct_993 = OpTypePointer PushConstant %_struct_993
       %4495 = OpVariable %_ptr_PushConstant__struct_993 PushConstant
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
%_ptr_PushConstant_v2uint = OpTypePointer PushConstant %v2uint
       %bool = OpTypeBool
     %v2bool = OpTypeVector %bool 2
%_ptr_Function_v3uint = OpTypePointer Function %v3uint
      %float = OpTypeFloat 32
        %150 = OpTypeImage %float 2D 0 0 0 1 Unknown
%_ptr_UniformConstant_150 = OpTypePointer UniformConstant %150
       %5759 = OpVariable %_ptr_UniformConstant_150 UniformConstant
      %v2int = OpTypeVector %int 2
    %v4float = OpTypeVector %float 4
    %v3float = OpTypeVector %float 3
  %float_255 = OpConstant %float 255
  %float_0_5 = OpConstant %float 0.5
%_ptr_Function_v4float = OpTypePointer Function %v4float
        %154 = OpTypeImage %float Buffer 0 0 0 1 Unknown
%_ptr_UniformConstant_154 = OpTypePointer UniformConstant %154
       %5945 = OpVariable %_ptr_UniformConstant_154 UniformConstant
     %uint_0 = OpConstant %uint 0
%_ptr_Function_uint = OpTypePointer Function %uint
     %uint_2 = OpConstant %uint 2
%_ptr_Function_float = OpTypePointer Function %float
     %uint_1 = OpConstant %uint 1
%float_0_298999995 = OpConstant %float 0.298999995
%float_0_587000012 = OpConstant %float 0.587000012
%float_0_114 = OpConstant %float 0.114
       %1268 = OpConstantComposite %v3float %float_0_298999995 %float_0_587000012 %float_0_114
     %uint_3 = OpConstant %uint 3
        %166 = OpTypeImage %float 2D 0 0 0 2 Rgba16f
%_ptr_UniformConstant_166 = OpTypePointer UniformConstant %166
       %3258 = OpVariable %_ptr_UniformConstant_166 UniformConstant
    %uint_16 = OpConstant %uint 16
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_16 %uint_8 %uint_1
       %5663 = OpFunction %void None %1282
      %24915 = OpLabel
       %3356 = OpVariable %_ptr_Function_v2uint Function
       %5494 = OpVariable %_ptr_Function_v3uint Function
       %4783 = OpVariable %_ptr_Function_v4float Function
      %13729 = OpLoad %v3uint %gl_GlobalInvocationID
      %14790 = OpVectorShuffle %v2uint %13729 %13729 0 1
               OpStore %3356 %14790
      %12144 = OpLoad %v2uint %3356
      %23034 = OpAccessChain %_ptr_PushConstant_v2uint %4495 %int_0
      %24389 = OpLoad %v2uint %23034
      %23437 = OpUGreaterThanEqual %v2bool %12144 %24389
      %23076 = OpAny %bool %23437
               OpSelectionMerge %9652 DontFlatten
               OpBranchConditional %23076 %21300 %9652
      %21300 = OpLabel
               OpReturn
       %9652 = OpLabel
      %13152 = OpLoad %150 %5759
      %13519 = OpLoad %v2uint %3356
      %23771 = OpBitcast %v2int %13519
       %6680 = OpImageFetch %v4float %13152 %23771 Lod %int_0
      %15311 = OpVectorShuffle %v3float %6680 %6680 0 1 2
      %22764 = OpVectorTimesScalar %v3float %15311 %float_255
      %15307 = OpCompositeConstruct %v3float %float_0_5 %float_0_5 %float_0_5
      %17157 = OpFAdd %v3float %22764 %15307
      %18356 = OpConvertFToU %v3uint %17157
               OpStore %5494 %18356
      %16968 = OpLoad %154 %5945
      %22084 = OpAccessChain %_ptr_Function_uint %5494 %uint_0
      %12847 = OpLoad %uint %22084
      %20578 = OpBitcast %int %12847
      %23942 = OpImageFetch %v4float %16968 %20578
      %11904 = OpCompositeExtract %float %23942 2
       %6535 = OpAccessChain %_ptr_Function_float %4783 %uint_0
               OpStore %6535 %11904
      %13767 = OpLoad %154 %5945
      %22085 = OpAccessChain %_ptr_Function_uint %5494 %uint_1
      %12848 = OpLoad %uint %22085
      %20579 = OpBitcast %int %12848
      %23943 = OpImageFetch %v4float %13767 %20579
      %11905 = OpCompositeExtract %float %23943 1
       %6536 = OpAccessChain %_ptr_Function_float %4783 %uint_1
               OpStore %6536 %11905
      %13768 = OpLoad %154 %5945
      %22086 = OpAccessChain %_ptr_Function_uint %5494 %uint_2
      %12849 = OpLoad %uint %22086
      %20580 = OpBitcast %int %12849
      %23944 = OpImageFetch %v4float %13768 %20580
      %11906 = OpCompositeExtract %float %23944 0
       %6537 = OpAccessChain %_ptr_Function_float %4783 %uint_2
               OpStore %6537 %11906
      %12998 = OpLoad %v4float %4783
      %24707 = OpVectorShuffle %v3float %12998 %12998 0 1 2
      %11477 = OpDot %float %24707 %1268
       %7433 = OpAccessChain %_ptr_Function_float %4783 %uint_3
               OpStore %7433 %11477
      %15656 = OpLoad %166 %3258
      %23689 = OpLoad %v2uint %3356
      %10962 = OpBitcast %v2int %23689
      %16897 = OpLoad %v4float %4783
               OpImageWrite %15656 %10962 %16897
               OpReturn
               OpFunctionEnd
#endif

const uint32_t apply_gamma_table_fxaa_luma_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00006154, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000002E, 0x0006000B, 0x00000001, 0x4C534C47,
    0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000F48,
    0x00060010, 0x0000161F, 0x00000011, 0x00000010, 0x00000008, 0x00000001,
    0x00040047, 0x00000F48, 0x0000000B, 0x0000001C, 0x00030047, 0x000003E1,
    0x00000002, 0x00050048, 0x000003E1, 0x00000000, 0x00000023, 0x00000000,
    0x00040047, 0x0000167F, 0x00000021, 0x00000000, 0x00040047, 0x0000167F,
    0x00000022, 0x00000001, 0x00040047, 0x00001739, 0x00000021, 0x00000000,
    0x00040047, 0x00001739, 0x00000022, 0x00000000, 0x00030047, 0x00000CBA,
    0x00000019, 0x00040047, 0x00000CBA, 0x00000021, 0x00000000, 0x00040047,
    0x00000CBA, 0x00000022, 0x00000002, 0x00040047, 0x00000B0F, 0x0000000B,
    0x00000019, 0x00020013, 0x00000008, 0x00030021, 0x00000502, 0x00000008,
    0x00040015, 0x0000000B, 0x00000020, 0x00000000, 0x00040017, 0x00000011,
    0x0000000B, 0x00000002, 0x00040020, 0x0000028E, 0x00000007, 0x00000011,
    0x00040017, 0x00000014, 0x0000000B, 0x00000003, 0x00040020, 0x00000291,
    0x00000001, 0x00000014, 0x0004003B, 0x00000291, 0x00000F48, 0x00000001,
    0x0003001E, 0x000003E1, 0x00000011, 0x00040020, 0x0000065E, 0x00000009,
    0x000003E1, 0x0004003B, 0x0000065E, 0x0000118F, 0x00000009, 0x00040015,
    0x0000000C, 0x00000020, 0x00000001, 0x0004002B, 0x0000000C, 0x00000A0B,
    0x00000000, 0x00040020, 0x0000028F, 0x00000009, 0x00000011, 0x00020014,
    0x00000009, 0x00040017, 0x0000000F, 0x00000009, 0x00000002, 0x00040020,
    0x00000292, 0x00000007, 0x00000014, 0x00030016, 0x0000000D, 0x00000020,
    0x00090019, 0x00000096, 0x0000000D, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x00000313, 0x00000000,
    0x00000096, 0x0004003B, 0x00000313, 0x0000167F, 0x00000000, 0x00040017,
    0x00000012, 0x0000000C, 0x00000002, 0x00040017, 0x0000001D, 0x0000000D,
    0x00000004, 0x00040017, 0x00000018, 0x0000000D, 0x00000003, 0x0004002B,
    0x0000000D, 0x00000540, 0x437F0000, 0x0004002B, 0x0000000D, 0x000000FC,
    0x3F000000, 0x00040020, 0x0000029A, 0x00000007, 0x0000001D, 0x00090019,
    0x0000009A, 0x0000000D, 0x00000005, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00040020, 0x00000317, 0x00000000, 0x0000009A,
    0x0004003B, 0x00000317, 0x00001739, 0x00000000, 0x0004002B, 0x0000000B,
    0x00000A0A, 0x00000000, 0x00040020, 0x00000288, 0x00000007, 0x0000000B,
    0x0004002B, 0x0000000B, 0x00000A10, 0x00000002, 0x00040020, 0x0000028A,
    0x00000007, 0x0000000D, 0x0004002B, 0x0000000B, 0x00000A0D, 0x00000001,
    0x0004002B, 0x0000000D, 0x00000351, 0x3E991687, 0x0004002B, 0x0000000D,
    0x00000458, 0x3F1645A2, 0x0004002B, 0x0000000D, 0x000001DC, 0x3DE978D5,
    0x0006002C, 0x00000018, 0x000004F4, 0x00000351, 0x00000458, 0x000001DC,
    0x0004002B, 0x0000000B, 0x00000A13, 0x00000003, 0x00090019, 0x000000A6,
    0x0000000D, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002,
    0x00000002, 0x00040020, 0x00000323, 0x00000000, 0x000000A6, 0x0004003B,
    0x00000323, 0x00000CBA, 0x00000000, 0x0004002B, 0x0000000B, 0x00000A3A,
    0x00000010, 0x0004002B, 0x0000000B, 0x00000A22, 0x00000008, 0x0006002C,
    0x00000014, 0x00000B0F, 0x00000A3A, 0x00000A22, 0x00000A0D, 0x00050036,
    0x00000008, 0x0000161F, 0x00000000, 0x00000502, 0x000200F8, 0x00006153,
    0x0004003B, 0x0000028E, 0x00000D1C, 0x00000007, 0x0004003B, 0x00000292,
    0x00001576, 0x00000007, 0x0004003B, 0x0000029A, 0x000012AF, 0x00000007,
    0x0004003D, 0x00000014, 0x000035A1, 0x00000F48, 0x0007004F, 0x00000011,
    0x000039C6, 0x000035A1, 0x000035A1, 0x00000000, 0x00000001, 0x0003003E,
    0x00000D1C, 0x000039C6, 0x0004003D, 0x00000011, 0x00002F70, 0x00000D1C,
    0x00050041, 0x0000028F, 0x000059FA, 0x0000118F, 0x00000A0B, 0x0004003D,
    0x00000011, 0x00005F45, 0x000059FA, 0x000500AE, 0x0000000F, 0x00005B8D,
    0x00002F70, 0x00005F45, 0x0004009A, 0x00000009, 0x00005A24, 0x00005B8D,
    0x000300F7, 0x000025B4, 0x00000002, 0x000400FA, 0x00005A24, 0x00005334,
    0x000025B4, 0x000200F8, 0x00005334, 0x000100FD, 0x000200F8, 0x000025B4,
    0x0004003D, 0x00000096, 0x00003360, 0x0000167F, 0x0004003D, 0x00000011,
    0x000034CF, 0x00000D1C, 0x0004007C, 0x00000012, 0x00005CDB, 0x000034CF,
    0x0007005F, 0x0000001D, 0x00001A18, 0x00003360, 0x00005CDB, 0x00000002,
    0x00000A0B, 0x0008004F, 0x00000018, 0x00003BCF, 0x00001A18, 0x00001A18,
    0x00000000, 0x00000001, 0x00000002, 0x0005008E, 0x00000018, 0x000058EC,
    0x00003BCF, 0x00000540, 0x00060050, 0x00000018, 0x00003BCB, 0x000000FC,
    0x000000FC, 0x000000FC, 0x00050081, 0x00000018, 0x00004305, 0x000058EC,
    0x00003BCB, 0x0004006D, 0x00000014, 0x000047B4, 0x00004305, 0x0003003E,
    0x00001576, 0x000047B4, 0x0004003D, 0x0000009A, 0x00004248, 0x00001739,
    0x00050041, 0x00000288, 0x00005644, 0x00001576, 0x00000A0A, 0x0004003D,
    0x0000000B, 0x0000322F, 0x00005644, 0x0004007C, 0x0000000C, 0x00005062,
    0x0000322F, 0x0005005F, 0x0000001D, 0x00005D86, 0x00004248, 0x00005062,
    0x00050051, 0x0000000D, 0x00002E80, 0x00005D86, 0x00000002, 0x00050041,
    0x0000028A, 0x00001987, 0x000012AF, 0x00000A0A, 0x0003003E, 0x00001987,
    0x00002E80, 0x0004003D, 0x0000009A, 0x000035C7, 0x00001739, 0x00050041,
    0x00000288, 0x00005645, 0x00001576, 0x00000A0D, 0x0004003D, 0x0000000B,
    0x00003230, 0x00005645, 0x0004007C, 0x0000000C, 0x00005063, 0x00003230,
    0x0005005F, 0x0000001D, 0x00005D87, 0x000035C7, 0x00005063, 0x00050051,
    0x0000000D, 0x00002E81, 0x00005D87, 0x00000001, 0x00050041, 0x0000028A,
    0x00001988, 0x000012AF, 0x00000A0D, 0x0003003E, 0x00001988, 0x00002E81,
    0x0004003D, 0x0000009A, 0x000035C8, 0x00001739, 0x00050041, 0x00000288,
    0x00005646, 0x00001576, 0x00000A10, 0x0004003D, 0x0000000B, 0x00003231,
    0x00005646, 0x0004007C, 0x0000000C, 0x00005064, 0x00003231, 0x0005005F,
    0x0000001D, 0x00005D88, 0x000035C8, 0x00005064, 0x00050051, 0x0000000D,
    0x00002E82, 0x00005D88, 0x00000000, 0x00050041, 0x0000028A, 0x00001989,
    0x000012AF, 0x00000A10, 0x0003003E, 0x00001989, 0x00002E82, 0x0004003D,
    0x0000001D, 0x000032C6, 0x000012AF, 0x0008004F, 0x00000018, 0x00006083,
    0x000032C6, 0x000032C6, 0x00000000, 0x00000001, 0x00000002, 0x00050094,
    0x0000000D, 0x00002CD5, 0x00006083, 0x000004F4, 0x00050041, 0x0000028A,
    0x00001D09, 0x000012AF, 0x00000A13, 0x0003003E, 0x00001D09, 0x00002CD5,
    0x0004003D, 0x000000A6, 0x00003D28, 0x00000CBA, 0x0004003D, 0x00000011,
    0x00005C89, 0x00000D1C, 0x0004007C, 0x00000012, 0x00002AD2, 0x00005C89,
    0x0004003D, 0x0000001D, 0x00004201, 0x000012AF, 0x00040063, 0x00003D28,
    0x00002AD2, 0x00004201, 0x000100FD, 0x00010038,
};

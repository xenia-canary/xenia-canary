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
               OpEntryPoint Fragment %5663 "main" %gl_FragCoord %3258
               OpExecutionMode %5663 OriginUpperLeft
               OpDecorate %gl_FragCoord BuiltIn FragCoord
               OpDecorate %5759 Binding 0
               OpDecorate %5759 DescriptorSet 1
               OpDecorate %5945 Binding 0
               OpDecorate %5945 DescriptorSet 0
               OpDecorate %3258 Location 0
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Input_v4float = OpTypePointer Input %v4float
%gl_FragCoord = OpVariable %_ptr_Input_v4float Input
    %v2float = OpTypeVector %float 2
     %v3uint = OpTypeVector %uint 3
%_ptr_Function_v3uint = OpTypePointer Function %v3uint
        %150 = OpTypeImage %float 2D 0 0 0 1 Unknown
%_ptr_UniformConstant_150 = OpTypePointer UniformConstant %150
       %5759 = OpVariable %_ptr_UniformConstant_150 UniformConstant
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
      %int_0 = OpConstant %int 0
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
    %float_1 = OpConstant %float 1
     %uint_3 = OpConstant %uint 3
%_ptr_Output_v4float = OpTypePointer Output %v4float
       %3258 = OpVariable %_ptr_Output_v4float Output
       %5663 = OpFunction %void None %1282
      %24915 = OpLabel
       %5494 = OpVariable %_ptr_Function_v3uint Function
       %4783 = OpVariable %_ptr_Function_v4float Function
      %14622 = OpLoad %v4float %gl_FragCoord
       %6659 = OpVectorShuffle %v2float %14622 %14622 0 1
       %8667 = OpConvertFToU %v2uint %6659
      %21665 = OpLoad %150 %5759
      %11127 = OpBitcast %v2int %8667
       %6680 = OpImageFetch %v4float %21665 %11127 Lod %int_0
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
       %6611 = OpAccessChain %_ptr_Function_float %4783 %uint_2
               OpStore %6611 %11906
      %19656 = OpAccessChain %_ptr_Function_float %4783 %uint_3
               OpStore %19656 %float_1
      %17934 = OpLoad %v4float %4783
               OpStore %3258 %17934
               OpReturn
               OpFunctionEnd
#endif

const uint32_t apply_gamma_table_ps[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00006154, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000002E, 0x0006000B, 0x00000001, 0x4C534C47,
    0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0007000F, 0x00000004, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000C93,
    0x00000CBA, 0x00030010, 0x0000161F, 0x00000007, 0x00040047, 0x00000C93,
    0x0000000B, 0x0000000F, 0x00040047, 0x0000167F, 0x00000021, 0x00000000,
    0x00040047, 0x0000167F, 0x00000022, 0x00000001, 0x00040047, 0x00001739,
    0x00000021, 0x00000000, 0x00040047, 0x00001739, 0x00000022, 0x00000000,
    0x00040047, 0x00000CBA, 0x0000001E, 0x00000000, 0x00020013, 0x00000008,
    0x00030021, 0x00000502, 0x00000008, 0x00040015, 0x0000000B, 0x00000020,
    0x00000000, 0x00040017, 0x00000011, 0x0000000B, 0x00000002, 0x00030016,
    0x0000000D, 0x00000020, 0x00040017, 0x0000001D, 0x0000000D, 0x00000004,
    0x00040020, 0x0000029A, 0x00000001, 0x0000001D, 0x0004003B, 0x0000029A,
    0x00000C93, 0x00000001, 0x00040017, 0x00000013, 0x0000000D, 0x00000002,
    0x00040017, 0x00000014, 0x0000000B, 0x00000003, 0x00040020, 0x00000291,
    0x00000007, 0x00000014, 0x00090019, 0x00000096, 0x0000000D, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x00040020,
    0x00000313, 0x00000000, 0x00000096, 0x0004003B, 0x00000313, 0x0000167F,
    0x00000000, 0x00040015, 0x0000000C, 0x00000020, 0x00000001, 0x00040017,
    0x00000012, 0x0000000C, 0x00000002, 0x0004002B, 0x0000000C, 0x00000A0B,
    0x00000000, 0x00040017, 0x00000018, 0x0000000D, 0x00000003, 0x0004002B,
    0x0000000D, 0x00000540, 0x437F0000, 0x0004002B, 0x0000000D, 0x000000FC,
    0x3F000000, 0x00040020, 0x0000029B, 0x00000007, 0x0000001D, 0x00090019,
    0x0000009A, 0x0000000D, 0x00000005, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x00040020, 0x00000317, 0x00000000, 0x0000009A,
    0x0004003B, 0x00000317, 0x00001739, 0x00000000, 0x0004002B, 0x0000000B,
    0x00000A0A, 0x00000000, 0x00040020, 0x00000288, 0x00000007, 0x0000000B,
    0x0004002B, 0x0000000B, 0x00000A10, 0x00000002, 0x00040020, 0x0000028A,
    0x00000007, 0x0000000D, 0x0004002B, 0x0000000B, 0x00000A0D, 0x00000001,
    0x0004002B, 0x0000000D, 0x0000008A, 0x3F800000, 0x0004002B, 0x0000000B,
    0x00000A13, 0x00000003, 0x00040020, 0x0000029C, 0x00000003, 0x0000001D,
    0x0004003B, 0x0000029C, 0x00000CBA, 0x00000003, 0x00050036, 0x00000008,
    0x0000161F, 0x00000000, 0x00000502, 0x000200F8, 0x00006153, 0x0004003B,
    0x00000291, 0x00001576, 0x00000007, 0x0004003B, 0x0000029B, 0x000012AF,
    0x00000007, 0x0004003D, 0x0000001D, 0x0000391E, 0x00000C93, 0x0007004F,
    0x00000013, 0x00001A03, 0x0000391E, 0x0000391E, 0x00000000, 0x00000001,
    0x0004006D, 0x00000011, 0x000021DB, 0x00001A03, 0x0004003D, 0x00000096,
    0x000054A1, 0x0000167F, 0x0004007C, 0x00000012, 0x00002B77, 0x000021DB,
    0x0007005F, 0x0000001D, 0x00001A18, 0x000054A1, 0x00002B77, 0x00000002,
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
    0x00002E82, 0x00005D88, 0x00000000, 0x00050041, 0x0000028A, 0x000019D3,
    0x000012AF, 0x00000A10, 0x0003003E, 0x000019D3, 0x00002E82, 0x00050041,
    0x0000028A, 0x00004CC8, 0x000012AF, 0x00000A13, 0x0003003E, 0x00004CC8,
    0x0000008A, 0x0004003D, 0x0000001D, 0x0000460E, 0x000012AF, 0x0003003E,
    0x00000CBA, 0x0000460E, 0x000100FD, 0x00010038,
};

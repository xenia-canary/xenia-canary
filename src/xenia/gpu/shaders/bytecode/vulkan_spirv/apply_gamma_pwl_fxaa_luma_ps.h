// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 25013
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
      %float = OpTypeFloat 32
%_ptr_Function_float = OpTypePointer Function %float
        %207 = OpTypeFunction %float %_ptr_Function_float
       %uint = OpTypeInt 32 0
%_ptr_Function_uint = OpTypePointer Function %uint
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
       %2785 = OpTypeFunction %float %_ptr_Function_uint %_ptr_Function_v2uint
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
     %uint_0 = OpConstant %uint 0
     %uint_7 = OpConstant %uint 7
     %uint_1 = OpConstant %uint 1
%float_0_125 = OpConstant %float 0.125
%float_1_52737048en05 = OpConstant %float 1.52737048e-05
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
 %float_1023 = OpConstant %float 1023
  %float_0_5 = OpConstant %float 0.5
%_ptr_Function_v4float = OpTypePointer Function %v4float
        %152 = OpTypeImage %uint Buffer 0 0 0 1 Unknown
%_ptr_UniformConstant_152 = OpTypePointer UniformConstant %152
       %5945 = OpVariable %_ptr_UniformConstant_152 UniformConstant
     %uint_3 = OpConstant %uint 3
     %v4uint = OpTypeVector %uint 4
     %uint_2 = OpConstant %uint 2
%float_0_298999995 = OpConstant %float 0.298999995
%float_0_587000012 = OpConstant %float 0.587000012
%float_0_114 = OpConstant %float 0.114
       %1268 = OpConstantComposite %v3float %float_0_298999995 %float_0_587000012 %float_0_114
%_ptr_Output_v4float = OpTypePointer Output %v4float
       %3258 = OpVariable %_ptr_Output_v4float Output
       %5663 = OpFunction %void None %1282
      %24915 = OpLabel
       %5494 = OpVariable %_ptr_Function_v3uint Function
       %4783 = OpVariable %_ptr_Function_v4float Function
      %14447 = OpVariable %_ptr_Function_uint Function
      %14448 = OpVariable %_ptr_Function_v2uint Function
      %14449 = OpVariable %_ptr_Function_uint Function
      %14450 = OpVariable %_ptr_Function_v2uint Function
      %14485 = OpVariable %_ptr_Function_uint Function
       %5786 = OpVariable %_ptr_Function_v2uint Function
      %14622 = OpLoad %v4float %gl_FragCoord
       %6659 = OpVectorShuffle %v2float %14622 %14622 0 1
       %8667 = OpConvertFToU %v2uint %6659
      %21665 = OpLoad %150 %5759
      %11127 = OpBitcast %v2int %8667
       %6680 = OpImageFetch %v4float %21665 %11127 Lod %int_0
      %15311 = OpVectorShuffle %v3float %6680 %6680 0 1 2
      %22764 = OpVectorTimesScalar %v3float %15311 %float_1023
      %15307 = OpCompositeConstruct %v3float %float_0_5 %float_0_5 %float_0_5
      %17157 = OpFAdd %v3float %22764 %15307
      %18356 = OpConvertFToU %v3uint %17157
               OpStore %5494 %18356
      %16968 = OpLoad %152 %5945
      %23414 = OpAccessChain %_ptr_Function_uint %5494 %uint_0
      %20511 = OpLoad %uint %23414
      %13614 = OpShiftRightLogical %uint %20511 %uint_3
      %12752 = OpIMul %uint %13614 %uint_3
      %17964 = OpBitcast %int %12752
      %16397 = OpImageFetch %v4uint %16968 %17964
      %10187 = OpAccessChain %_ptr_Function_uint %5494 %uint_0
      %12248 = OpLoad %uint %10187
               OpStore %14447 %12248
      %16078 = OpVectorShuffle %v2uint %16397 %16397 0 1
               OpStore %14448 %16078
       %6793 = OpFunctionCall %float %4408 %14447 %14448
       %9096 = OpAccessChain %_ptr_Function_float %4783 %uint_0
               OpStore %9096 %6793
      %13767 = OpLoad %152 %5945
      %23415 = OpAccessChain %_ptr_Function_uint %5494 %uint_1
      %20512 = OpLoad %uint %23415
      %13690 = OpShiftRightLogical %uint %20512 %uint_3
      %12611 = OpIMul %uint %13690 %uint_3
      %18887 = OpIAdd %uint %12611 %uint_1
      %14156 = OpBitcast %int %18887
      %19628 = OpImageFetch %v4uint %13767 %14156
      %10188 = OpAccessChain %_ptr_Function_uint %5494 %uint_1
      %12249 = OpLoad %uint %10188
               OpStore %14449 %12249
      %16079 = OpVectorShuffle %v2uint %19628 %19628 0 1
               OpStore %14450 %16079
       %6794 = OpFunctionCall %float %4408 %14449 %14450
       %9097 = OpAccessChain %_ptr_Function_float %4783 %uint_1
               OpStore %9097 %6794
      %13768 = OpLoad %152 %5945
      %23416 = OpAccessChain %_ptr_Function_uint %5494 %uint_2
      %20513 = OpLoad %uint %23416
      %13691 = OpShiftRightLogical %uint %20513 %uint_3
      %12612 = OpIMul %uint %13691 %uint_3
      %18888 = OpIAdd %uint %12612 %uint_2
      %14157 = OpBitcast %int %18888
      %19629 = OpImageFetch %v4uint %13768 %14157
      %10189 = OpAccessChain %_ptr_Function_uint %5494 %uint_2
      %12250 = OpLoad %uint %10189
               OpStore %14485 %12250
      %16080 = OpVectorShuffle %v2uint %19629 %19629 0 1
               OpStore %5786 %16080
       %6795 = OpFunctionCall %float %4408 %14485 %5786
       %9098 = OpAccessChain %_ptr_Function_float %4783 %uint_2
               OpStore %9098 %6795
      %12998 = OpLoad %v4float %4783
      %24707 = OpVectorShuffle %v3float %12998 %12998 0 1 2
      %11477 = OpDot %float %24707 %1268
       %7433 = OpAccessChain %_ptr_Function_float %4783 %uint_3
               OpStore %7433 %11477
      %17934 = OpLoad %v4float %4783
               OpStore %3258 %17934
               OpReturn
               OpFunctionEnd
       %5033 = OpFunction %float None %207
       %3189 = OpFunctionParameter %_ptr_Function_float
      %21624 = OpLabel
      %17255 = OpLoad %float %3189
      %11568 = OpExtInst %float %1 FClamp %17255 %float_0 %float_1
               OpReturnValue %11568
               OpFunctionEnd
       %4408 = OpFunction %float None %2785
       %4264 = OpFunctionParameter %_ptr_Function_uint
       %3259 = OpFunctionParameter %_ptr_Function_v2uint
      %22654 = OpLabel
      %17608 = OpVariable %_ptr_Function_float Function
      %12973 = OpAccessChain %_ptr_Function_uint %3259 %uint_0
      %14099 = OpLoad %uint %12973
      %14027 = OpConvertUToF %float %14099
      %19819 = OpLoad %uint %4264
      %25012 = OpBitwiseAnd %uint %19819 %uint_7
      %20416 = OpAccessChain %_ptr_Function_uint %3259 %uint_1
      %15049 = OpLoad %uint %20416
      %11729 = OpIMul %uint %25012 %15049
      %15978 = OpConvertUToF %float %11729
      %17832 = OpFMul %float %15978 %float_0_125
      %14290 = OpFAdd %float %14027 %17832
      %21024 = OpFMul %float %14290 %float_1_52737048en05
               OpStore %17608 %21024
      %11431 = OpFunctionCall %float %5033 %17608
               OpReturnValue %11431
               OpFunctionEnd
#endif

const uint32_t apply_gamma_pwl_fxaa_luma_ps[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x000061B5, 0x00000000, 0x00020011,
    0x00000001, 0x00020011, 0x0000002E, 0x0006000B, 0x00000001, 0x4C534C47,
    0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0007000F, 0x00000004, 0x0000161F, 0x6E69616D, 0x00000000, 0x00000C93,
    0x00000CBA, 0x00030010, 0x0000161F, 0x00000007, 0x00040047, 0x00000C93,
    0x0000000B, 0x0000000F, 0x00040047, 0x0000167F, 0x00000021, 0x00000000,
    0x00040047, 0x0000167F, 0x00000022, 0x00000001, 0x00040047, 0x00001739,
    0x00000021, 0x00000000, 0x00040047, 0x00001739, 0x00000022, 0x00000000,
    0x00040047, 0x00000CBA, 0x0000001E, 0x00000000, 0x00020013, 0x00000008,
    0x00030021, 0x00000502, 0x00000008, 0x00030016, 0x0000000D, 0x00000020,
    0x00040020, 0x0000028A, 0x00000007, 0x0000000D, 0x00040021, 0x000000CF,
    0x0000000D, 0x0000028A, 0x00040015, 0x0000000B, 0x00000020, 0x00000000,
    0x00040020, 0x00000288, 0x00000007, 0x0000000B, 0x00040017, 0x00000011,
    0x0000000B, 0x00000002, 0x00040020, 0x0000028E, 0x00000007, 0x00000011,
    0x00050021, 0x00000AE1, 0x0000000D, 0x00000288, 0x0000028E, 0x0004002B,
    0x0000000D, 0x00000A0C, 0x00000000, 0x0004002B, 0x0000000D, 0x0000008A,
    0x3F800000, 0x0004002B, 0x0000000B, 0x00000A0A, 0x00000000, 0x0004002B,
    0x0000000B, 0x00000A1F, 0x00000007, 0x0004002B, 0x0000000B, 0x00000A0D,
    0x00000001, 0x0004002B, 0x0000000D, 0x000001E0, 0x3E000000, 0x0004002B,
    0x0000000D, 0x000009AA, 0x37802008, 0x00040017, 0x0000001D, 0x0000000D,
    0x00000004, 0x00040020, 0x0000029A, 0x00000001, 0x0000001D, 0x0004003B,
    0x0000029A, 0x00000C93, 0x00000001, 0x00040017, 0x00000013, 0x0000000D,
    0x00000002, 0x00040017, 0x00000014, 0x0000000B, 0x00000003, 0x00040020,
    0x00000291, 0x00000007, 0x00000014, 0x00090019, 0x00000096, 0x0000000D,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000,
    0x00040020, 0x00000313, 0x00000000, 0x00000096, 0x0004003B, 0x00000313,
    0x0000167F, 0x00000000, 0x00040015, 0x0000000C, 0x00000020, 0x00000001,
    0x00040017, 0x00000012, 0x0000000C, 0x00000002, 0x0004002B, 0x0000000C,
    0x00000A0B, 0x00000000, 0x00040017, 0x00000018, 0x0000000D, 0x00000003,
    0x0004002B, 0x0000000D, 0x00000409, 0x447FC000, 0x0004002B, 0x0000000D,
    0x000000FC, 0x3F000000, 0x00040020, 0x0000029B, 0x00000007, 0x0000001D,
    0x00090019, 0x00000098, 0x0000000B, 0x00000005, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x00040020, 0x00000315, 0x00000000,
    0x00000098, 0x0004003B, 0x00000315, 0x00001739, 0x00000000, 0x0004002B,
    0x0000000B, 0x00000A13, 0x00000003, 0x00040017, 0x00000017, 0x0000000B,
    0x00000004, 0x0004002B, 0x0000000B, 0x00000A10, 0x00000002, 0x0004002B,
    0x0000000D, 0x00000351, 0x3E991687, 0x0004002B, 0x0000000D, 0x00000458,
    0x3F1645A2, 0x0004002B, 0x0000000D, 0x000001DC, 0x3DE978D5, 0x0006002C,
    0x00000018, 0x000004F4, 0x00000351, 0x00000458, 0x000001DC, 0x00040020,
    0x0000029C, 0x00000003, 0x0000001D, 0x0004003B, 0x0000029C, 0x00000CBA,
    0x00000003, 0x00050036, 0x00000008, 0x0000161F, 0x00000000, 0x00000502,
    0x000200F8, 0x00006153, 0x0004003B, 0x00000291, 0x00001576, 0x00000007,
    0x0004003B, 0x0000029B, 0x000012AF, 0x00000007, 0x0004003B, 0x00000288,
    0x0000386F, 0x00000007, 0x0004003B, 0x0000028E, 0x00003870, 0x00000007,
    0x0004003B, 0x00000288, 0x00003871, 0x00000007, 0x0004003B, 0x0000028E,
    0x00003872, 0x00000007, 0x0004003B, 0x00000288, 0x00003895, 0x00000007,
    0x0004003B, 0x0000028E, 0x0000169A, 0x00000007, 0x0004003D, 0x0000001D,
    0x0000391E, 0x00000C93, 0x0007004F, 0x00000013, 0x00001A03, 0x0000391E,
    0x0000391E, 0x00000000, 0x00000001, 0x0004006D, 0x00000011, 0x000021DB,
    0x00001A03, 0x0004003D, 0x00000096, 0x000054A1, 0x0000167F, 0x0004007C,
    0x00000012, 0x00002B77, 0x000021DB, 0x0007005F, 0x0000001D, 0x00001A18,
    0x000054A1, 0x00002B77, 0x00000002, 0x00000A0B, 0x0008004F, 0x00000018,
    0x00003BCF, 0x00001A18, 0x00001A18, 0x00000000, 0x00000001, 0x00000002,
    0x0005008E, 0x00000018, 0x000058EC, 0x00003BCF, 0x00000409, 0x00060050,
    0x00000018, 0x00003BCB, 0x000000FC, 0x000000FC, 0x000000FC, 0x00050081,
    0x00000018, 0x00004305, 0x000058EC, 0x00003BCB, 0x0004006D, 0x00000014,
    0x000047B4, 0x00004305, 0x0003003E, 0x00001576, 0x000047B4, 0x0004003D,
    0x00000098, 0x00004248, 0x00001739, 0x00050041, 0x00000288, 0x00005B76,
    0x00001576, 0x00000A0A, 0x0004003D, 0x0000000B, 0x0000501F, 0x00005B76,
    0x000500C2, 0x0000000B, 0x0000352E, 0x0000501F, 0x00000A13, 0x00050084,
    0x0000000B, 0x000031D0, 0x0000352E, 0x00000A13, 0x0004007C, 0x0000000C,
    0x0000462C, 0x000031D0, 0x0005005F, 0x00000017, 0x0000400D, 0x00004248,
    0x0000462C, 0x00050041, 0x00000288, 0x000027CB, 0x00001576, 0x00000A0A,
    0x0004003D, 0x0000000B, 0x00002FD8, 0x000027CB, 0x0003003E, 0x0000386F,
    0x00002FD8, 0x0007004F, 0x00000011, 0x00003ECE, 0x0000400D, 0x0000400D,
    0x00000000, 0x00000001, 0x0003003E, 0x00003870, 0x00003ECE, 0x00060039,
    0x0000000D, 0x00001A89, 0x00001138, 0x0000386F, 0x00003870, 0x00050041,
    0x0000028A, 0x00002388, 0x000012AF, 0x00000A0A, 0x0003003E, 0x00002388,
    0x00001A89, 0x0004003D, 0x00000098, 0x000035C7, 0x00001739, 0x00050041,
    0x00000288, 0x00005B77, 0x00001576, 0x00000A0D, 0x0004003D, 0x0000000B,
    0x00005020, 0x00005B77, 0x000500C2, 0x0000000B, 0x0000357A, 0x00005020,
    0x00000A13, 0x00050084, 0x0000000B, 0x00003143, 0x0000357A, 0x00000A13,
    0x00050080, 0x0000000B, 0x000049C7, 0x00003143, 0x00000A0D, 0x0004007C,
    0x0000000C, 0x0000374C, 0x000049C7, 0x0005005F, 0x00000017, 0x00004CAC,
    0x000035C7, 0x0000374C, 0x00050041, 0x00000288, 0x000027CC, 0x00001576,
    0x00000A0D, 0x0004003D, 0x0000000B, 0x00002FD9, 0x000027CC, 0x0003003E,
    0x00003871, 0x00002FD9, 0x0007004F, 0x00000011, 0x00003ECF, 0x00004CAC,
    0x00004CAC, 0x00000000, 0x00000001, 0x0003003E, 0x00003872, 0x00003ECF,
    0x00060039, 0x0000000D, 0x00001A8A, 0x00001138, 0x00003871, 0x00003872,
    0x00050041, 0x0000028A, 0x00002389, 0x000012AF, 0x00000A0D, 0x0003003E,
    0x00002389, 0x00001A8A, 0x0004003D, 0x00000098, 0x000035C8, 0x00001739,
    0x00050041, 0x00000288, 0x00005B78, 0x00001576, 0x00000A10, 0x0004003D,
    0x0000000B, 0x00005021, 0x00005B78, 0x000500C2, 0x0000000B, 0x0000357B,
    0x00005021, 0x00000A13, 0x00050084, 0x0000000B, 0x00003144, 0x0000357B,
    0x00000A13, 0x00050080, 0x0000000B, 0x000049C8, 0x00003144, 0x00000A10,
    0x0004007C, 0x0000000C, 0x0000374D, 0x000049C8, 0x0005005F, 0x00000017,
    0x00004CAD, 0x000035C8, 0x0000374D, 0x00050041, 0x00000288, 0x000027CD,
    0x00001576, 0x00000A10, 0x0004003D, 0x0000000B, 0x00002FDA, 0x000027CD,
    0x0003003E, 0x00003895, 0x00002FDA, 0x0007004F, 0x00000011, 0x00003ED0,
    0x00004CAD, 0x00004CAD, 0x00000000, 0x00000001, 0x0003003E, 0x0000169A,
    0x00003ED0, 0x00060039, 0x0000000D, 0x00001A8B, 0x00001138, 0x00003895,
    0x0000169A, 0x00050041, 0x0000028A, 0x0000238A, 0x000012AF, 0x00000A10,
    0x0003003E, 0x0000238A, 0x00001A8B, 0x0004003D, 0x0000001D, 0x000032C6,
    0x000012AF, 0x0008004F, 0x00000018, 0x00006083, 0x000032C6, 0x000032C6,
    0x00000000, 0x00000001, 0x00000002, 0x00050094, 0x0000000D, 0x00002CD5,
    0x00006083, 0x000004F4, 0x00050041, 0x0000028A, 0x00001D09, 0x000012AF,
    0x00000A13, 0x0003003E, 0x00001D09, 0x00002CD5, 0x0004003D, 0x0000001D,
    0x0000460E, 0x000012AF, 0x0003003E, 0x00000CBA, 0x0000460E, 0x000100FD,
    0x00010038, 0x00050036, 0x0000000D, 0x000013A9, 0x00000000, 0x000000CF,
    0x00030037, 0x0000028A, 0x00000C75, 0x000200F8, 0x00005478, 0x0004003D,
    0x0000000D, 0x00004367, 0x00000C75, 0x0008000C, 0x0000000D, 0x00002D30,
    0x00000001, 0x0000002B, 0x00004367, 0x00000A0C, 0x0000008A, 0x000200FE,
    0x00002D30, 0x00010038, 0x00050036, 0x0000000D, 0x00001138, 0x00000000,
    0x00000AE1, 0x00030037, 0x00000288, 0x000010A8, 0x00030037, 0x0000028E,
    0x00000CBB, 0x000200F8, 0x0000587E, 0x0004003B, 0x0000028A, 0x000044C8,
    0x00000007, 0x00050041, 0x00000288, 0x000032AD, 0x00000CBB, 0x00000A0A,
    0x0004003D, 0x0000000B, 0x00003713, 0x000032AD, 0x00040070, 0x0000000D,
    0x000036CB, 0x00003713, 0x0004003D, 0x0000000B, 0x00004D6B, 0x000010A8,
    0x000500C7, 0x0000000B, 0x000061B4, 0x00004D6B, 0x00000A1F, 0x00050041,
    0x00000288, 0x00004FC0, 0x00000CBB, 0x00000A0D, 0x0004003D, 0x0000000B,
    0x00003AC9, 0x00004FC0, 0x00050084, 0x0000000B, 0x00002DD1, 0x000061B4,
    0x00003AC9, 0x00040070, 0x0000000D, 0x00003E6A, 0x00002DD1, 0x00050085,
    0x0000000D, 0x000045A8, 0x00003E6A, 0x000001E0, 0x00050081, 0x0000000D,
    0x000037D2, 0x000036CB, 0x000045A8, 0x00050085, 0x0000000D, 0x00005220,
    0x000037D2, 0x000009AA, 0x0003003E, 0x000044C8, 0x00005220, 0x00050039,
    0x0000000D, 0x00002CA7, 0x000013A9, 0x000044C8, 0x000200FE, 0x00002CA7,
    0x00010038,
};

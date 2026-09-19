# A divide that cannot overflow still has to clear XER[OV].
test_divwo_1:
  #_ REGISTER_IN r4 0x7FFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  #_ REGISTER_IN r7 0x0000000000000064
  #_ REGISTER_IN r8 0x0000000000000002
  addo r6, r4, r5
  divwo r3, r7, r8
  blr
  #_ REGISTER_OUT r3 0x0000000000000032
  #_ REGISTER_OUT xer 0x80000000

# RT is undefined for both overflowing forms, so only XER is asserted.
test_divwo_2:
  #_ REGISTER_IN r4 0x0000000000000064
  #_ REGISTER_IN r5 0x0000000000000000
  divwo r3, r4, r5
  blr
  #_ REGISTER_OUT xer 0xC0000000

test_divwo_3:
  #_ REGISTER_IN r4 0xFFFFFFFF80000000
  #_ REGISTER_IN r5 0xFFFFFFFFFFFFFFFF
  divwo r3, r4, r5
  blr
  #_ REGISTER_OUT xer 0xC0000000

# The unsigned form has no unrepresentable quotient, only the zero divisor.
test_divwuo_1:
  #_ REGISTER_IN r4 0xFFFFFFFF80000000
  #_ REGISTER_IN r5 0xFFFFFFFFFFFFFFFF
  divwuo r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0000000000000000
  #_ REGISTER_OUT xer 0x00000000

test_divwuo_2:
  #_ REGISTER_IN r4 0x0000000000000064
  #_ REGISTER_IN r5 0x0000000000000000
  divwuo r3, r4, r5
  blr
  #_ REGISTER_OUT xer 0xC0000000

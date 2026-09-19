# Only if judged on 32 bits does this look like an overflow.
test_nego_1:
  #_ REGISTER_IN r4 0x0000000180000000
  nego r3, r4
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFE80000000
  #_ REGISTER_OUT xer 0x00000000

test_nego_2:
  #_ REGISTER_IN r4 0x8000000000000000
  nego r3, r4
  blr
  #_ REGISTER_OUT r3 0x8000000000000000
  #_ REGISTER_OUT xer 0xC0000000

test_nego_3:
  #_ REGISTER_IN r4 0x0000000000000005
  nego r3, r4
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFB
  #_ REGISTER_OUT xer 0x00000000

# Overflows only in the low word.
test_subfco_1:
  #_ REGISTER_IN r4 0x0000000023280000
  #_ REGISTER_IN r5 0x000000008AD00000
  subfco r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0000000067A80000
  #_ REGISTER_OUT xer 0xE0000000

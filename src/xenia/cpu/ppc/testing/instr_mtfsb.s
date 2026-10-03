test_mtfsb_1:
  #_ REGISTER_IN f1 0x0000000000000100
  mtfsf 0xFF, f1
  mtfsb0 23
  mffs f2
  blr
  #_ REGISTER_OUT f2 0x0000000000000000

# FX rises with OX and the record form copies both into CR1.
test_mtfsb_2:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN cr 0x00000000
  mtfsf 0xFF, f1
  mtfsb1. 3
  mffs f2
  blr
  #_ REGISTER_OUT f2 0x0000000090000000
  #_ REGISTER_OUT cr 0x09000000

# No FX for a bit that was already set and VX can't be set directly.
test_mtfsb_3:
  #_ REGISTER_IN f1 0x0000000010000000
  mtfsf 0xFF, f1
  mtfsb1 3
  mtfsb1 2
  mffs f2
  blr
  #_ REGISTER_OUT f2 0x0000000010000000

# A VX cause sets the VX summary too.
test_mtfsb_4:
  #_ REGISTER_IN f1 0x0000000000000000
  mtfsf 0xFF, f1
  mtfsb1 23
  mffs f2
  blr
  #_ REGISTER_OUT f2 0x00000000A0000100

# Round toward zero gives 1 + 1 ulp for 1 + 1.5 ulp, where nearest gives 2 ulp.
test_mtfsb_5:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN f3 0x3CB8000000000000
  mtfsf 0xFF, f1
  mtfsb1 31
  mffs f4
  fadd f5, f2, f3
  mtfsb0 31
  blr
  #_ REGISTER_OUT f4 0x0000000000000001
  #_ REGISTER_OUT f5 0x3FF0000000000001

test_fresx_1:
  #_ REGISTER_IN f1 2.0
  fres. f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FDFE00000000000
  #_ REGISTER_OUT f1 2.0

test_fresx_2:
  #_ REGISTER_IN f1 4.0
  fres. f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FCFE00000000000
  #_ REGISTER_OUT f1 4.0

test_fresx_3:
  #_ REGISTER_IN f1 0.5
  fres. f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FFFE00000000000
  #_ REGISTER_OUT f1 0.5

test_fresx_4:
  #_ REGISTER_IN f1 1.0
  fres. f3, f1
  blr
  #_ REGISTER_OUT f3 0x3FEFE00000000000
  #_ REGISTER_OUT f1 1.0

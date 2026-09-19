test_addo_1:
  #_ REGISTER_IN r4 0x0000000000000001
  #_ REGISTER_IN r5 0x0000000000000002
  addo r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0000000000000003
  #_ REGISTER_OUT xer 0x00000000

test_addo_2:
  #_ REGISTER_IN r4 0x7FFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  addo r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x8000000000000000
  #_ REGISTER_OUT xer 0xC0000000

# Overflows only if judged on 32 bits.
test_addo_3:
  #_ REGISTER_IN r4 0x000000007FFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  addo r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0x0000000080000000
  #_ REGISTER_OUT xer 0x00000000

# XER[OV] clears on the second add, XER[SO] does not.
test_addo_4:
  #_ REGISTER_IN r4 0x7FFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  #_ REGISTER_IN r7 0x0000000000000001
  #_ REGISTER_IN r8 0x0000000000000002
  addo r6, r4, r5
  addo r3, r7, r8
  blr
  #_ REGISTER_OUT r3 0x0000000000000003
  #_ REGISTER_OUT xer 0x80000000

# A compare into any field copies XER[SO] into that field's summary bit.
test_addo_5:
  #_ REGISTER_IN r4 0x7FFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  #_ REGISTER_IN r6 0x0000000000000000
  addo r3, r4, r5
  cmpwi cr1, r6, 0
  blr
  #_ REGISTER_OUT xer 0xC0000000
  #_ REGISTER_OUT cr 0x03000000

# mtxer clears the sticky bit, and the next compare must not resurrect it.
test_addo_6:
  #_ REGISTER_IN r4 0x7FFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0x0000000000000001
  #_ REGISTER_IN r6 0x0000000000000000
  addo r3, r4, r5
  mtxer r6
  cmpwi cr1, r6, 0
  blr
  #_ REGISTER_OUT xer 0x00000000
  #_ REGISTER_OUT cr 0x02000000

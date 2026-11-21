test_srad_1:
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 0
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 1
  #_ REGISTER_OUT r4 1
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r6 0

test_srad_1_constant:
  li r4, 1
  li r5, 0
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 1
  #_ REGISTER_OUT r4 1
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r6 0

test_srad_2:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 0
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r6 0

test_srad_2_constant:
  li r4, -1
  li r5, 0
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r6 0

test_srad_3:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 1
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT r6 1

test_srad_3_constant:
  li r4, -1
  li r5, 1
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT r6 1

test_srad_4:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 62
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 62
  #_ REGISTER_OUT r6 1

test_srad_4_constant:
  li r4, -1
  li r5, 62
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 62
  #_ REGISTER_OUT r6 1

test_srad_5:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 63
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 63
  #_ REGISTER_OUT r6 1

test_srad_5_constant:
  li r4, -1
  li r5, 63
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 63
  #_ REGISTER_OUT r6 1

test_srad_6:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 64
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 64
  #_ REGISTER_OUT r6 1

test_srad_6_constant:
  li r4, -1
  li r5, 64
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 64
  #_ REGISTER_OUT r6 1

test_srad_7:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r5 100
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 100
  #_ REGISTER_OUT r6 1

test_srad_7_constant:
  li r4, -1
  li r5, 100
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 100
  #_ REGISTER_OUT r6 1

# Only the sign bit set: every bit shifted out for 64 or more sets CA

test_srad_8:
  #_ REGISTER_IN r4 0x8000000000000000
  #_ REGISTER_IN r5 64
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0x8000000000000000
  #_ REGISTER_OUT r5 64
  #_ REGISTER_OUT r6 1

test_srad_9:
  #_ REGISTER_IN r4 0x8000000000000000
  #_ REGISTER_IN r5 127
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0x8000000000000000
  #_ REGISTER_OUT r5 127
  #_ REGISTER_OUT r6 1

test_srad_10:
  #_ REGISTER_IN r4 0x8000000000000000
  #_ REGISTER_IN r5 63
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0xffffffffffffffff
  #_ REGISTER_OUT r4 0x8000000000000000
  #_ REGISTER_OUT r5 63
  #_ REGISTER_OUT r6 0

test_srad_11:
  #_ REGISTER_IN r4 0x4000000000000000
  #_ REGISTER_IN r5 64
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r4 0x4000000000000000
  #_ REGISTER_OUT r5 64
  #_ REGISTER_OUT r6 0

test_srad_12:
  #_ REGISTER_IN r4 0x8000000000000000
  #_ REGISTER_IN r5 128
  srad r3, r4, r5
  adde r6, r0, r0
  blr
  #_ REGISTER_OUT r3 0x8000000000000000
  #_ REGISTER_OUT r4 0x8000000000000000
  #_ REGISTER_OUT r5 128
  #_ REGISTER_OUT r6 0

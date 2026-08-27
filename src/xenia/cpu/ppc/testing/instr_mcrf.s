test_mcrf_1:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN r4 2
  cmpw cr0, r3, r4
  mcrf cr1, cr0
  mfcr r12
  blr
  #_ REGISTER_OUT r3 1
  #_ REGISTER_OUT r4 2
  #_ REGISTER_OUT r12 0x88000000

test_mcrf_1_constant:
  li r3, 1
  li r4, 2
  cmpw cr0, r3, r4
  mcrf cr1, cr0
  mfcr r12
  blr
  #_ REGISTER_OUT r3 1
  #_ REGISTER_OUT r4 2
  #_ REGISTER_OUT r12 0x88000000

test_mcrf_2:
  #_ REGISTER_IN r3 2
  #_ REGISTER_IN r4 1
  cmpw cr0, r3, r4
  mcrf cr2, cr0
  mfcr r12
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT r4 1
  #_ REGISTER_OUT r12 0x40400000

test_mcrf_2_constant:
  li r3, 2
  li r4, 1
  cmpw cr0, r3, r4
  mcrf cr2, cr0
  mfcr r12
  blr
  #_ REGISTER_OUT r3 2
  #_ REGISTER_OUT r4 1
  #_ REGISTER_OUT r12 0x40400000

test_mcrf_3:
  #_ REGISTER_IN r3 5
  #_ REGISTER_IN r4 5
  cmpw cr3, r3, r4
  mcrf cr0, cr3
  mfcr r12
  blr
  #_ REGISTER_OUT r3 5
  #_ REGISTER_OUT r4 5
  # cr0 = eq, cr3 = eq
  #_ REGISTER_OUT r12 0x20020000

test_mcrf_3_constant:
  li r3, 5
  li r4, 5
  cmpw cr3, r3, r4
  mcrf cr0, cr3
  mfcr r12
  blr
  #_ REGISTER_OUT r3 5
  #_ REGISTER_OUT r4 5
  # cr0 = eq, cr3 = eq
  #_ REGISTER_OUT r12 0x20020000

test_mcrf_4:
  #_ REGISTER_IN r3 100
  #_ REGISTER_IN r4 50
  cmpw cr5, r3, r4
  mcrf cr7, cr5
  mfcr r12
  blr
  #_ REGISTER_OUT r3 100
  #_ REGISTER_OUT r4 50
  #_ REGISTER_OUT r12 0x00000404

test_mcrf_4_constant:
  li r3, 100
  li r4, 50
  cmpw cr5, r3, r4
  mcrf cr7, cr5
  mfcr r12
  blr
  #_ REGISTER_OUT r3 100
  #_ REGISTER_OUT r4 50
  #_ REGISTER_OUT r12 0x00000404

test_mcrf_5:
  #_ REGISTER_IN r3 10
  #_ REGISTER_IN r4 20
  #_ REGISTER_IN r5 30
  #_ REGISTER_IN r6 30
  cmpw cr2, r3, r4
  cmpw cr4, r5, r6
  mcrf cr6, cr2
  mfcr r12
  blr
  #_ REGISTER_OUT r3 10
  #_ REGISTER_OUT r4 20
  #_ REGISTER_OUT r5 30
  #_ REGISTER_OUT r6 30
  # cr2 = lt, cr4 = eq, cr6 = lt
  #_ REGISTER_OUT r12 0x00802080

test_mcrf_5_constant:
  li r3, 10
  li r4, 20
  li r5, 30
  li r6, 30
  cmpw cr2, r3, r4
  cmpw cr4, r5, r6
  mcrf cr6, cr2
  mfcr r12
  blr
  #_ REGISTER_OUT r3 10
  #_ REGISTER_OUT r4 20
  #_ REGISTER_OUT r5 30
  #_ REGISTER_OUT r6 30
  # cr2 = lt, cr4 = eq, cr6 = lt
  #_ REGISTER_OUT r12 0x00802080

test_mcrf_6:
  #_ REGISTER_IN r3 5
  #_ REGISTER_IN r4 5
  cmpw cr3, r3, r4
  mcrf cr3, cr3
  mfcr r12
  blr
  #_ REGISTER_OUT r3 5
  #_ REGISTER_OUT r4 5
  # cr3 = eq
  #_ REGISTER_OUT r12 0x00020000

test_mcrf_6_constant:
  li r3, 5
  li r4, 5
  cmpw cr3, r3, r4
  mcrf cr3, cr3
  mfcr r12
  blr
  #_ REGISTER_OUT r3 5
  #_ REGISTER_OUT r4 5
  # cr3 = eq
  #_ REGISTER_OUT r12 0x00020000

test_mcrf_7:
  #_ REGISTER_IN r3 9
  mtcrf 0xFF, r3
  mcrf cr0, cr7
  mfcr r12
  blr
  #_ REGISTER_OUT r3 9
  # cr7 = lt|so, copied to cr0
  #_ REGISTER_OUT r12 0x90000009

test_mcrf_7_constant:
  li r3, 9
  mtcrf 0xFF, r3
  mcrf cr0, cr7
  mfcr r12
  blr
  #_ REGISTER_OUT r3 9
  # cr7 = lt|so, copied to cr0
  #_ REGISTER_OUT r12 0x90000009

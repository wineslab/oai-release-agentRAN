
add_executable(rrc_bearers_test rrc_bearers_test.c ../rrc_gNB_radio_bearers.c ${OPENAIR2_DIR}/LAYER2/nr_pdcp/asn1_utils.c)
add_dependencies(tests rrc_bearers_test)
add_test(NAME rrc_bearers_test COMMAND rrc_bearers_test)
target_include_directories(rrc_bearers_test PRIVATE ./common/utils/ds common/utils/LOG)
target_link_libraries(rrc_bearers_test PRIVATE ds alg asn1_e1ap asn1_lte_rrc_hdrs asn1_nr_rrc_hdrs LOG T)
target_compile_definitions(rrc_bearers_test PRIVATE ENABLE_TESTS)

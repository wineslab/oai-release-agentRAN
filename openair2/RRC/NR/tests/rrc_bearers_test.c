/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "common/utils/utils.h"
#include "common/utils/assertions.h"
#include "openair2/RRC/NR/rrc_gNB_radio_bearers.h"
#include "ds/seq_arr.h"
#include "common/utils/LOG/log.h"

int nr_rlc_get_available_tx_space(int module_id, int rnti, int drb_id) { return 0; }
softmodem_params_t *get_softmodem_params(void) { return NULL; }
configmodule_interface_t *uniqCfg = NULL;

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  printf("detected error at %s:%d:%s: %s\n", file, line, function, s);
  abort();
}

static void test_rrc_pdu_session(void)
{
  const int pdu_session_id = 3;
  gNB_RRC_UE_t ue = {0};
  seq_arr_init(&ue.pduSessions, sizeof(rrc_pdu_session_param_t));

  /* test add */
  pdusession_t p1 = {0};
  seq_arr_init(&p1.qos, sizeof(nr_rrc_qos_t));
  p1.pdusession_id = pdu_session_id;
  p1.n3_incoming.teid = 2002;
  LOG_I(NR_RRC, "Adding first PDU Session with ID %d\n", p1.pdusession_id);
  rrc_pdu_session_param_t *s1 = add_pduSession(&ue.pduSessions, &p1); // add 1st PDU Session
  AssertFatal(s1 != NULL, "Could not add PDU Session\n");
  AssertFatal(s1->param.pdusession_id == p1.pdusession_id, "PDU Session ID mismatch in added PDU Session\n");
  AssertFatal(s1->param.n3_incoming.teid == p1.n3_incoming.teid, "teid mismatch in added PDU Session\n");
  LOG_A(NR_RRC, "First PDU Session added successfully\n");

  /* test find */
  rrc_pdu_session_param_t *a1 = find_pduSession(&ue.pduSessions, p1.pdusession_id);
  AssertFatal(a1 != NULL, "Could not find PDU Session\n");
  AssertFatal(a1 == s1, "Found PDU Session mismatch\n");
  LOG_A(NR_RRC, "PDU Session find test passed\n");

  /* test add duplicate */
  pdusession_t input2 = {0};
  seq_arr_init(&input2.qos, sizeof(nr_rrc_qos_t));
  input2.pdusession_id = pdu_session_id;
  input2.n3_incoming.teid = 9999;
  rrc_pdu_session_param_t *s2 = add_pduSession(&ue.pduSessions, &input2); // add 2nd PDU Session
  AssertFatal(s2 == NULL, "Duplicated PDU session was added!\n");
  LOG_A(NR_RRC, "Duplicated PDU session test passed\n");
  seq_arr_free(&input2.qos, NULL);

  /* add DRB and fetch PDU Session */
  seq_arr_init(&ue.drbs, sizeof(drb_t));
  nr_pdcp_configuration_t pdcp = {.drb.discard_timer = 100, .drb.sn_size = 18, .drb.t_reordering = 50};
  drb_t *added = nr_rrc_add_drb(&ue.drbs, pdu_session_id, &pdcp);
  AssertFatal(added != NULL, "Failed to add DRB");
  rrc_pdu_session_param_t *a2 = find_pduSession_from_drbId(&ue, added->drb_id);
  AssertFatal(a2 && a2->param.pdusession_id == pdu_session_id, "find_pduSession_from_drbId failed");

  seq_arr_free(&ue.pduSessions, free_pdusession);
  seq_arr_free(&ue.drbs, free_drb);
}

// ---------------- DRB TESTS ----------------

static void test_rrc_drb(void)
{
  LOG_I(NR_RRC, "Starting DRB test\n");
  seq_arr_t pduSessions = {0};
  seq_arr_t drbs = {0};
  seq_arr_init(&pduSessions, sizeof(rrc_pdu_session_param_t));
  seq_arr_init(&drbs, sizeof(drb_t));

  /* test add 1 DRB to 1st PDU session */
  const int id1 = 1;
  pdusession_t s1 = {0};
  seq_arr_init(&s1.qos, sizeof(nr_rrc_qos_t));
  s1.pdusession_id = id1;
  add_pduSession(&pduSessions, &s1); // add PDU session

  nr_pdcp_configuration_t pdcp = {.drb.discard_timer = 100, .drb.sn_size = 18, .drb.t_reordering = 50};
  drb_t *in1 = nr_rrc_add_drb(&drbs, id1, &pdcp); // add DRB
  AssertFatal(in1, "add_rrc_drb failed");
  LOG_A(NR_RRC, "First DRB created with ID %d\n", in1->drb_id);

  drb_t *out1 = get_drb(&drbs, 1); // fetch DRB
  AssertFatal(out1 && out1 == in1, "get_drb failed");
  LOG_A(NR_RRC, "DRB retrieval test passed\n");

  /* test add DRB to 2nd PDU session */
  const int id2 = 2;
  pdusession_t s2 = {0};
  seq_arr_init(&s2.qos, sizeof(nr_rrc_qos_t));
  s2.pdusession_id = id2;
  add_pduSession(&pduSessions, &s2); // add 2nd PDU session
  drb_t *in2 = nr_rrc_add_drb(&drbs, id2, &pdcp); // add DRB to 2nd PDU session
  AssertFatal(in2, "add_rrc_drb failed");
  LOG_A(NR_RRC, "Second DRB created with ID %d\n", in2->drb_id);
  drb_t *out2 = get_drb(&drbs, 2);
  AssertFatal(out2 && out2->drb_id == 2, "get_drb failed");
  LOG_A(NR_RRC, "Second DRB retrieval test passed\n");

  seq_arr_free(&pduSessions, free_pdusession);
  seq_arr_free(&drbs, free_drb);
}

// ---------------- DRB TESTS ----------------

static void test_rrc_qos(void)
{
  seq_arr_t pduSessions = {0};
  seq_arr_init(&pduSessions, sizeof(rrc_pdu_session_param_t));

  const int session_id = 70;
  pdusession_t in = {0};
  in.pdusession_id = session_id;
  seq_arr_init(&in.qos, sizeof(nr_rrc_qos_t));
  add_pduSession(&pduSessions, &in);
  LOG_A(NR_RRC, "Created PDU Session %d for QoS test\n", session_id);

  const int qfi = 4;
  const pdusession_level_qos_parameter_t param = { .fiveQI = 9, .fiveQI_type = NON_DYNAMIC, .qfi = qfi };
  LOG_A(NR_RRC, "Adding QoS flow with QFI %d\n", qfi);
  nr_rrc_qos_t *added = add_qos(&in.qos, &param);
  AssertFatal(added, "add_qos failed");
  LOG_A(NR_RRC, "QoS flow added successfully\n");

  nr_rrc_qos_t *found = find_qos(&in.qos, qfi);
  AssertFatal(found && found == added, "find_qos failed");
  LOG_A(NR_RRC, "QoS flow find test passed\n");

  seq_arr_free(&pduSessions, free_pdusession);
}

int main()
{
  // Initialize logging system
  logInit();
  // Set logging level for NR_RRC to show INFO messages
  set_log(NR_RRC, OAILOG_INFO);
  // PDU Session
  test_rrc_pdu_session();
  // DRB
  test_rrc_drb();
  // QoS
  test_rrc_qos();
  LOG_A(NR_RRC, "All RRC Bearers tests passed successfully!\n");
  return 0;
}

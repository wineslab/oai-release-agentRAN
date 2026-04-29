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

#ifndef _RRC_GNB_DRBS_H_
#define _RRC_GNB_DRBS_H_

#include <stdbool.h>
#include <stdint.h>
#include "e1ap_messages_types.h"
#include "nr_rrc_defs.h"

/// @brief retrieve the data structure representing DRB with ID drb_id of UE ue
drb_t *get_drb(seq_arr_t *seq, int id);

/// @brief retrieve PDU session of UE ue with ID id
rrc_pdu_session_param_t *find_pduSession(seq_arr_t *seq, int id);

/// @brief Add a new PDU session for UE @param ue and configuration @param in
rrc_pdu_session_param_t *add_pduSession(seq_arr_t *sessions_ptr, const pdusession_t *in);

/// @brief get PDU session of UE ue through the DRB drb_id
rrc_pdu_session_param_t *find_pduSession_from_drbId(gNB_RRC_UE_t *ue, int drb_id);

/// @brief Remove PDU Session from RRC list
/// Also removes all associated DRBs for this PDU session.
bool rm_pduSession(seq_arr_t *sessions, seq_arr_t *drbs, int pdusession_id);

/// @brief set PDCP configuration in E1 Bearer Context Management message
bearer_context_pdcp_config_t set_bearer_context_pdcp_config(const nr_pdcp_configuration_t pdcp,
                                                            bool um_on_default_drb,
                                                            const nr_redcap_ue_cap_t *redcap_cap);

void free_pdusession(void *ptr);

/// @brief Add DRB to RRC list
drb_t *nr_rrc_add_drb(seq_arr_t *drb_ptr, int pdusession_id, nr_pdcp_configuration_t *pdcp);

/// @brief Function to free DRB in RRC
void free_drb(void *ptr);

/// @brief retrieve QoS flow associated to @param qfi
nr_rrc_qos_t *find_qos(seq_arr_t *seq, int qfi);

/// @brief Add a new QoS to the list
nr_rrc_qos_t *add_qos(seq_arr_t *qos, const pdusession_level_qos_parameter_t *in);

#endif

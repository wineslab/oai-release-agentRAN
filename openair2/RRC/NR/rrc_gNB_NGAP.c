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

/*! \file rrc_gNB_NGAP.h
 * \brief rrc NGAP procedures for gNB
 * \author Yoshio INOUE, Masayuki HARADA
 * \date 2020
 * \version 0.1
 * \email: yoshio.inoue@fujitsu.com,masayuki.harada@fujitsu.com
 *         (yoshio.inoue%40fujitsu.com%2cmasayuki.harada%40fujitsu.com) 
 */

#include "rrc_gNB_NGAP.h"
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <openair3/ocp-gtpu/gtp_itf.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "E1AP_ConfidentialityProtectionIndication.h"
#include "E1AP_IntegrityProtectionIndication.h"
#include "NR_UE-CapabilityRAT-ContainerList.h"
#include "NR_HandoverCommand.h"
#include "NR_HandoverCommand-IEs.h"
#include "E1AP_RLC-Mode.h"
#include "NR_PDCP-Config.h"
#include "F1AP_CauseRadioNetwork.h"
#include "NGAP_CauseRadioNetwork.h"
#include "NGAP_Dynamic5QIDescriptor.h"
#include "NGAP_GTPTunnel.h"
#include "NGAP_NonDynamic5QIDescriptor.h"
#include "NGAP_PDUSessionResourceModifyRequestTransfer.h"
#include "NGAP_PDUSessionResourceSetupRequestTransfer.h"
#include "NGAP_QosFlowAddOrModifyRequestItem.h"
#include "NGAP_QosFlowSetupRequestItem.h"
#include "NGAP_asn_constant.h"
#include "NGAP_ProtocolIE-Field.h"
#include "NGAP_CellSize.h"
#include "NR_UE-NR-Capability.h"
#include "NR_UERadioAccessCapabilityInformation.h"
#include "MAC/mac.h"
#include "OCTET_STRING.h"
#include "RRC/NR/MESSAGES/asn1_msg.h"
#include "RRC/NR/nr_rrc_common.h"
#include "RRC/NR/nr_rrc_defs.h"
#include "RRC/NR/nr_rrc_proto.h"
#include "RRC/NR/rrc_gNB_UE_context.h"
#include "RRC/NR/rrc_gNB_radio_bearers.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac.h"
#include "T.h"
#include "aper_decoder.h"
#include "asn_codecs.h"
#include "assertions.h"
#include "common/ngran_types.h"
#include "common/platform_constants.h"
#include "common/ran_context.h"
#include "common/utils/T/T.h"
#include "constr_TYPE.h"
#include "conversions.h"
#include "e1ap_messages_types.h"
#include "f1ap_messages_types.h"
#include "gtpv1_u_messages_types.h"
#include "intertask_interface.h"
#include "nr_pdcp/nr_pdcp_entity.h"
#include "nr_pdcp/nr_pdcp_oai_api.h"
#include "oai_asn1.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "openair3/SECU/key_nas_deriver.h"
#include "rrc_messages_types.h"
#include "s1ap_messages_types.h"
#include "uper_encoder.h"
#include "rrc_gNB_mobility.h"
#include "rrc_gNB_du.h"
#include "common/utils/alg/find.h"

#ifdef E2_AGENT
#include "openair2/E2AP/RAN_FUNCTION/O-RAN/ran_func_rc_extern.h"
#endif

/* Masks for NGAP Encryption algorithms, NEA0 is always supported (not coded) */
static const uint16_t NGAP_ENCRYPTION_NEA1_MASK = 0x8000;
static const uint16_t NGAP_ENCRYPTION_NEA2_MASK = 0x4000;
static const uint16_t NGAP_ENCRYPTION_NEA3_MASK = 0x2000;

/* Masks for NGAP Integrity algorithms, NIA0 is always supported (not coded) */
static const uint16_t NGAP_INTEGRITY_NIA1_MASK = 0x8000;
static const uint16_t NGAP_INTEGRITY_NIA2_MASK = 0x4000;
static const uint16_t NGAP_INTEGRITY_NIA3_MASK = 0x2000;

#define INTEGRITY_ALGORITHM_NONE NR_IntegrityProtAlgorithm_nia0

static void set_UE_security_algos(const gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, const ngap_security_capabilities_t *cap);

/** @brief Validates PLMN against allowed PLMN list and returns pointer to matching PLMN
 * @param rrc RRC instance
 * @param plmn PLMN to validate
 * @return pointer to matching PLMN in configuration, or NULL if not found */
static const plmn_id_t *get_serving_plmn(gNB_RRC_INST *rrc, const plmn_id_t *plmn)
{
  // Find matching PLMN in configuration
  for (int idx = 0; idx < rrc->configuration.num_plmn; idx++) {
    const plmn_id_t *allowed = &rrc->configuration.plmn[idx];
    if (allowed->mcc == plmn->mcc && allowed->mnc == plmn->mnc && allowed->mnc_digit_length == plmn->mnc_digit_length) {
      LOG_D(NR_RRC, "PLMN (MCC:%d, MNC:%*d) matched with allowed PLMN[%d]\n", plmn->mcc, plmn->mnc_digit_length, plmn->mnc, idx);
      return allowed;
    }
  }
  LOG_W(NR_RRC, "PLMN (MCC:%d, MNC:%*d) not found in allowed PLMN list\n", plmn->mcc, plmn->mnc_digit_length, plmn->mnc);
  return NULL;
}

/*!
 *\brief save security key.
 *\param UE              UE context.
 *\param security_key_pP The security key received from NGAP.
 */
static void set_UE_security_key(gNB_RRC_UE_t *UE, uint8_t *security_key_pP)
{
  int i;

  /* Saves the security key */
  memcpy(UE->kgnb, security_key_pP, SECURITY_KEY_LENGTH);
  memset(UE->nh, 0, SECURITY_KEY_LENGTH);
  /* 3GPP TS 33.501 §6.9.2.1.1: On Initial Context Setup, AMF does not send NH;
   * gNB shall initialize NCC to 0. */
  UE->nh_ncc = 0;

  char ascii_buffer[65];
  for (i = 0; i < 32; i++) {
    sprintf(&ascii_buffer[2 * i], "%02X", UE->kgnb[i]);
  }
  ascii_buffer[2 * 1] = '\0';

  LOG_I(NR_RRC, "[UE %x] Saved security key %s\n", UE->rnti, ascii_buffer);
}

void nr_rrc_pdcp_config_security(gNB_RRC_UE_t *UE, bool enable_ciphering)
{
  static int                          print_keys= 1;

  /* Derive the keys from kgnb */
  nr_pdcp_entity_security_keys_and_algos_t security_parameters;
  /* set ciphering algorithm depending on 'enable_ciphering' */
  security_parameters.ciphering_algorithm = enable_ciphering ? UE->ciphering_algorithm : 0;
  security_parameters.integrity_algorithm = UE->integrity_algorithm;
  /* use current ciphering algorithm, independently of 'enable_ciphering' to derive ciphering key */
  nr_derive_key(RRC_ENC_ALG, UE->ciphering_algorithm, UE->kgnb, security_parameters.ciphering_key);
  nr_derive_key(RRC_INT_ALG, UE->integrity_algorithm, UE->kgnb, security_parameters.integrity_key);

  if ( LOG_DUMPFLAG( DEBUG_SECURITY ) ) {
    if (print_keys == 1 ) {
      print_keys =0;
      LOG_DUMPMSG(NR_RRC, DEBUG_SECURITY, UE->kgnb, 32, "\nKgNB:");
      LOG_DUMPMSG(NR_RRC, DEBUG_SECURITY, security_parameters.ciphering_key, 16,"\nKRRCenc:" );
      LOG_DUMPMSG(NR_RRC, DEBUG_SECURITY, security_parameters.integrity_key, 16,"\nKRRCint:" );
    }
  }

  nr_pdcp_config_set_security(UE->rrc_ue_id, DL_SCH_LCID_DCCH, true, &security_parameters);
}

/** @brief Process AMF Identifier and fill GUAMI struct members */
static nr_guami_t get_guami(const uint32_t amf_Id, const plmn_id_t plmn)
{
  nr_guami_t guami = {0};
  guami.amf_region_id = (amf_Id >> 16) & 0xff;
  guami.amf_set_id = (amf_Id >> 6) & 0x3ff;
  guami.amf_pointer = amf_Id & 0x3f;
  guami.plmn = plmn;
  return guami;
}

/** @brief Copy NGAP PDU Session Transfer item to RRC pdusession_t struct */
static void cp_pdusession_transfer_to_pdusession(pdusession_t *dst, const pdusession_transfer_t *src)
{
  dst->pdu_session_type = src->pdu_session_type;
  dst->n3_incoming = src->n3_incoming;
  /* QoS handling */
  DevAssert(!dst->qos.data);
  DevAssert(src->nb_qos < MAX_QOS_FLOWS);
  // Initialise mapped QoS list per PDU Session
  seq_arr_init(&dst->qos, sizeof(nr_rrc_qos_t));
  // Add QoS flow to list
  for (uint8_t i = 0; i < src->nb_qos; ++i) {
    if (!add_qos(&dst->qos, &src->qos[i])) {
      LOG_E(NR_RRC, "Failed to add QoS flow %d for PDU session %d\n", src->qos[i].qfi, dst->pdusession_id);
      continue;
    }
  }
}

/** @brief Copy NGAP PDU Session Resource item to RRC pdusession_t struct to setup */
static void cp_pdusession_resource_item_to_pdusession(pdusession_t *dst, const pdusession_resource_item_t *src)
{
  dst->pdusession_id = src->pdusession_id;
  dst->nas_pdu = src->nas_pdu;
  dst->nssai = src->nssai;

  // Use the PDU session transfer function to handle QoS and other PDU session transfer-specific fields
  cp_pdusession_transfer_to_pdusession(dst, &src->pdusessionTransfer);
}

/**
 * @brief Prepare the Initial UE Message (Uplink NAS) to be forwarded to the AMF over N2
 *        extracts NAS PDU, Selected PLMN and Registered AMF from the RRCSetupComplete
 */
void rrc_gNB_send_NGAP_NAS_FIRST_REQ(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, NR_RRCSetupComplete_IEs_t *rrcSetupComplete)
{
  MessageDef *message_p = itti_alloc_new_message(TASK_RRC_GNB, rrc->module_id, NGAP_NAS_FIRST_REQ);
  ngap_nas_first_req_t *req = &NGAP_NAS_FIRST_REQ(message_p);
  memset(req, 0, sizeof(*req));

  // RAN UE NGAP ID
  req->gNB_ue_ngap_id = UE->rrc_ue_id;

  // RRC Establishment Cause
  /* Assume that cause is coded in the same way in RRC and NGap, just check that the value is in NGap range */
  AssertFatal(UE->establishment_cause < NGAP_RRC_CAUSE_LAST, "Establishment cause invalid (%jd/%d)!", UE->establishment_cause, NGAP_RRC_CAUSE_LAST);
  req->establishment_cause = UE->establishment_cause;

  // NAS-PDU
  req->nas_pdu = create_byte_array(rrcSetupComplete->dedicatedNAS_Message.size, rrcSetupComplete->dedicatedNAS_Message.buf);

  /* Selected PLMN Identity (Optional)
   * selectedPLMN-Identity in RRCSetupComplete: Index of the PLMN selected by the UE from the plmn-IdentityInfoList (SIB1) */
  int idx = rrcSetupComplete->selectedPLMN_Identity - 1; // Convert 1-based PLMN Identity IE to 0-based index
  if (idx < 0 || idx >= rrc->configuration.num_plmn) {
    LOG_E(NGAP,
          "Failed to send Initial UE Message: selected PLMN index (%d) is out of bounds [0..%d)\n",
          idx,
          rrc->configuration.num_plmn);
    return;
  }
  req->plmn = rrc->configuration.plmn[idx]; // Select from the stored list
  /* UE is connected to only one PLMN at a time: store this as the serving PLMN */
  UE->serving_plmn = req->plmn;

  // Cell ID (NR CGI)
  req->nr_cell_id = UE->nr_cellid;
  // PLMN (NR CGI and TAI)
  plmn_id_t *p = &req->plmn;
  LOG_I(NGAP, "Selected PLMN in the NG Initial UE Message: MCC=%03d MNC=%0*d\n", p->mcc, p->mnc_digit_length, p->mnc);

  /* 5G-S-TMSI */
  if (UE->Initialue_identity_5g_s_TMSI.presence) {
    req->ue_identity.presenceMask |= NGAP_UE_IDENTITIES_FiveG_s_tmsi;
    req->ue_identity.s_tmsi.amf_set_id = UE->Initialue_identity_5g_s_TMSI.amf_set_id;
    req->ue_identity.s_tmsi.amf_pointer = UE->Initialue_identity_5g_s_TMSI.amf_pointer;
    req->ue_identity.s_tmsi.m_tmsi = UE->Initialue_identity_5g_s_TMSI.fiveg_tmsi;
  }

  /* Process Registered AMF IE */
  if (rrcSetupComplete->registeredAMF != NULL) {
    /* Fetch the AMF-Identifier from the registeredAMF IE
     * The IE AMF-Identifier (AMFI) comprises of an AMF Region ID (8b),
     * an AMF Set ID (10b) and an AMF Pointer (6b)
     * as specified in TS 23.003 [21], clause 2.10.1. */
    NR_RegisteredAMF_t *r_amf = rrcSetupComplete->registeredAMF;
    req->ue_identity.presenceMask |= NGAP_UE_IDENTITIES_guami;
    uint32_t amf_Id = BIT_STRING_to_uint32(&r_amf->amf_Identifier);
    UE->ue_guami = req->ue_identity.guami = get_guami(amf_Id, req->plmn);
    LOG_I(NGAP,
          "GUAMI in NGAP_NAS_FIRST_REQ (UE %04x): AMF Set ID %u, Region ID %u, Pointer %u\n",
          UE->rnti,
          req->ue_identity.guami.amf_set_id,
          req->ue_identity.guami.amf_region_id,
          req->ue_identity.guami.amf_pointer);
  }

  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, message_p);
}

/** @brief Returns an instance of E1AP DRB To Setup List */
static DRB_nGRAN_to_setup_t fill_e1_drb_to_setup(const drb_t *rrc_drb,
                                                 const pdusession_t *session,
                                                 bool const um_on_default_drb,
                                                 const nr_redcap_ue_cap_t *redcap_cap)
{
  DRB_nGRAN_to_setup_t drb_ngran = {0};
  drb_ngran.id = rrc_drb->drb_id;

  drb_ngran.sdap_config.defaultDRB = true;
  drb_ngran.sdap_config.sDAP_Header_UL = session->sdap_config.header_ul_absent ? 1 : 0;
  drb_ngran.sdap_config.sDAP_Header_DL = session->sdap_config.header_dl_absent ? 1 : 0;

  drb_ngran.pdcp_config = set_bearer_context_pdcp_config(rrc_drb->pdcp_config, um_on_default_drb, redcap_cap);

  drb_ngran.numCellGroups = 1;
  for (int k = 0; k < drb_ngran.numCellGroups; k++) {
    drb_ngran.cellGroupList[k] = MCG; // 1 cellGroup only
  }

  FOR_EACH_SEQ_ARR(nr_rrc_qos_t *, qos, &session->qos) {
    pdusession_level_qos_parameter_t *qos_session = &qos->qos;
    DevAssert(drb_ngran.numQosFlow2Setup < MAX_QOS_FLOWS);
    qos_flow_to_setup_t *qos_flow = &drb_ngran.qosFlows[drb_ngran.numQosFlow2Setup++];
    qos_characteristics_t *qos_char = &qos_flow->qos_params.qos_characteristics;
    qos_flow->qfi = qos_session->qfi;
    qos_char->qos_type = qos_session->fiveQI_type;
    if (qos_char->qos_type == DYNAMIC) {
      qos_char->dynamic.fiveqi = qos_session->fiveQI;
      qos_char->dynamic.qos_priority_level = qos_session->qos_priority;
    } else {
      qos_char->non_dynamic.fiveqi = qos_session->fiveQI;
      qos_char->non_dynamic.qos_priority_level = qos_session->qos_priority;
    }

    ngran_allocation_retention_priority_t *arp_out = &qos_flow->qos_params.alloc_reten_priority;
    qos_arp_t *arp_in = &qos_session->arp;
    arp_out->priority_level = arp_in->priority_level;
    arp_out->preemption_capability = arp_in->pre_emp_capability;
    arp_out->preemption_vulnerability = arp_in->pre_emp_vulnerability;
  }

  return drb_ngran;
}

static nr_sdap_configuration_t get_sdap_config(const bool enable_sdap)
{
  nr_sdap_configuration_t sdap = {.header_dl_absent = !enable_sdap, .header_ul_absent = !enable_sdap};
  return sdap;
}

/**
 * @brief Triggers bearer setup for the specified UE.
 *
 * This function initiates the setup of bearer contexts for a given UE
 * by preparing and sending an E1AP Bearer Setup Request message to the CU-UP.
 *
 * @return True if bearer setup was successfully initiated, false otherwise
 * @retval true Bearer setup was initiated successfully
 * @retval false No CU-UP is associated, so bearer setup could not be initiated
 *
 * @note the return value is expected to be used (as per declaration)
 *
 */
bool trigger_bearer_setup(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, int n, pdusession_t *sessions, uint64_t ueAggMaxBitRateDownlink)
{
  AssertFatal(UE->as_security_active, "logic bug: security should be active when activating DRBs\n");
  e1ap_bearer_setup_req_t bearer_req = {0};

  // Reject bearers setup if there's no CU-UP associated
  if (!is_cuup_associated(rrc)) {
    return false;
  }

  e1ap_nssai_t cuup_nssai = {0};
  for (int i = 0; i < n; i++) {
    // Retrieve SDAP configuration and PDU Session to the list
    sessions[i].sdap_config = get_sdap_config(rrc->configuration.enable_sdap);
    rrc_pdu_session_param_t *pduSession = add_pduSession(&UE->pduSessions, &sessions[i]);
    if (!pduSession) {
      LOG_E(NR_RRC, "Could not add PDU Session %d for UE %d\n", sessions[i].pdusession_id, UE->rrc_ue_id);
      continue;
    }
    pdusession_t *session = &pduSession->param;
    // Fill E1 Bearer Context Modification Request
    bearer_req.gNB_cu_cp_ue_id = UE->rrc_ue_id;
    security_information_t *secInfo = &bearer_req.secInfo;
    secInfo->cipheringAlgorithm = rrc->security.do_drb_ciphering ? UE->ciphering_algorithm : 0;
    secInfo->integrityProtectionAlgorithm = rrc->security.do_drb_integrity ? UE->integrity_algorithm : 0;
    nr_derive_key(UP_ENC_ALG, secInfo->cipheringAlgorithm, UE->kgnb, (uint8_t *)secInfo->encryptionKey);
    nr_derive_key(UP_INT_ALG, secInfo->integrityProtectionAlgorithm, UE->kgnb, (uint8_t *)secInfo->integrityProtectionKey);
    bearer_req.ueDlAggMaxBitRate = ueAggMaxBitRateDownlink;
    pdu_session_to_setup_t *pdu = bearer_req.pduSession + bearer_req.numPDUSessions;
    bearer_req.numPDUSessions++;
    pdu->sessionId = session->pdusession_id;
    pdu->nssai = sessions[i].nssai;
    if (cuup_nssai.sst == 0)
      cuup_nssai = pdu->nssai; /* for CU-UP selection below */

    security_indication_t *sec = &pdu->securityIndication;
    sec->integrityProtectionIndication = rrc->security.do_drb_integrity ? SECURITY_REQUIRED
                                                                        : SECURITY_NOT_NEEDED;
    sec->confidentialityProtectionIndication = rrc->security.do_drb_ciphering ? SECURITY_REQUIRED
                                                                              : SECURITY_NOT_NEEDED;
    gtpu_tunnel_t *n3_incoming = &session->n3_incoming;
    pdu->UP_TL_information.teId = n3_incoming->teid;
    memcpy(&pdu->UP_TL_information.tlAddress, n3_incoming->addr.buffer, sizeof(in_addr_t));
    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, n3_incoming->addr.buffer, ip_str, sizeof(ip_str));
    LOG_I(NR_RRC, "Bearer Context Setup: PDU Session ID=%d, incoming TEID=0x%08x, Addr=%s\n", session->pdusession_id, n3_incoming->teid, ip_str);

    pdu->numDRB2Setup = 1; // One DRB per PDU Session. TODO: Remove hardcoding
    for (int j = 0; j < pdu->numDRB2Setup; j++) {
      drb_t *rrc_drb = nr_rrc_add_drb(&UE->drbs, session->pdusession_id, &rrc->pdcp_config);
      if (!rrc_drb) {
        LOG_E(NR_RRC, "UE %d: failed to add DRB for PDU session ID %d\n", UE->rrc_ue_id, session->pdusession_id);
        return false;
      }
      DevAssert(seq_arr_size(&pduSession->param.qos) == 1);
      nr_rrc_qos_t *qos = (nr_rrc_qos_t *)seq_arr_at(&pduSession->param.qos, 0);
      qos->drb_id = rrc_drb->drb_id; // map DRB to QFI
      LOG_I(NR_RRC, "UE %d: added DRB %d for PDU session ID %d\n", UE->rrc_ue_id, rrc_drb->drb_id, rrc_drb->pdusession_id);
      pdu->DRBnGRanList[0] = fill_e1_drb_to_setup(rrc_drb, session, rrc->configuration.um_on_default_drb, UE->redcap_cap);
    }
  }
  /* Limitation: we assume one fixed CU-UP per UE. We base the selection on
   * NSSAI, but the UE might have multiple PDU sessions with differing slices,
   * in which we might need to select different CU-UPs. In this case, we would
   * actually need to group the E1 bearer context setup for the different
   * CU-UPs, and send them to the different CU-UPs. */
  sctp_assoc_t assoc_id = get_new_cuup_for_ue(rrc, UE, cuup_nssai.sst, cuup_nssai.sd);
  rrc->cucp_cuup.bearer_context_setup(assoc_id, &bearer_req);
  return true;
}

/**
 * @brief Fill PDU Session Resource Failed to Setup Item of the
 *        PDU Session Resource Failed to Setup List for either:
 *        - NGAP PDU Session Resource Setup Response
 *        - NGAP Initial Context Setup Response
 */
static void fill_pdu_session_resource_failed_to_setup_item(pdusession_failed_t *f, int pdusession_id, ngap_cause_t cause)
{
  f->pdusession_id = pdusession_id;
  f->cause = cause;
}

/**
 * @brief Fill Initial Context Setup Response with a PDU Session Resource Failed to Setup List
 *        and send ITTI message to TASK_NGAP
 */
static void send_ngap_initial_context_setup_resp_fail(instance_t instance,
                                                      ngap_initial_context_setup_req_t *msg,
                                                      ngap_cause_t cause)
{
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, instance, NGAP_INITIAL_CONTEXT_SETUP_RESP);
  ngap_initial_context_setup_resp_t *resp = &NGAP_INITIAL_CONTEXT_SETUP_RESP(msg_p);
  resp->gNB_ue_ngap_id = msg->gNB_ue_ngap_id;

  for (int i = 0; i < msg->nb_of_pdusessions; i++) {
    fill_pdu_session_resource_failed_to_setup_item(&resp->pdusessions_failed[i], msg->pdusession[i].pdusession_id, cause);
  }
  resp->nb_of_pdusessions = 0;
  resp->nb_of_pdusessions_failed = msg->nb_of_pdusessions;
  itti_send_msg_to_task(TASK_NGAP, instance, msg_p);
}

//------------------------------------------------------------------------------
int rrc_gNB_process_NGAP_INITIAL_CONTEXT_SETUP_REQ(MessageDef *msg_p, instance_t instance)
//------------------------------------------------------------------------------
{
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  ngap_initial_context_setup_req_t *req = &NGAP_INITIAL_CONTEXT_SETUP_REQ(msg_p);

  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, req->gNB_ue_ngap_id);
  if (ue_context_p == NULL) {
    /* Can not associate this message to an UE index, send a failure to NGAP and discard it! */
    LOG_W(NR_RRC, "[gNB %ld] In NGAP_INITIAL_CONTEXT_SETUP_REQ: unknown UE from NGAP ids (%u)\n", instance, req->gNB_ue_ngap_id);
    ngap_cause_t cause = { .type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_LOCAL_UE_NGAP_ID};
    rrc_gNB_send_NGAP_INITIAL_CONTEXT_SETUP_FAIL(req->gNB_ue_ngap_id,
                                                 NULL,
                                                 cause);
    return (-1);
  }
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  UE->amf_ue_ngap_id = req->amf_ue_ngap_id;

  // Directly copy the entire guami structure
  UE->ue_guami = req->guami;

  /* NAS PDU */
  // this is malloced pointers, we pass it for later free()
  UE->nas_pdu = req->nas_pdu;

  if (req->nb_of_pdusessions > 0) {
    /* if there are PDU sessions to setup, store them to be created once
     * security (and UE capabilities) are received */
    UE->n_initial_pdu = req->nb_of_pdusessions;
    UE->initial_pdus = calloc_or_fail(UE->n_initial_pdu, sizeof(*UE->initial_pdus));
    for (int i = 0; i < UE->n_initial_pdu; ++i)
      cp_pdusession_resource_item_to_pdusession(&UE->initial_pdus[i], &req->pdusession[i]);
  }

  /* security */
  set_UE_security_algos(rrc, UE, &req->security_capabilities);
  set_UE_security_key(UE, req->security_key);

  /* TS 38.413: "store the received Security Key in the UE context and, if the
   * NG-RAN node is required to activate security for the UE, take this
   * security key into use.": I interpret this as "if AS security is already
   * active, don't do anything" */
  if (!UE->as_security_active) {
    /* configure only integrity, ciphering comes after receiving SecurityModeComplete */
    nr_rrc_pdcp_config_security(UE, false);
    rrc_gNB_generate_SecurityModeCommand(rrc, UE);
  } else {
    /* if AS security key is active, we also have the UE capabilities. Then,
     * there are two possibilities: we should set up PDU sessions, and/or
     * forward the NAS message. */
    if (req->nb_of_pdusessions > 0) {
      // do not remove the above allocation which is reused here: this is used
      // in handle_rrcReconfigurationComplete() to know that we need to send a
      // Initial context setup response message
      if (!trigger_bearer_setup(rrc, UE, UE->n_initial_pdu, UE->initial_pdus, 0)) {
        LOG_W(NR_RRC, "UE %d: reject PDU Session Setup in Initial Context Setup Response\n", UE->rrc_ue_id);
        ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_RESOURCES_NOT_AVAILABLE_FOR_THE_SLICE};
        send_ngap_initial_context_setup_resp_fail(rrc->module_id, req, cause);
        rrc_forward_ue_nas_message(rrc, UE);
        return -1;
      }
    } else {
      /* no PDU sesion to setup: acknowledge this message, and forward NAS
       * message, if required */
      rrc_gNB_send_NGAP_INITIAL_CONTEXT_SETUP_RESP(rrc, UE);
      rrc_forward_ue_nas_message(rrc, UE);
    }
  }

#ifdef E2_AGENT
  signal_rrc_state_changed_to(UE, RRC_CONNECTED_RRC_STATE_E2SM_RC);
#endif

  return 0;
}

static gtpu_tunnel_t cp_gtp_tunnel(const gtpu_tunnel_t in)
{
  gtpu_tunnel_t out = {0};
  out.teid = in.teid;
  out.addr.length = in.addr.length;
  memcpy(out.addr.buffer, in.addr.buffer, out.addr.length);
  return out;
}

/** @brief Fill NG PDU Session Setup item from stored setup PDU Session in RRC list */
static pdusession_setup_t fill_ngap_pdusession_setup(pdusession_t *session)
{
  DevAssert(session != NULL);
  pdusession_setup_t out = {0};
  out.pdusession_id = session->pdusession_id;
  out.pdu_session_type = session->pdu_session_type;
  out.n3_outgoing = cp_gtp_tunnel(session->n3_outgoing);
  FOR_EACH_SEQ_ARR(nr_rrc_qos_t *, qos_session, &session->qos) {
    DevAssert(out.nb_of_qos_flow < MAX_QOS_FLOWS);
    pdusession_associate_qosflow_t *q = &out.associated_qos_flows[out.nb_of_qos_flow++];
    q->qfi = qos_session->qos.qfi;
    q->qos_flow_mapping_ind = QOSFLOW_MAPPING_INDICATION_DL;
  }
  char ip_str[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, out.n3_outgoing.addr.buffer, ip_str, sizeof(ip_str));
  LOG_I(NR_RRC, "PDU Session Setup: ID=%d, outgoing TEID=0x%08x, Addr=%s\n", out.pdusession_id, out.n3_outgoing.teid, ip_str);
  return out;
}

void rrc_gNB_send_NGAP_INITIAL_CONTEXT_SETUP_RESP(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE)
{
  MessageDef *msg_p = NULL;
  int pdu_sessions_done = 0;
  int pdu_sessions_failed = 0;
  msg_p = itti_alloc_new_message (TASK_RRC_ENB, rrc->module_id, NGAP_INITIAL_CONTEXT_SETUP_RESP);
  ngap_initial_context_setup_resp_t *resp = &NGAP_INITIAL_CONTEXT_SETUP_RESP(msg_p);

  resp->gNB_ue_ngap_id = UE->rrc_ue_id;

  // Optional: PDU Sessions to Setup
  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
    if (session->status == PDU_SESSION_STATUS_DONE) {
      resp->pdusessions[pdu_sessions_done++] = fill_ngap_pdusession_setup(&session->param);
    } else if (session->status != PDU_SESSION_STATUS_ESTABLISHED) {
      session->status = PDU_SESSION_STATUS_FAILED;
      ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_PDU_SESSION_ID};
      fill_pdu_session_resource_failed_to_setup_item(&resp->pdusessions_failed[pdu_sessions_failed],
                                                     session->param.pdusession_id,
                                                     cause);
      pdu_sessions_failed++;
    }
  }

  resp->nb_of_pdusessions = pdu_sessions_done;
  resp->nb_of_pdusessions_failed = pdu_sessions_failed;
  itti_send_msg_to_task (TASK_NGAP, rrc->module_id, msg_p);
}

void rrc_gNB_send_NGAP_INITIAL_CONTEXT_SETUP_FAIL(uint32_t gnb,
                                                  const rrc_gNB_ue_context_t *const ue_context_pP,
                                                  const ngap_cause_t causeP)
{
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_INITIAL_CONTEXT_SETUP_FAIL);
  ngap_initial_context_setup_fail_t *fail = &NGAP_INITIAL_CONTEXT_SETUP_FAIL(msg_p);
  memset(fail, 0, sizeof(*fail));
  fail->gNB_ue_ngap_id = gnb;
  fail->cause = causeP;
  itti_send_msg_to_task(TASK_NGAP, 0, msg_p);
}

static NR_CipheringAlgorithm_t rrc_gNB_select_ciphering(const gNB_RRC_INST *rrc, uint16_t algorithms)
{
  int i;
  /* preset nea0 as fallback */
  int ret = 0;

  /* Select ciphering algorithm based on gNB configuration file and
   * UE's supported algorithms.
   * We take the first from the list that is supported by the UE.
   * The ordering of the list comes from the configuration file.
   */
  for (i = 0; i < rrc->security.ciphering_algorithms_count; i++) {
    int nea_mask[4] = {
      0,
      NGAP_ENCRYPTION_NEA1_MASK,
      NGAP_ENCRYPTION_NEA2_MASK,
      NGAP_ENCRYPTION_NEA3_MASK
    };
    if (rrc->security.ciphering_algorithms[i] == 0) {
      /* nea0 */
      break;
    }
    if (algorithms & nea_mask[rrc->security.ciphering_algorithms[i]]) {
      ret = rrc->security.ciphering_algorithms[i];
      break;
    }
  }

  LOG_D(RRC, "selecting ciphering algorithm %d\n", ret);

  return ret;
}

static e_NR_IntegrityProtAlgorithm rrc_gNB_select_integrity(const gNB_RRC_INST *rrc, uint16_t algorithms)
{
  int i;
  /* preset nia0 as fallback */
  int ret = 0;

  /* Select integrity algorithm based on gNB configuration file and
   * UE's supported algorithms.
   * We take the first from the list that is supported by the UE.
   * The ordering of the list comes from the configuration file.
   */
  for (i = 0; i < rrc->security.integrity_algorithms_count; i++) {
    int nia_mask[4] = {
      0,
      NGAP_INTEGRITY_NIA1_MASK,
      NGAP_INTEGRITY_NIA2_MASK,
      NGAP_INTEGRITY_NIA3_MASK
    };
    if (rrc->security.integrity_algorithms[i] == 0) {
      /* nia0 */
      break;
    }
    if (algorithms & nia_mask[rrc->security.integrity_algorithms[i]]) {
      ret = rrc->security.integrity_algorithms[i];
      break;
    }
  }

  LOG_D(RRC, "selecting integrity algorithm %d\n", ret);

  return ret;
}

/*
 * \brief set security algorithms
 * \param rrc     pointer to RRC context
 * \param UE      UE context
 * \param cap     security capabilities for this UE
 */
static void set_UE_security_algos(const gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, const ngap_security_capabilities_t *cap)
{
  /* Save security parameters */
  UE->security_capabilities = *cap;

  /* Select relevant algorithms */
  NR_CipheringAlgorithm_t cipheringAlgorithm = rrc_gNB_select_ciphering(rrc, cap->nRencryption_algorithms);
  e_NR_IntegrityProtAlgorithm integrityProtAlgorithm = rrc_gNB_select_integrity(rrc, cap->nRintegrity_algorithms);

  UE->ciphering_algorithm = cipheringAlgorithm;
  UE->integrity_algorithm = integrityProtAlgorithm;

  LOG_UE_EVENT(UE,
               "Selected security algorithms: ciphering %lx, integrity %x\n",
               cipheringAlgorithm,
               integrityProtAlgorithm);
}

//------------------------------------------------------------------------------
int rrc_gNB_process_NGAP_DOWNLINK_NAS(MessageDef *msg_p, instance_t instance, mui_t *rrc_gNB_mui)
//------------------------------------------------------------------------------
{
  ngap_downlink_nas_t *req = &NGAP_DOWNLINK_NAS(msg_p);
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, req->gNB_ue_ngap_id);

  if (ue_context_p == NULL) {
    /* Can not associate this message to an UE index, send a failure to NGAP and discard it! */
    MessageDef *msg_fail_p;
    LOG_W(NR_RRC, "[gNB %ld] In NGAP_DOWNLINK_NAS: unknown UE from NGAP ids (%u)\n", instance, req->gNB_ue_ngap_id);
    msg_fail_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_NAS_NON_DELIVERY_IND);
    ngap_nas_non_delivery_ind_t *msg = &NGAP_NAS_NON_DELIVERY_IND(msg_fail_p);
    msg->gNB_ue_ngap_id = req->gNB_ue_ngap_id;
    msg->nas_pdu = req->nas_pdu;
    // TODO add failure cause when defined!
    itti_send_msg_to_task(TASK_NGAP, instance, msg_fail_p);
    return (-1);
  }

  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  UE->amf_ue_ngap_id = req->amf_ue_ngap_id;
  UE->nas_pdu = req->nas_pdu;
  rrc_forward_ue_nas_message(rrc, UE);
  return 0;
}

void rrc_gNB_send_NGAP_UPLINK_NAS(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, const NR_UL_DCCH_Message_t *const ul_dcch_msg)
{
  NR_ULInformationTransfer_t *ulInformationTransfer = ul_dcch_msg->message.choice.c1->choice.ulInformationTransfer;

  NR_ULInformationTransfer__criticalExtensions_PR p = ulInformationTransfer->criticalExtensions.present;
  if (p != NR_ULInformationTransfer__criticalExtensions_PR_ulInformationTransfer) {
    LOG_E(NR_RRC, "UE %d: expected presence of ulInformationTransfer, but message has %d\n", UE->rrc_ue_id, p);
    return;
  }

  NR_DedicatedNAS_Message_t *nas = ulInformationTransfer->criticalExtensions.choice.ulInformationTransfer->dedicatedNAS_Message;
  if (!nas) {
    LOG_E(NR_RRC, "UE %d: expected NAS message in ulInformation, but it is NULL\n", UE->rrc_ue_id);
    return;
  }

  uint8_t *buf = malloc_or_fail(nas->size);
  memcpy(buf, nas->buf, nas->size);
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, rrc->module_id, NGAP_UPLINK_NAS);
  NGAP_UPLINK_NAS(msg_p).gNB_ue_ngap_id = UE->rrc_ue_id;
  NGAP_UPLINK_NAS(msg_p).nas_pdu.len = nas->size;
  NGAP_UPLINK_NAS(msg_p).nas_pdu.buf = buf;
  /* Fill PLMN and location info: use serving PLMN of the UE */
  NGAP_UPLINK_NAS(msg_p).plmn = UE->serving_plmn;
  NGAP_UPLINK_NAS(msg_p).nr_cell_id = rrc->nr_cellid;
  NGAP_UPLINK_NAS(msg_p).tac = rrc->configuration.tac;
  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
}

void rrc_gNB_send_NGAP_PDUSESSION_SETUP_RESP(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, uint8_t xid)
{
  MessageDef *msg_p;
  int pdu_sessions_done = 0;
  int pdu_sessions_failed = 0;

  msg_p = itti_alloc_new_message (TASK_RRC_GNB, rrc->module_id, NGAP_PDUSESSION_SETUP_RESP);
  ngap_pdusession_setup_resp_t *resp = &NGAP_PDUSESSION_SETUP_RESP(msg_p);
  resp->gNB_ue_ngap_id = UE->rrc_ue_id;

  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
    if (session->status == PDU_SESSION_STATUS_DONE) {
      resp->pdusessions[pdu_sessions_done++] = fill_ngap_pdusession_setup(&session->param);
      session->status = PDU_SESSION_STATUS_ESTABLISHED;
    } else if (session->status != PDU_SESSION_STATUS_ESTABLISHED) {
      session->status = PDU_SESSION_STATUS_FAILED;
      pdusession_failed_t *fail = &resp->pdusessions_failed[pdu_sessions_failed];
      fail->pdusession_id = session->param.pdusession_id;
      fail->cause.type = NGAP_CAUSE_RADIO_NETWORK;
      fail->cause.value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_PDU_SESSION_ID;
      pdu_sessions_failed++;
    }
    resp->nb_of_pdusessions = pdu_sessions_done;
    resp->nb_of_pdusessions_failed = pdu_sessions_failed;
  }

  if ((pdu_sessions_done > 0 || pdu_sessions_failed)) {
    LOG_I(NR_RRC, "NGAP_PDUSESSION_SETUP_RESP: sending the message\n");
    itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
  }

  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
    session->xid = -1;
  }

  return;
}

/**
 * @brief Fill PDU Session Resource Setup Response with a list of PDU Session Resources Failed to Setup
 *        and send ITTI message to TASK_NGAP
 */
static void send_ngap_pdu_session_setup_resp_fail(instance_t instance, ngap_pdusession_setup_req_t *msg, ngap_cause_t cause)
{
  MessageDef *msg_resp = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_PDUSESSION_SETUP_RESP);
  ngap_pdusession_setup_resp_t *resp = &NGAP_PDUSESSION_SETUP_RESP(msg_resp);
  resp->gNB_ue_ngap_id = msg->gNB_ue_ngap_id;
  resp->nb_of_pdusessions_failed = msg->nb_pdusessions_tosetup;
  resp->nb_of_pdusessions = 0;
  for (int i = 0; i < resp->nb_of_pdusessions_failed; ++i) {
    fill_pdu_session_resource_failed_to_setup_item(&resp->pdusessions_failed[i], msg->pdusession[i].pdusession_id, cause);
  }
  itti_send_msg_to_task(TASK_NGAP, instance, msg_resp);
}

void rrc_gNB_process_NGAP_PDUSESSION_SETUP_REQ(MessageDef *msg_p, instance_t instance)
{
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  ngap_pdusession_setup_req_t* msg=&NGAP_PDUSESSION_SETUP_REQ(msg_p);
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, msg->gNB_ue_ngap_id);
  // Reject PDU Session Resource setup if no UE context is found
  if (ue_context_p == NULL) {
    LOG_W(NR_RRC,
          "[gNB %ld] In NGAP_PDUSESSION_SETUP_REQ: no UE context found from UE NGAP ID (%u)\n",
          instance,
          msg->gNB_ue_ngap_id);
    ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_LOCAL_UE_NGAP_ID};
    send_ngap_pdu_session_setup_resp_fail(instance, msg, cause);
    return;
  }

  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  LOG_I(NR_RRC, "UE %d: received PDU Session Resource Setup Request\n", UE->rrc_ue_id);

  // Reject PDU Session Resource setup if gNB_ue_ngap_id is not matching
  if (UE->rrc_ue_id != msg->gNB_ue_ngap_id) {
    LOG_W(NR_RRC, "[gNB %ld] In NGAP_PDUSESSION_SETUP_REQ: unknown UE NGAP ID (%u)\n", instance, msg->gNB_ue_ngap_id);
    ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_LOCAL_UE_NGAP_ID};
    send_ngap_pdu_session_setup_resp_fail(instance, msg, cause);
    rrc_forward_ue_nas_message(rrc, UE);
    return;
  }

  // Reject PDU Session Resource setup if there is no security context active
  if (!UE->as_security_active) {
    LOG_E(NR_RRC, "UE %d: no security context active for UE, rejecting PDU Session Resource Setup Request\n", UE->rrc_ue_id);
    ngap_cause_t cause = {.type = NGAP_CAUSE_PROTOCOL, .value = NGAP_CAUSE_PROTOCOL_MSG_NOT_COMPATIBLE_WITH_RECEIVER_STATE};
    send_ngap_pdu_session_setup_resp_fail(instance, msg, cause);
    rrc_forward_ue_nas_message(rrc, UE);
    return;
  }

  // Reject PDU session if at least one exists already with that ID.
  // At least one because marking one as existing, and setting up another, that
  // might be more work than is worth it. See 8.2.1.4 in 38.413
  for (int i = 0; i < msg->nb_pdusessions_tosetup; ++i) {
    const pdusession_resource_item_t *p = &msg->pdusession[i];
    rrc_pdu_session_param_t *exist = find_pduSession(&UE->pduSessions, p->pdusession_id);
    if (exist) {
      LOG_E(NR_RRC, "UE %d: already has existing PDU session %d rejecting PDU Session Resource Setup Request\n", UE->rrc_ue_id, p->pdusession_id);
      ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_MULTIPLE_PDU_SESSION_ID_INSTANCES};
      send_ngap_pdu_session_setup_resp_fail(instance, msg, cause);
      rrc_forward_ue_nas_message(rrc, UE);
      return;
    }
  }

  UE->amf_ue_ngap_id = msg->amf_ue_ngap_id;


  pdusession_t to_setup[NGAP_MAX_PDU_SESSION] = {0};
  for (int i = 0; i < msg->nb_pdusessions_tosetup; ++i)
    cp_pdusession_resource_item_to_pdusession(&to_setup[i], &msg->pdusession[i]);

  uint64_t dl_ambr = msg->has_ue_ambr ? msg->ueAggMaxBitRate.br_dl : 0;

  if (!trigger_bearer_setup(rrc, UE, msg->nb_pdusessions_tosetup, to_setup, dl_ambr)) {
    // Reject PDU Session Resource setup if there's no CU-UP associated
    LOG_W(NR_RRC, "UE %d: reject PDU Session Setup in PDU Session Resource Setup Response\n", UE->rrc_ue_id);
    ngap_cause_t cause = {.type = NGAP_CAUSE_RADIO_NETWORK, .value = NGAP_CAUSE_RADIO_NETWORK_RESOURCES_NOT_AVAILABLE_FOR_THE_SLICE};
    send_ngap_pdu_session_setup_resp_fail(instance, msg, cause);
    rrc_forward_ue_nas_message(rrc, UE);
  } else {
    // Set ongoing_transaction flag to true
    init_delayed_action(&UE->delayed_action);
  }
}

/** @brief Update existing QoS Flow mapped in the UE context */
static void nr_rrc_update_qos(seq_arr_t *list, const int nb_qos, const pdusession_level_qos_parameter_t *in_qos)
{
  DevAssert(nb_qos == 1);
  DevAssert(nb_qos < MAX_QOS_FLOWS);
  for (uint8_t i = 0; i < nb_qos; ++i) {
    nr_rrc_qos_t *qos = find_qos(list, in_qos[i].qfi);
    if (qos) {
      AssertFatal(qos->qos.qfi == in_qos[i].qfi, "QoS Flow to modify must match existing one");
      LOG_I(NR_RRC, "Updating QoS for QFI=%d\n", in_qos[i].qfi);
      qos->qos = in_qos[i];
    } else {
      LOG_E(NR_RRC, "Failed to update QoS for QFI=%d: QoS flow not found\n", in_qos[i].qfi);
    }
  }
}

/** @brief Update stored pdusession_t in list from NGAP PDU Session Resource item */
static void nr_rrc_update_pdusession(pdusession_t *dst, const pdusession_resource_item_t *src)
{
  dst->pdusession_id = src->pdusession_id;
  dst->nas_pdu = src->nas_pdu;
  dst->pdu_session_type = src->pdusessionTransfer.pdu_session_type;
  dst->n3_incoming = src->pdusessionTransfer.n3_incoming;
  dst->nssai = src->nssai;
  DevAssert(dst->qos.data); // QoS list has to be initialized at this stage
  // Update QoS flow
  nr_rrc_update_qos(&dst->qos, src->pdusessionTransfer.nb_qos, src->pdusessionTransfer.qos);
}

//------------------------------------------------------------------------------
int rrc_gNB_process_NGAP_PDUSESSION_MODIFY_REQ(MessageDef *msg_p, instance_t instance)
//------------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t *ue_context_p = NULL;

  ngap_pdusession_modify_req_t *req = &NGAP_PDUSESSION_MODIFY_REQ(msg_p);

  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  ue_context_p = rrc_gNB_get_ue_context(rrc, req->gNB_ue_ngap_id);
  if (ue_context_p == NULL) {
    LOG_W(NR_RRC, "[gNB %ld] In NGAP_PDUSESSION_MODIFY_REQ: unknown UE from NGAP ids (%u)\n", instance, req->gNB_ue_ngap_id);
    // TO implement return setup failed
    return (-1);
  }
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  bool all_failed = true;
  for (int i = 0; i < req->nb_pdusessions_tomodify; i++) {
    const pdusession_resource_item_t *sessMod = &req->pdusession[i];
    rrc_pdu_session_param_t *session = find_pduSession(&UE->pduSessions, sessMod->pdusession_id);
    if (!session) {
      LOG_W(NR_RRC, "Requested modification of non-existing PDU session, refusing modification\n");
      rrc_pdu_session_param_t to_add = {0};
      to_add.status = PDU_SESSION_STATUS_FAILED;
      to_add.param.pdusession_id = sessMod->pdusession_id;
      to_add.cause.type = NGAP_CAUSE_RADIO_NETWORK;
      to_add.cause.value = NGAP_CAUSE_RADIO_NETWORK_UNKNOWN_PDU_SESSION_ID;
      seq_arr_push_back(&UE->pduSessions, &to_add, sizeof(to_add));
    } else {
      all_failed = false;
      session->status = PDU_SESSION_STATUS_NEW;
      session->cause.type = NGAP_CAUSE_NOTHING;
      nr_rrc_update_pdusession(&session->param, sessMod);
    }
  }

  if (!all_failed) {
    rrc_gNB_modify_dedicatedRRCReconfiguration(rrc, UE);
  } else {
    LOG_I(NR_RRC,
          "pdu session modify failed, fill NGAP_PDUSESSION_MODIFY_RESP with the pdu session information that failed to modify \n");
    MessageDef *msg_fail_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_PDUSESSION_MODIFY_RESP);
    if (msg_fail_p == NULL) {
      LOG_E(NR_RRC, "itti_alloc_new_message failed, msg_fail_p is NULL \n");
      return (-1);
    }
    ngap_pdusession_modify_resp_t *msg = &NGAP_PDUSESSION_MODIFY_RESP(msg_fail_p);
    msg->gNB_ue_ngap_id = req->gNB_ue_ngap_id;
    msg->nb_of_pdusessions = 0;

    FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
      if (session->status == PDU_SESSION_STATUS_FAILED) {
        msg->pdusessions_failed[msg->nb_of_pdusessions_failed].pdusession_id = session->param.pdusession_id;
        msg->pdusessions_failed[msg->nb_of_pdusessions_failed].cause.type = session->cause.type;
        msg->pdusessions_failed[msg->nb_of_pdusessions_failed].cause.value = session->cause.value;
        msg->nb_of_pdusessions_failed++;
      }
    }
    itti_send_msg_to_task(TASK_NGAP, instance, msg_fail_p);
  }
  return (0);
}

int rrc_gNB_send_NGAP_PDUSESSION_MODIFY_RESP(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, uint8_t xid)
{
  MessageDef *msg_p = NULL;
  uint16_t pdu_sessions_failed = 0;
  uint16_t pdu_sessions_done = 0;

  if (!seq_arr_size(&UE->pduSessions)) {
    LOG_W(NR_RRC, "UE %d: No PDU sessions in the list, don't send NG PDU Session Modify Response\n", UE->rrc_ue_id);
    return -1;
  }

  msg_p = itti_alloc_new_message (TASK_RRC_GNB, rrc->module_id, NGAP_PDUSESSION_MODIFY_RESP);
  if (msg_p == NULL) {
    LOG_E(NR_RRC, "itti_alloc_new_message failed, msg_p is NULL \n");
    return (-1);
  }
  ngap_pdusession_modify_resp_t *resp = &NGAP_PDUSESSION_MODIFY_RESP(msg_p);
  LOG_I(NR_RRC, "send message NGAP_PDUSESSION_MODIFY_RESP \n");

  resp->gNB_ue_ngap_id = UE->rrc_ue_id;

  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
    if (xid != session->xid) {
      LOG_W(NR_RRC,
            "xid does not correspond (PDU Session %d, status %d, xid %d/%d) \n ",
            session->param.pdusession_id,
            session->status,
            xid,
            session->xid);
      continue;
    }
    if (session->status == PDU_SESSION_STATUS_DONE) {
      LOG_I(NR_RRC, "Successfully modified PDU Session %d \n", session->param.pdusession_id);
      // Update status
      session->status = PDU_SESSION_STATUS_ESTABLISHED;
      session->cause.type = NGAP_CAUSE_NOTHING;
      // Fill response
      DevAssert(pdu_sessions_done < NGAP_MAX_PDU_SESSION);
      pdusession_modify_t *p = &resp->pdusessions[pdu_sessions_done++];
      p->pdusession_id = session->param.pdusession_id;
      FOR_EACH_SEQ_ARR(nr_rrc_qos_t *, qos_session, &session->param.qos) {
        DevAssert(p->nb_of_qos_flow < MAX_QOS_FLOWS);
        qos_flow_tobe_modified_t *q = &p->qos[p->nb_of_qos_flow++];
        q->qfi = qos_session->qos.qfi;
      }
      LOG_I(NR_RRC,
            "Modify Resp (msg index %d, status %d, xid %d): nb_of_pduSessions %ld, pdusession_id %d \n ",
            pdu_sessions_done,
            session->status,
            xid,
            seq_arr_size(&UE->pduSessions),
            p->pdusession_id);
    } else if ((session->status == PDU_SESSION_STATUS_NEW) || (session->status == PDU_SESSION_STATUS_ESTABLISHED)) {
      LOG_D(NR_RRC, "PDU SESSION is NEW or already ESTABLISHED\n");
    } else if (session->status == PDU_SESSION_STATUS_FAILED) {
      DevAssert(pdu_sessions_failed < NGAP_MAX_PDU_SESSION);
      resp->pdusessions_failed[pdu_sessions_failed].pdusession_id = session->param.pdusession_id;
      resp->pdusessions_failed[pdu_sessions_failed].cause.type = session->cause.type;
      resp->pdusessions_failed[pdu_sessions_failed].cause.value = session->cause.value;
      pdu_sessions_failed++;
    } else
      LOG_W(NR_RRC, "Modify pdu session %d, unknown state %d \n ", session->param.pdusession_id, session->status);
  }

  resp->nb_of_pdusessions = pdu_sessions_done;
  resp->nb_of_pdusessions_failed = pdu_sessions_failed;

  if (pdu_sessions_done > 0 || pdu_sessions_failed > 0) {
    LOG_D(NR_RRC, "NGAP_PDUSESSION_MODIFY_RESP: sending the message (total pdu session %ld)\n", seq_arr_size(&UE->pduSessions));
    itti_send_msg_to_task (TASK_NGAP, rrc->module_id, msg_p);
  } else {
    itti_free (ITTI_MSG_ORIGIN_ID(msg_p), msg_p);
  }

  return 0;
}

/** @brief Send UE Context Release Request (NG-RAN node initiated)
 * Direction: NG-RAN node -> AMF (8.3.2.2 3GPP TS 38.413) */
void rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_REQ(const module_id_t gnb_mod_idP,
                                              const rrc_gNB_ue_context_t *const ue_context_pP,
                                              const ngap_cause_t causeP)
//------------------------------------------------------------------------------
{
  if (ue_context_pP == NULL) {
    LOG_E(RRC, "[gNB] In NGAP_UE_CONTEXT_RELEASE_REQ: invalid UE\n");
  } else {
    const gNB_RRC_UE_t *UE = &ue_context_pP->ue_context;
    MessageDef *msg = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_UE_CONTEXT_RELEASE_REQ);
    ngap_ue_release_req_t *req = &NGAP_UE_CONTEXT_RELEASE_REQ(msg);
    memset(req, 0, sizeof(*req));
    req->gNB_ue_ngap_id = UE->rrc_ue_id;
    req->cause.type = causeP.type;
    req->cause.value = causeP.value;
    // PDU Session Resource List (optional)
    FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
      DevAssert(req->nb_of_pdusessions < NGAP_MAX_PDU_SESSION);
      req->pdusession_ids[req->nb_of_pdusessions] = session->param.pdusession_id;
      req->nb_of_pdusessions++;
    }
    itti_send_msg_to_task(TASK_NGAP, GNB_MODULE_ID_TO_INSTANCE(gnb_mod_idP), msg);
  }
}

/** @brief Sends the NG Handover Failure from the Target NG-RAN to the AMF */
void rrc_gNB_send_NGAP_HANDOVER_FAILURE(gNB_RRC_INST *rrc, ngap_handover_failure_t *msg)
{
  LOG_I(NR_RRC, "Send NG Handover Failure message (amf_ue_ngap_id %ld) with cause %d \n ", msg->amf_ue_ngap_id, msg->cause.value);
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_HANDOVER_FAILURE);
  NGAP_HANDOVER_FAILURE(msg_p) = *msg;
  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
}

/** @brief Process NG Handover Request message (8.4.2.2 3GPP TS 38.413) */
int rrc_gNB_process_Handover_Request(gNB_RRC_INST *rrc, instance_t instance, ngap_handover_request_t *msg)
{
  // Check if UE context already exists for this AMF UE NGAP ID
  rrc_gNB_ue_context_t *existing_ue_context = rrc_gNB_get_ue_context_by_amf_ue_ngap_id(rrc, msg->amf_ue_ngap_id);
  if (existing_ue_context != NULL) {
    LOG_E(RRC, "UE context already exists for AMF UE NGAP ID %ld, cannot process handover request\n", msg->amf_ue_ngap_id);
    ngap_handover_failure_t fail = {
        .amf_ue_ngap_id = msg->amf_ue_ngap_id,
        .cause.type = NGAP_CAUSE_RADIO_NETWORK,
        .cause.value = NGAP_CAUSE_RADIO_NETWORK_HO_FAILURE_IN_TARGET_5GC_NGRAN_NODE_OR_TARGET_SYSTEM,
    };
    rrc_gNB_send_NGAP_HANDOVER_FAILURE(rrc, &fail);
    return -1;
  }

  struct nr_rrc_du_container_t *du = get_du_by_cell_id(rrc, msg->nr_cell_id);
  if (du == NULL) {
    /* Cell Not Found! Return HO Request Failure*/
    LOG_E(RRC, "Failed to process Handover Request: no DU found with NR Cell ID=%lu \n", msg->nr_cell_id);
    ngap_handover_failure_t fail = {
        .amf_ue_ngap_id = msg->amf_ue_ngap_id,
        .cause.type = NGAP_CAUSE_RADIO_NETWORK,
        .cause.value = NGAP_CAUSE_RADIO_NETWORK_RADIO_RESOURCES_NOT_AVAILABLE,
    };
    rrc_gNB_send_NGAP_HANDOVER_FAILURE(rrc, &fail);
    return -1;
  }

  // Validate PLMN from GUAMI against allowed PLMN list
  const plmn_id_t *serving_plmn = get_serving_plmn(rrc, &msg->guami.plmn);
  if (!serving_plmn) {
    LOG_E(NR_RRC, "PLMN from GUAMI not supported - rejecting handover\n");
    ngap_handover_failure_t fail = {
        .amf_ue_ngap_id = msg->amf_ue_ngap_id,
        .cause.type = NGAP_CAUSE_RADIO_NETWORK,
        .cause.value = NGAP_CAUSE_RADIO_NETWORK_HO_TARGET_NOT_ALLOWED,
    };
    rrc_gNB_send_NGAP_HANDOVER_FAILURE(rrc, &fail);
    return -1;
  }

  uint16_t pci = du->setup_req->cell[0].info.nr_pci;
  LOG_I(NR_RRC, "Received Handover Request (on NR Cell ID=%lu, PCI=%u) \n", msg->nr_cell_id, pci);

  // Create UE context
  sctp_assoc_t curr_assoc_id = du->assoc_id;
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_create_ue_context(curr_assoc_id, UINT16_MAX, rrc, UINT64_MAX, UINT32_MAX);
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  // allocate context for target
  UE->ho_context = alloc_ho_ctx(HO_CTX_TARGET);
  UE->ho_context->target->ho_trigger = nr_rrc_trigger_n2_ho_target;

  // Store IDs in UE context
  UE->amf_ue_ngap_id = msg->amf_ue_ngap_id;
  UE->ue_guami = msg->guami;
  // Store the serving PLMN
  UE->serving_plmn = *serving_plmn;

  UE->ho_context->target->ue_ho_prep_info = copy_byte_array(msg->ue_ho_prep_info);
  // store the received UE Security Capabilities in the UE context
  FREE_AND_ZERO_BYTE_ARRAY(UE->ue_cap_buffer);
  UE->ue_cap_buffer = copy_byte_array(msg->ue_cap);

  /* store the received Security Context in the UE context
     and take it into use as defined in TS 33.501 */
  set_UE_security_algos(rrc, UE, &msg->security_capabilities);
  UE->nh_ncc = msg->security_context.next_hop_chain_count;
  memcpy(UE->nh, msg->security_context.next_hop, SECURITY_KEY_LENGTH);
  // Reset KgNB
  memset(UE->kgnb, 0, SECURITY_KEY_LENGTH);
  // Derive KgNB*
  const f1ap_served_cell_info_t *cell_info = &du->setup_req->cell[0].info;
  uint32_t ssb_arfcn = get_ssb_arfcn(du);
  nr_derive_key_ng_ran_star(cell_info->nr_pci, ssb_arfcn, UE->nh, UE->kgnb);
  UE->as_security_active = true;
  // Activate SRBs
  activate_srb(UE, SRB1);
  activate_srb(UE, SRB2);
  // During N2 handover, the UE continues using the old security context from the source gNB
  // until it receives and processes the RRC Reconfiguration with masterKeyUpdate. The first
  // PDUs sent after CFRA are still ciphered with the old keys. The target gNB must enable
  // ciphering during handover setup to correctly decipher and verify integrity of these PDUs.
  nr_rrc_pdcp_config_security(UE, true);

  // Process all PDU Session Resource Setup items from handover request
  DevAssert(msg->nb_of_pdusessions <= NGAP_MAX_PDU_SESSION);
  pdusession_t to_setup[NGAP_MAX_PDU_SESSION];
  for (int i = 0; i < msg->nb_of_pdusessions; i++) {
    ho_request_pdusession_t *ho_pdu = &msg->pduSessionResourceSetupList[i];
    pdusession_t *pdu = &to_setup[i];
    pdu->nssai = ho_pdu->nssai;
    pdu->pdu_session_type = ho_pdu->pdu_session_type;
    pdu->pdusession_id = ho_pdu->pdusession_id;
    cp_pdusession_transfer_to_pdusession(pdu, &ho_pdu->pdusessionTransfer);
  }

  if (!trigger_bearer_setup(rrc, UE, msg->nb_of_pdusessions, to_setup, msg->ue_ambr.br_dl)) {
    LOG_E(NR_RRC, "Failed to establish PDU session: handover failed\n");

    ngap_handover_failure_t fail = {
        .amf_ue_ngap_id = msg->amf_ue_ngap_id,
        .cause.type = NGAP_CAUSE_RADIO_NETWORK,
        .cause.value = NGAP_CAUSE_RADIO_NETWORK_HO_FAILURE_IN_TARGET_5GC_NGRAN_NODE_OR_TARGET_SYSTEM,
    };
    rrc_gNB_send_NGAP_HANDOVER_FAILURE(rrc, &fail);
    rrc_remove_ue(rrc, ue_context_p);
    return -1;
  }

  return 0;
}

void rrc_gNB_free_Handover_Request(ngap_handover_request_t *msg)
{
  free_byte_array(msg->ue_cap);
  free_byte_array(msg->ue_ho_prep_info);
  free(msg->mobility_restriction);
}

/** @brief Send NG Uplink RAN Status Transfer message (8.4.6 3GPP TS 38.413)
 * Direction: source NG-RAN node -> AMF */
int rrc_gNB_send_NGAP_ul_ran_status_transfer(gNB_RRC_INST *rrc,
                                             gNB_RRC_UE_t *UE,
                                             const int n_to_mod,
                                             const int *drb_ids,
                                             const e1_pdcp_status_info_t *pdcp_status)
{
  AssertFatal(UE != NULL, "UE context is NULL\n");
  DevAssert(n_to_mod <= MAX_DRBS_PER_UE);
  DevAssert(drb_ids);

  LOG_I(NR_RRC,
        "Sending NGAP Uplink RAN Status Transfer (AMF_UE_NGAP_ID=%" PRIu64 ", GNB_UE_NGAP_ID=%u)\n",
        UE->amf_ue_ngap_id,
        UE->rrc_ue_id);

  ngap_ran_status_transfer_t msg = {
      .amf_ue_ngap_id = UE->amf_ue_ngap_id,
      .gnb_ue_ngap_id = UE->rrc_ue_id,
  };

  // Loop through DRBs and extract COUNT values
  for (int i = 0; i < n_to_mod; ++i) {
    int drb_id = drb_ids[i];

    // Find the DRB in the UE's DRB list
    drb_t *drb = get_drb(&UE->drbs, drb_id);
    if (!drb) {
      LOG_E(NR_RRC, "Failed to send UL RAN Status Transfer: DRB %d not found\n", drb_id);
      continue;
    }

    bool sn_length_18 = drb->pdcp_config.drb.sn_size == 18;

    DevAssert(msg.ran_status.nb_drb < MAX_DRBS_PER_UE);
    ngap_drb_status_t *item = &msg.ran_status.drb_status_list[msg.ran_status.nb_drb++];
    item->drb_id = drb_id;

    const e1_pdcp_count_t *ul_pdcp = &pdcp_status[i].ul_count;
    const e1_pdcp_count_t *dl_pdcp = &pdcp_status[i].dl_count;

    item->ul_count.pdcp_sn = ul_pdcp->sn;
    item->ul_count.hfn = ul_pdcp->hfn;
    item->ul_count.sn_len = sn_length_18 ? NGAP_SN_LENGTH_18 : NGAP_SN_LENGTH_12;

    item->dl_count.pdcp_sn = dl_pdcp->sn;
    item->dl_count.hfn = dl_pdcp->hfn;
    item->dl_count.sn_len = sn_length_18 ? NGAP_SN_LENGTH_18 : NGAP_SN_LENGTH_12;
  }

  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_UL_RAN_STATUS_TRANSFER);
  NGAP_UL_RAN_STATUS_TRANSFER(msg_p) = msg;
  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);

  return 0;
}

/** @brief Process NG Handover Command on Source gNB */
void rrc_gNB_process_HandoverCommand(gNB_RRC_INST *rrc, const ngap_handover_command_t *msg)
{
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, msg->gNB_ue_ngap_id);

  if (ue_context_p == NULL) {
    LOG_W(NR_RRC, "Unknown UE context associated to gNB_ue_ngap_id (%u)\n", msg->gNB_ue_ngap_id);
    return;
  }
  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;

  byte_array_t buffer = doRRCReconfiguration_from_HandoverCommand(msg->handoverCommand);
  if (!buffer.buf || buffer.len == 0) {
    LOG_E(NR_RRC, "Failed to decode/encode RRCReconfiguration from HandoverCommand\n");
    // Notify AMF that handover was cancelled
    DevAssert(UE->ho_context);
    DevAssert(UE->ho_context->source);
    DevAssert(UE->ho_context->source->ho_cancel);
    UE->ho_context->source->ho_cancel(rrc, UE);
    return;
  }

  rrc_gNB_trigger_reconfiguration_for_handover(rrc, UE, buffer.buf, buffer.len);
  LOG_A(NR_RRC, "Send reconfiguration (HO Command) to UE %u/RNTI %04x\n", UE->rrc_ue_id, UE->rnti);
  free_byte_array(buffer);
}

void rrc_gNB_free_Handover_Command(ngap_handover_command_t *msg)
{
  free_byte_array(msg->handoverCommand);
}

/*
* Process the NG command NGAP_UE_CONTEXT_RELEASE_COMMAND, sent by AMF.
* The gNB should remove all pdu session, NG context, and other context of the UE.
*/
int rrc_gNB_process_NGAP_UE_CONTEXT_RELEASE_COMMAND(MessageDef *msg_p, instance_t instance)
{
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  uint32_t gNB_ue_ngap_id = NGAP_UE_CONTEXT_RELEASE_COMMAND(msg_p).gNB_ue_ngap_id;
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(RC.nrrrc[instance], gNB_ue_ngap_id);

  if (ue_context_p == NULL) {
    /* Can not associate this message to an UE index */
    LOG_W(NR_RRC, "[gNB %ld] In NGAP_UE_CONTEXT_RELEASE_COMMAND: unknown UE from gNB_ue_ngap_id (%u)\n",
          instance,
          gNB_ue_ngap_id);
    rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_COMPLETE(instance, gNB_ue_ngap_id, NULL);
    return -1;
  }

  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  UE->an_release = true;
#ifdef E2_AGENT
  signal_rrc_state_changed_to(UE, RRC_IDLE_RRC_STATE_E2SM_RC);
#endif

  /* a UE might not be associated to a CU-UP if it never requested a PDU
   * session (intentionally, or because of erros) */
  if (ue_associated_to_cuup(rrc, UE)) {
    sctp_assoc_t assoc_id = get_existing_cuup_for_ue(rrc, UE);
    e1ap_cause_t cause = {.type = E1AP_CAUSE_RADIO_NETWORK, .value = E1AP_RADIO_CAUSE_NORMAL_RELEASE};
    e1ap_bearer_release_cmd_t cmd = {
      .gNB_cu_cp_ue_id = UE->rrc_ue_id,
      .gNB_cu_up_ue_id = UE->rrc_ue_id,
      .cause = cause,
    };
    rrc->cucp_cuup.bearer_context_release(assoc_id, &cmd);
  }

  /* special case: the DU might be offline, in which case the f1_ue_data exists
   * but is set to 0 */
  if (cu_exists_f1_ue_data(UE->rrc_ue_id) && cu_get_f1_ue_data(UE->rrc_ue_id).du_assoc_id != 0) {
    if (UE->rl_failure) {
      /* Radio link is dead (DU reported rl-failure): send F1AP UE Context
       * Release Command without RRC-Container per TS 38.473 section 8.3.3.
       * The DU will release resources locally without signaling the UE. */
      LOG_I(NR_RRC, "UE %u: radio link failure, sending release command without RRC Release\n", UE->rrc_ue_id);
      f1_ue_data_t ue_data = cu_get_f1_ue_data(UE->rrc_ue_id);
      f1ap_ue_context_rel_cmd_t cmd = {
        .gNB_CU_ue_id = UE->rrc_ue_id,
        .gNB_DU_ue_id = ue_data.secondary_ue,
        .cause = F1AP_CAUSE_RADIO_NETWORK,
        .cause_value = F1AP_CauseRadioNetwork_rl_failure_rlc,
      };
      rrc->mac_rrc.ue_context_release_command(ue_data.du_assoc_id, &cmd);
    } else {
      rrc_gNB_generate_RRCRelease(rrc, UE);
    }

    /* UE will be freed after UE context release complete */
  } else {
    // the DU is offline already
    rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_COMPLETE(0, UE->rrc_ue_id, &UE->pduSessions);
    rrc_remove_ue(rrc, ue_context_p);
  }

  return 0;
}

void rrc_gNB_send_NGAP_UE_CONTEXT_RELEASE_COMPLETE(instance_t instance, uint32_t gNB_ue_ngap_id, seq_arr_t *pdusessions)
{
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_UE_CONTEXT_RELEASE_COMPLETE);
  ngap_ue_release_complete_t *complete = &NGAP_UE_CONTEXT_RELEASE_COMPLETE(msg);
  complete->gNB_ue_ngap_id = gNB_ue_ngap_id;
  // PDU Session Resource List (Optional)
  if (pdusessions && seq_arr_size(pdusessions)) {
    FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, pdusessions) {
      complete->pdu_session_id[complete->num_pdu_sessions++] = session->param.pdusession_id;
    }
  }
  itti_send_msg_to_task(TASK_NGAP, instance, msg);
}

void rrc_gNB_send_NGAP_UE_CAPABILITIES_IND(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, const NR_UECapabilityInformation_t *const ue_cap_info)
//------------------------------------------------------------------------------
{
  NR_UE_CapabilityRAT_ContainerList_t *ueCapabilityRATContainerList =
      ue_cap_info->criticalExtensions.choice.ueCapabilityInformation->ue_CapabilityRAT_ContainerList;
  void *buf;
  NR_UERadioAccessCapabilityInformation_t rac = {0};

  if (ueCapabilityRATContainerList->list.count == 0) {
    LOG_W(RRC, "[UE %d] bad UE capabilities\n", UE->rrc_ue_id);
    }

    int ret = uper_encode_to_new_buffer(&asn_DEF_NR_UE_CapabilityRAT_ContainerList, NULL, ueCapabilityRATContainerList, &buf);
    AssertFatal(ret > 0, "fail to encode ue capabilities\n");

    rac.criticalExtensions.present = NR_UERadioAccessCapabilityInformation__criticalExtensions_PR_c1;
    asn1cCalloc(rac.criticalExtensions.choice.c1, c1);
    c1->present = NR_UERadioAccessCapabilityInformation__criticalExtensions__c1_PR_ueRadioAccessCapabilityInformation;
    asn1cCalloc(c1->choice.ueRadioAccessCapabilityInformation, info);
    info->ue_RadioAccessCapabilityInfo.buf = buf;
    info->ue_RadioAccessCapabilityInfo.size = ret;
    info->nonCriticalExtension = NULL;
    /* 8192 is arbitrary, should be big enough */
    void *buf2 = NULL;
    int encoded = uper_encode_to_new_buffer(&asn_DEF_NR_UERadioAccessCapabilityInformation, NULL, &rac, &buf2);

    AssertFatal(encoded > 0, "fail to encode ue capabilities\n");
    ;
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NR_UERadioAccessCapabilityInformation, &rac);
    MessageDef *msg_p;
    msg_p = itti_alloc_new_message (TASK_RRC_GNB, rrc->module_id, NGAP_UE_CAPABILITIES_IND);
    ngap_ue_cap_info_ind_t *ind = &NGAP_UE_CAPABILITIES_IND(msg_p);
    memset(ind, 0, sizeof(*ind));
    ind->gNB_ue_ngap_id = UE->rrc_ue_id;
    ind->ue_radio_cap.len = encoded;
    ind->ue_radio_cap.buf = buf2;
    itti_send_msg_to_task (TASK_NGAP, rrc->module_id, msg_p);
    LOG_I(NR_RRC,"Send message to ngap: NGAP_UE_CAPABILITIES_IND\n");
}

void rrc_gNB_send_NGAP_HANDOVER_REQUEST_ACKNOWLEDGE(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, byte_array_t ho_command)
{
  LOG_D(NR_RRC, "Sending Handover Request Acknowledge\n");

  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_HANDOVER_REQUEST_ACKNOWLEDGE);
  ngap_handover_request_ack_t *msg = &NGAP_HANDOVER_REQUEST_ACKNOWLEDGE(msg_p);
  memset(msg, 0, sizeof(*msg));

  // RAN UE NGAP ID
  msg->gNB_ue_ngap_id = UE->rrc_ue_id;
  // AMF UE NGAP ID
  msg->amf_ue_ngap_id = UE->amf_ue_ngap_id;
  // PDU Session Resource Admitted List
  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t*, session, &UE->pduSessions) {
    session->status = PDU_SESSION_STATUS_ESTABLISHED;
    // PDU Session ID
    DevAssert(msg->nb_of_pdusessions < NGAP_MAX_PDU_SESSION);
    pdu_session_resource_admitted_t *pdu = &msg->pdusessions[msg->nb_of_pdusessions++];
    pdu->pdu_session_id = session->param.pdusession_id;
    // Handover Request Acknowledge Transfer
    ho_request_ack_transfer_t *transfer = &pdu->ack_transfer;
    transfer->gtp_teid = session->param.n3_outgoing.teid;
    memcpy(transfer->gNB_addr.buffer, session->param.n3_outgoing.addr.buffer, session->param.n3_outgoing.addr.length);
    transfer->gNB_addr.length = session->param.n3_outgoing.addr.length;
    FOR_EACH_SEQ_ARR(nr_rrc_qos_t *, qos, &session->param.qos) {
      DevAssert(transfer->nb_of_qos_flow < MAX_QOS_FLOWS);
      pdusession_associate_qosflow_t *qos_setup = &transfer->qos_setup_list[transfer->nb_of_qos_flow++];
      qos_setup->qfi = qos->qos.qfi;
      qos_setup->qos_flow_mapping_ind = QOSFLOW_MAPPING_INDICATION_DL;
    }
  }
  // Target to Source Transparent Container
  msg->target2source = copy_byte_array(ho_command);

  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
}

/** @brief Prepare NG Handover Notify message and inform NGAP */
void rrc_gNB_send_NGAP_HANDOVER_NOTIFY(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE)
{
  LOG_I(NR_RRC, "Triggering NGAP Handover Notify\n");
  nr_rrc_du_container_t *du = get_du_by_cell_id(rrc, rrc->nr_cellid);
  if (du == NULL) {
    LOG_E(NR_RRC, "Failed to send Handover Notify: no DU found with NR Cell ID=%lu\n", rrc->nr_cellid);
    return;
  }

  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_HANDOVER_NOTIFY);
  ngap_handover_notify_t *ho_notify = &NGAP_HANDOVER_NOTIFY(msg_p);
  memset(ho_notify, 0, sizeof(*ho_notify));

  ho_notify->gNB_ue_ngap_id = UE->rrc_ue_id;
  ho_notify->amf_ue_ngap_id = UE->amf_ue_ngap_id;
  ho_notify->user_info.nrCellIdentity = rrc->nr_cellid;

  target_ran_node_id_t *target_ng_ran = &ho_notify->user_info.target_ng_ran;
  target_ng_ran->tac = *du->setup_req->cell->info.tac;
  target_ng_ran->targetgNBId = rrc->node_id;
  target_ng_ran->plmn_identity = du->setup_req->cell->info.plmn;

  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
}

/** @brief Prepare NG Handover Cancel (source NG-RAN) and inform NGAP (N2-based HO only)
 *  3GPP TS 38.413 §8.4.5 (Handover Cancellation) - source NG-RAN -> AMF
 *  3GPP TS 38.413 §9.2.3.11 (HANDOVER CANCEL)
 *  @param rrc CU/RRC instance
 *  @param UE UE context
 *  @param cause cancellation cause */
void rrc_gNB_send_NGAP_HANDOVER_CANCEL(int module_id, gNB_RRC_UE_t *UE, ngap_cause_t cause)
{
  DevAssert(UE != NULL);

  LOG_I(NR_RRC, "Triggering NGAP Handover Cancel (gNB_ue_ngap_id=%d, amf_ue_ngap_id=%ld)\n", UE->rrc_ue_id, UE->amf_ue_ngap_id);

  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_HANDOVER_CANCEL);
  ngap_handover_cancel_t *ho_cancel = &NGAP_HANDOVER_CANCEL(msg_p);
  memset(ho_cancel, 0, sizeof(*ho_cancel));

  /* Mandatory IEs (38.413 §9.2.3.11) */
  ho_cancel->gNB_ue_ngap_id = UE->rrc_ue_id;
  ho_cancel->amf_ue_ngap_id = UE->amf_ue_ngap_id;
  ho_cancel->cause = cause;

  itti_send_msg_to_task(TASK_NGAP, module_id, msg_p);
}

void rrc_gNB_send_NGAP_PDUSESSION_RELEASE_RESPONSE(gNB_RRC_INST *rrc, gNB_RRC_UE_t *UE, uint8_t xid)
{
  MessageDef   *msg_p;
  msg_p = itti_alloc_new_message (TASK_RRC_GNB, rrc->module_id, NGAP_PDUSESSION_RELEASE_RESPONSE);
  ngap_pdusession_release_resp_t *resp = &NGAP_PDUSESSION_RELEASE_RESPONSE(msg_p);
  memset(resp, 0, sizeof(*resp));
  resp->gNB_ue_ngap_id = UE->rrc_ue_id;

  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t *, session, &UE->pduSessions) {
    if (xid == session->xid) {
      const pdusession_t *pdusession = &session->param;
      if (session->status == PDU_SESSION_STATUS_TORELEASE) {
        DevAssert(resp->nb_of_pdusessions_released < NGAP_MAX_PDU_SESSION);
        resp->pdusession_release[resp->nb_of_pdusessions_released++].pdusession_id = pdusession->pdusession_id;
      }
    }
  }

  for (int i = 0; i < resp->nb_of_pdusessions_released; ++i) {
    rm_pduSession(&UE->pduSessions, &UE->drbs, resp->pdusession_release[i].pdusession_id);
  }

  LOG_I(NR_RRC, "NGAP PDUSESSION RELEASE RESPONSE: rrc_ue_id %u release_pdu_sessions %d\n", resp->gNB_ue_ngap_id, resp->nb_of_pdusessions_released);
  itti_send_msg_to_task (TASK_NGAP, rrc->module_id, msg_p);
}

/** @brief Process NG PDU Session Resource Release command (8.2.2 of 3GPP TS 38.413)
 * upon reception the NG-RAN node shall execute the release of the requested PDU sessions.
 * For each PDU session to be released the NG-RAN node shall release the corresponding
 * resources over Uu and over NG, if any. */
int rrc_gNB_process_NGAP_PDUSESSION_RELEASE_COMMAND(ngap_pdusession_release_command_t *cmd, gNB_RRC_INST *rrc)
{
  uint32_t gNB_ue_ngap_id = cmd->gNB_ue_ngap_id;
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, gNB_ue_ngap_id);

  if (!ue_context_p) {
    LOG_E(NR_RRC, "[gNB %d] UE context not found for gNB_ue_ngap_id %u \n", rrc->module_id, gNB_ue_ngap_id);
    return -1;
  }

  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  LOG_I(NR_RRC, "NG PDU Session Release command: AMF_UE_NGAP_ID=%lu, rrc_ue_id=%u, nb_pdusessions_torelease=%d \n",
        cmd->amf_ue_ngap_id,
        gNB_ue_ngap_id,
        cmd->nb_pdusessions_torelease);
  e1ap_bearer_mod_req_t req = {0};
  for (int pdusession = 0; pdusession < cmd->nb_pdusessions_torelease; pdusession++) {
    rrc_pdu_session_param_t *pduSession = find_pduSession(&UE->pduSessions, cmd->pdusession_ids[pdusession]);
    if (!pduSession) {
      LOG_E(NR_RRC, "Failed to release non-existing PDU Session %d\n", cmd->pdusession_ids[pdusession]);
      continue;
    }
    if (pduSession->status == PDU_SESSION_STATUS_TORELEASE) {
      LOG_W(NR_RRC, "PDU Session %d already set to be released\n", pduSession->param.pdusession_id);
      continue;
    }
    // Set PDU session to release, regardless of the status
    LOG_I(NR_RRC, "Set PDU Session %d to release\n", pduSession->param.pdusession_id);
    pdu_session_to_remove_t *release = &req.pduSessionRem[req.numPDUSessionsRem++];
    release->sessionId = pduSession->param.pdusession_id;
    release->cause.type = E1AP_CAUSE_RADIO_NETWORK;
    release->cause.value = E1AP_RADIO_CAUSE_NORMAL_RELEASE;
    pduSession->status = PDU_SESSION_STATUS_TORELEASE;
  }

  if (req.numPDUSessionsRem == 0) {
    LOG_E(NR_RRC, "Received NG PDU Session Release Command but no PDU Sessions to release\n");
    return -1;
  } else if (!ue_associated_to_cuup(rrc, UE)) {
    LOG_E(NR_RRC, "UE %d is not associated to CU-UP\n", UE->rrc_ue_id);
    // TODO handle, e.g., only trigger F1 release
    return 0;
  } else {
    /* If present, store NAS-PDU in the UE context for later inclusion in RRCReconfiguration */
    if (cmd->nas_pdu.len > 0) {
      UE->nas_pdu = copy_byte_array(cmd->nas_pdu);
    }
    LOG_I(NR_RRC, "Send F1 Bearer Context Modification Request with PDU Session release \n");
    req.gNB_cu_cp_ue_id = UE->rrc_ue_id;
    req.gNB_cu_up_ue_id = UE->rrc_ue_id;
    sctp_assoc_t assoc_id = get_existing_cuup_for_ue(rrc, UE);
    rrc->cucp_cuup.bearer_context_mod(assoc_id, &req);
  }
  init_delayed_action(&UE->delayed_action);
  return 0;
}

int rrc_gNB_process_PAGING_IND(MessageDef *msg_p, instance_t instance)
{
  ngap_paging_ind_t *msg = &NGAP_PAGING_IND(msg_p);
  for (uint16_t tai_size = 0; tai_size < msg->tai_size; tai_size++) {
    plmn_id_t *p = &msg->plmn_identity[tai_size];
    LOG_I(NR_RRC,
          "[gNB %ld] TAI List for Paging: MCC=%03d, MNC=%0*d, TAC=%d\n",
          instance,
          p->mcc,
          p->mnc_digit_length,
          p->mnc,
          msg->tac[tai_size]);
    gNB_RrcConfigurationReq *req = &RC.nrrrc[instance]->configuration;
    for (uint8_t j = 0; j < req->num_plmn; j++) {
      plmn_id_t *req_plmn = &req->plmn[j];
      if (req_plmn->mcc == p->mcc && req_plmn->mnc == p->mnc && req->tac == msg->tac[tai_size]) {
        for (uint8_t CC_id = 0; CC_id < MAX_NUM_CCs; CC_id++) {
          AssertFatal(false, "to be implemented properly\n");
          if (NODE_IS_CU(RC.nrrrc[instance]->node_type)) {
            MessageDef *m = itti_alloc_new_message(TASK_RRC_GNB, 0, F1AP_PAGING_IND);
            F1AP_PAGING_IND(m).plmn = *req_plmn;
            F1AP_PAGING_IND (m).nr_cellid        = RC.nrrrc[j]->nr_cellid;
            F1AP_PAGING_IND(m).ueidentityindexvalue = (uint16_t)(msg->ue_paging_identity.s_tmsi.m_tmsi % 1024);
            F1AP_PAGING_IND(m).fiveg_s_tmsi = msg->ue_paging_identity.s_tmsi.m_tmsi;
            F1AP_PAGING_IND(m).paging_drx = msg->paging_drx;
            LOG_E(F1AP, "ueidentityindexvalue %u fiveg_s_tmsi %ld paging_drx %u\n", F1AP_PAGING_IND (m).ueidentityindexvalue, F1AP_PAGING_IND (m).fiveg_s_tmsi, F1AP_PAGING_IND (m).paging_drx);
            itti_send_msg_to_task(TASK_CU_F1, instance, m);
          } else {
            //rrc_gNB_generate_pcch_msg(NGAP_PAGING_IND(msg_p).ue_paging_identity.s_tmsi.m_tmsi,(uint8_t)NGAP_PAGING_IND(msg_p).paging_drx, instance, CC_id);
          } // end of nodetype check
        } // end of cc loop
      } // end of mcc mnc check
    } // end of num_plmn
  } // end of tai size

  return 0;
}

/** @brief Callback for NGAP Handover Required message (3GPP TS 38.413 9.2.3.1)
 * Direction: source gNB -> AMF */
void rrc_gNB_send_NGAP_HANDOVER_REQUIRED(gNB_RRC_INST *rrc,
                                         gNB_RRC_UE_t *UE,
                                         const nr_neighbour_cell_t *neighbour,
                                         const byte_array_t hoPrepInfo)
{
  LOG_I(NR_RRC, "Handover Preparation: send Handover Required (target gNB ID=%d, PCI=%d)\n", neighbour->gNB_ID, neighbour->physicalCellId);

  const plmn_id_t plmn = neighbour->plmn;
  const target_ran_node_id_t target = {
      .plmn_identity = plmn,
      .tac = neighbour->tac,
      .targetgNBId = neighbour->gNB_ID,
  };

  ngap_handover_required_t msg = {
      .amf_ue_ngap_id = UE->amf_ue_ngap_id,
      .gNB_ue_ngap_id = UE->rrc_ue_id,
      .cause.type = NGAP_CAUSE_RADIO_NETWORK,
      .cause.value = NGAP_CAUSE_RADIO_NETWORK_HANDOVER_DESIRABLE_FOR_RADIO_REASON,
      .handoverType = HANDOVER_TYPE_INTRA5GS,
      .target_gnb_id = target,
  };

  cell_id_t target_cell = {
      .nrCellIdentity = neighbour->nrcell_id,
      .plmn_identity = plmn,
  };

  // Source to Target Transparent Container (M)
  msg.source2target = calloc_or_fail(1, sizeof(*msg.source2target));
  // Target Cell ID (M)
  msg.source2target->targetCellId = target_cell;
  // RRC Container (M)
  msg.source2target->handoverInfo = copy_byte_array(hoPrepInfo);
  // UE History Info (M)
  msg.source2target->ue_history_info.cause = malloc_or_fail(sizeof(*msg.source2target->ue_history_info.cause));
  msg.source2target->ue_history_info.cause->type = NGAP_CAUSE_RADIO_NETWORK;
  msg.source2target->ue_history_info.cause->value = NGAP_CAUSE_RADIO_NETWORK_HANDOVER_DESIRABLE_FOR_RADIO_REASON;
  msg.source2target->ue_history_info.type = NGAP_CellSize_small;
  msg.source2target->ue_history_info.time_in_cell = min(time(NULL) - UE->last_seen, 4095);
  msg.source2target->ue_history_info.id = target_cell;

  /* Fill both PDU Session Resource List IE (M) in Handover Required
     and PDU Session Resource Information List IE (O) in the Source NG-RAN
     Node to Target NG-RAN Node Transparent Container */
  FOR_EACH_SEQ_ARR(rrc_pdu_session_param_t*, pduSession, &UE->pduSessions) {
    if (pduSession->status != PDU_SESSION_STATUS_DONE && pduSession->status != PDU_SESSION_STATUS_ESTABLISHED)
      continue;
    pdusession_t *session = &pduSession->param;
    DevAssert(msg.nb_of_pdusessions < NGAP_MAX_PDU_SESSION);
    pdusession_resource_t *pdu = &msg.pdusessions[msg.nb_of_pdusessions++];
    // PDU Session ID
    pdu->pdusession_id = session->pdusession_id;
    // PDU Session Resource Information List (O)
    DevAssert(msg.source2target->nb_pdu_session_resource < NGAP_MAX_PDU_SESSION);
    pdusession_resource_info_t *pdu_info = &msg.source2target->pdu_session_resource[msg.source2target->nb_pdu_session_resource++];
    pdu_info->pdusession_id = session->pdusession_id;
    FOR_EACH_SEQ_ARR(nr_rrc_qos_t *, qos, &session->qos) {
      DevAssert(pdu_info->nb_of_qos_flow < MAX_QOS_FLOWS);
      qosflow_info_t *qos_flow = &pdu_info->qos_flow_info[pdu_info->nb_of_qos_flow++];
      qos_flow->qfi = qos->qos.qfi;
    }
  }

  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_GNB, 0, NGAP_HANDOVER_REQUIRED);
  NGAP_HANDOVER_REQUIRED(msg_p) = msg;
  itti_send_msg_to_task(TASK_NGAP, rrc->module_id, msg_p);
}

int rrc_gNB_process_NGAP_DL_RAN_STATUS_TRANSFER(MessageDef *msg_p, instance_t instance)
{
  const ngap_ran_status_transfer_t *cmd = &NGAP_DL_RAN_STATUS_TRANSFER(msg_p);
  gNB_RRC_INST *rrc = RC.nrrrc[instance];
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_get_ue_context(rrc, cmd->gnb_ue_ngap_id);

  if (!ue_context_p) {
    LOG_E(NR_RRC, "[gNB %ld] No UE context for gNB_ue_ngap_id %u\n", instance, cmd->gnb_ue_ngap_id);
    return -1;
  }

  gNB_RRC_UE_t *UE = &ue_context_p->ue_context;
  LOG_I(NR_RRC,
        "[gNB %ld] DL RAN Status Transfer for gNB_ue_ngap_id %u AMF_UE_NGAP_ID %lu\n",
        instance,
        cmd->gnb_ue_ngap_id,
        cmd->amf_ue_ngap_id);

  for (int i = 0; i < cmd->ran_status.nb_drb; ++i) {
    const ngap_drb_status_t *s = &cmd->ran_status.drb_status_list[i];
    LOG_I(NR_RRC,
          "DL RAN Status Transfer - DRB ID %d:\n"
          "  UL COUNT: PDCP SN = %u, HFN = %u (%s)\n"
          "  DL COUNT: PDCP SN = %u, HFN = %u (%s)\n",
          s->drb_id,
          s->ul_count.pdcp_sn,
          s->ul_count.hfn,
          s->ul_count.sn_len == NGAP_SN_LENGTH_18 ? "18-bit" : "12-bit",
          s->dl_count.pdcp_sn,
          s->dl_count.hfn,
          s->dl_count.sn_len == NGAP_SN_LENGTH_18 ? "18-bit" : "12-bit");

    // Send to PDCP layer
    e1_notify_pdcp_status(rrc, UE, s);
  }

  return 0;
}

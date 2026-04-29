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

#ifndef FIVEG_PLATFORM_TYPES_H__
#define FIVEG_PLATFORM_TYPES_H__

#include <stdint.h>

typedef struct plmn_id_s {
  uint16_t mcc;
  uint16_t mnc;
  uint8_t mnc_digit_length;
} plmn_id_t;

typedef struct nssai_s {
  uint8_t sst;
  uint32_t sd;
} nssai_t;

// Globally Unique AMF Identifier
typedef struct nr_guami_s {
  plmn_id_t plmn;
  uint8_t amf_region_id;
  uint16_t amf_set_id;
  uint8_t amf_pointer;
} nr_guami_t;

typedef enum {
  PDUSessionType_ipv4 = 0,
  PDUSessionType_ipv6 = 1,
  PDUSessionType_ipv4v6 = 2,
  PDUSessionType_ethernet = 3,
  PDUSessionType_unstructured = 4
} pdu_session_type_t;

typedef enum { NON_DYNAMIC, DYNAMIC } fiveQI_t;

/* QoS Priority Level */
typedef enum {
  QOS_PRIORITY_SPARE = 0,
  QOS_PRIORITY_HIGHEST,
  QOS_PRIORITY_2,
  QOS_PRIORITY_3,
  QOS_PRIORITY_4,
  QOS_PRIORITY_5,
  QOS_PRIORITY_6,
  QOS_PRIORITY_7,
  QOS_PRIORITY_8,
  QOS_PRIORITY_9,
  QOS_PRIORITY_10,
  QOS_PRIORITY_11,
  QOS_PRIORITY_12,
  QOS_PRIORITY_13,
  QOS_PRIORITY_LOWEST,
  QOS_NO_PRIORITY
} qos_priority_t;

/* Pre-emption Capability */
typedef enum {
  PEC_SHALL_NOT_TRIGGER_PREEMPTION = 0,
  PEC_MAY_TRIGGER_PREEMPTION,
  PEC_MAX,
} qos_pec_t;

/* Pre-emption Vulnerability */
typedef enum {
  PEV_NOT_PREEMPTABLE = 0,
  PEV_PREEMPTABLE = 1,
  PEV_MAX,
} qos_pev_t;

/* Allocation Retention Priority */
typedef struct {
  qos_priority_t priority_level;
  qos_pec_t pre_emp_capability;
  qos_pev_t pre_emp_vulnerability;
} qos_arp_t;

typedef struct pdusession_level_qos_parameter_s {
  uint8_t qfi;
  uint64_t fiveQI;
  uint64_t qos_priority;
  fiveQI_t fiveQI_type;
  qos_arp_t arp;
} pdusession_level_qos_parameter_t;

#endif

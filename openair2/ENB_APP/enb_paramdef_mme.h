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

/*! \file openair2/ENB_APP/enb_paramdef_mme.h
 * \brief definition of configuration parameters for MME modules 
 * \author Javier MORGADE
 * \date 2019
 * \version 0.1
 * \company VICOMTECH Spain
 * \email: javier.morgade@ieee.org
 * \note
 * \warning
 */

#ifndef __ENB_APP_ENB_PARAMDEF_MME__H__
#define __ENB_APP_ENB_PARAMDEF_MME__H__

#include "common/config/config_paramdesc.h"
#include "RRC_paramsvalues.h"

#define MME_CONFIG_STRING_NETWORK_INTERFACES_CONFIG     "NETWORK_INTERFACES"

/* interface configuration parameters names   */
/* M3 interface configuration parameters names */
#define MME_CONFIG_STRING_MME_INTERFACE_NAME_FOR_M3_MCE "MME_INTERFACE_NAME_FOR_M3_MCE"
#define MME_CONFIG_STRING_MME_IPV4_ADDRESS_FOR_M3C "MME_IPV4_ADDRESS_FOR_M3C"
#define MME_CONFIG_STRING_MME_PORT_FOR_M3C "MME_PORT_FOR_M3C"


#define MME_NETPARAMS_DESC {  \
{MME_CONFIG_STRING_MME_INTERFACE_NAME_FOR_M3_MCE,        NULL,      0,        .strptr=&mme_interface_name_for_m3_mce,   .defstrval="lo",      TYPE_STRING,      0},      \
{MME_CONFIG_STRING_MME_IPV4_ADDRESS_FOR_M3C,             NULL,      0,        .strptr=&mme_ipv4_address_for_m3c,        .defstrval="127.0.0.18/24",      TYPE_STRING,      0},      \
{MME_CONFIG_STRING_MME_PORT_FOR_M3C,                     NULL,      0,        .uptr=&mme_port_for_m3c,           	.defintval=36444L,    TYPE_UINT,        0},      \
} 

#endif

#/*
# * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# * contributor license agreements.  See the NOTICE file distributed with
# * this work for additional information regarding copyright ownership.
# * The OpenAirInterface Software Alliance licenses this file to You under
# * the OAI Public License, Version 1.1  (the "License"); you may not use this file
# * except in compliance with the License.
# * You may obtain a copy of the License at
# *
# *      http://www.openairinterface.org/?page_id=698
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# *-------------------------------------------------------------------------------
# * For more information about the OpenAirInterface (OAI) Software Alliance:
# *      contact@openairinterface.org
# */
#---------------------------------------------------------------------
# Python for CI of OAI-eNB + COTS-UE
#
#   Required Python Version
#     Python 3.x
#
#   Required Python Package
#     pexpect
#---------------------------------------------------------------------



#-----------------------------------------------------------
# Import Components
#-----------------------------------------------------------

import helpreadme as HELP
import constants as CONST


import cls_oaicitest		 #main class for OAI CI test framework
import cls_containerize	 #class Containerize for all container-based operations on RAN/UE objects
import cls_static_code_analysis  #class for static code analysis
import cls_cluster		 # class for building/deploying on cluster
import cls_native        # class for all native/source-based operations
from cls_ci_helper import TestCaseCtx

import ran
import cls_cmd
import cls_oai_html


#-----------------------------------------------------------
# Import Libs
#-----------------------------------------------------------
import sys		# arg
import re		# reg
import time		# sleep
import os
import subprocess
import lxml.etree as ET
import logging
import signal
import traceback
logging.basicConfig(
	level=logging.DEBUG,
	stream=sys.stdout,
	format="[%(asctime)s] %(levelname)8s: %(message)s"
)




#-----------------------------------------------------------
# General Functions
#-----------------------------------------------------------



def CheckClassValidity(xml_class_list,action,id):
	if action not in xml_class_list:
		logging.error('test-case ' + id + ' has unlisted class ' + action + ' ##CHECK xml_class_list.yml')
		resp=False
	else:
		resp=True
	return resp

def ExecuteActionWithParam(action, ctx, node):
	global RAN
	global HTML
	global CONTAINERS
	global SCA
	global CLUSTER
	if action == 'Build_eNB' or action == 'Build_Image' or action == 'Build_Proxy' or action == "Build_Cluster_Image" or action == "Build_Run_Tests":
		RAN.Build_eNB_args=test.findtext('Build_eNB_args')
		CONTAINERS.imageKind=test.findtext('kind')
		proxy_commit = test.findtext('proxy_commit')
		if proxy_commit is not None:
			CONTAINERS.proxyCommit = proxy_commit
		if action == 'Build_eNB':
			success = cls_native.Native.Build(ctx, node, HTML, RAN.eNBSourceCodePath, RAN.Build_eNB_args)
		elif action == 'Build_Image':
			success = CONTAINERS.BuildImage(ctx, node, HTML)
		elif action == 'Build_Proxy':
			success = CONTAINERS.BuildProxy(ctx, node, HTML)
		elif action == 'Build_Cluster_Image':
			success = CLUSTER.BuildClusterImage(ctx, node, HTML)
		elif action == 'Build_Run_Tests':
			success = CONTAINERS.BuildRunTests(ctx, node, HTML)

	elif action == 'Initialize_eNB':
		datalog_rt_stats_file=test.findtext('rt_stats_cfg')
		if datalog_rt_stats_file is None:
			RAN.datalog_rt_stats_file='datalog_rt_stats.default.yaml'
		else:
			RAN.datalog_rt_stats_file=datalog_rt_stats_file
		RAN.Initialize_eNB_args=test.findtext('Initialize_eNB_args')
		USRPIPAddress = test.findtext('USRP_IPAddress') or ''

		#local variable air_interface
		air_interface = test.findtext('air_interface')		
		if (air_interface is None) or (air_interface.lower() not in ['nr','lte']):
			RAN.air_interface = 'lte-softmodem'
		else:
			RAN.air_interface = air_interface.lower() +'-softmodem'

		cmd_prefix = test.findtext('cmd_prefix')
		if cmd_prefix is not None: RAN.cmd_prefix = cmd_prefix
		success = RAN.InitializeeNB(ctx, node, HTML)

	elif action == 'Terminate_eNB':
		#retx checkers
		string_field = test.findtext('d_retx_th')
		if (string_field is not None):
			RAN.ran_checkers['d_retx_th'] = [float(x) for x in string_field.split(',')]
		string_field=test.findtext('u_retx_th')
		if (string_field is not None):
			RAN.ran_checkers['u_retx_th'] = [float(x) for x in string_field.split(',')]

		#local variable air_interface
		air_interface = test.findtext('air_interface')		
		if (air_interface is None) or (air_interface.lower() not in ['nr','lte']):
			RAN.air_interface = 'lte-softmodem'
		else:
			RAN.air_interface = air_interface.lower() +'-softmodem'
		success = RAN.TerminateeNB(ctx, node, HTML)

	elif action == 'Initialize_UE' or action == 'Attach_UE' or action == 'Detach_UE' or action == 'Terminate_UE' or action == 'CheckStatusUE' or action == 'DataEnable_UE' or action == 'DataDisable_UE':
		CiTestObj.ue_ids = test.findtext('id').split(' ')
		if action == 'Initialize_UE':
			success = CiTestObj.InitializeUE(node, HTML)
		elif action == 'Attach_UE':
			success = CiTestObj.AttachUE(node, HTML)
		elif action == 'Detach_UE':
			success = CiTestObj.DetachUE(node, HTML)
		elif action == 'Terminate_UE':
			success = CiTestObj.TerminateUE(ctx, node, HTML)
		elif action == 'CheckStatusUE':
			success = CiTestObj.CheckStatusUE(node, HTML)
		elif action == 'DataEnable_UE':
			success = CiTestObj.DataEnableUE(node, HTML)
		elif action == 'DataDisable_UE':
			success = CiTestObj.DataDisableUE(node, HTML)

	elif action == 'Ping':
		CiTestObj.ping_args = test.findtext('ping_args')
		CiTestObj.ping_packetloss_threshold = test.findtext('ping_packetloss_threshold')
		CiTestObj.ue_ids = test.findtext('id').split(' ')
		CiTestObj.svr_id = test.findtext('svr_id')
		if test.findtext('svr_node'):
			CiTestObj.svr_node = test.findtext('svr_node') if not force_local else 'localhost'
		ping_rttavg_threshold = test.findtext('ping_rttavg_threshold') or ''
		success = CiTestObj.Ping(ctx, node, HTML)

	elif action == 'Iperf' or action == 'Iperf2_Unidir':
		CiTestObj.iperf_args = test.findtext('iperf_args')
		CiTestObj.ue_ids = test.findtext('id').split(' ')
		CiTestObj.svr_id = test.findtext('svr_id')
		if test.findtext('svr_node'):
			CiTestObj.svr_node = test.findtext('svr_node') if not force_local else 'localhost'
		CiTestObj.iperf_packetloss_threshold = test.findtext('iperf_packetloss_threshold')
		CiTestObj.iperf_bitrate_threshold = test.findtext('iperf_bitrate_threshold') or '90'
		CiTestObj.iperf_profile = test.findtext('iperf_profile') or 'balanced'
		CiTestObj.iperf_tcp_rate_target = test.findtext('iperf_tcp_rate_target') or None
		if CiTestObj.iperf_profile != 'balanced' and CiTestObj.iperf_profile != 'unbalanced' and CiTestObj.iperf_profile != 'single-ue':
			logging.error(f'test-case has wrong profile {CiTestObj.iperf_profile}, forcing balanced')
			CiTestObj.iperf_profile = 'balanced'
		if action == 'Iperf':
			success = CiTestObj.Iperf(ctx, node, HTML)
		elif action == 'Iperf2_Unidir':
			success = CiTestObj.Iperf2_Unidir(ctx, node, HTML)

	elif action == 'IdleSleep':
		st = test.findtext('idle_sleep_time_in_sec') or "5"
		success = cls_oaicitest.IdleSleep(HTML, int(st))

	elif action == 'Deploy_Run_OC_PhySim':
		oc_release = test.findtext('oc_release')
		script = "scripts/oc-deploy-physims.sh"
		image_tag = cls_containerize.CreateTag(CLUSTER.ranCommitID, CLUSTER.ranBranch, CLUSTER.ranAllowMerge)
		options = f"oaicicd-core-for-ci-ran {oc_release} {image_tag} {CLUSTER.eNBSourceCodePath}"
		workdir = CLUSTER.eNBSourceCodePath
		success = cls_oaicitest.Deploy_Physim(ctx, HTML, node, workdir, script, options)

	elif action == 'Build_Deploy_Docker_PhySim' or action == 'Build_Deploy_Source_PhySim':
		ctest_opt = test.findtext('ctest-opt') or ''
		script = "scripts/docker-build-and-deploy-physims.sh" if action == 'Build_Deploy_Docker_PhySim' else 'scripts/source-deploy-physims.sh'
		options = f"{CONTAINERS.eNBSourceCodePath} {ctest_opt}"
		workdir = CONTAINERS.eNBSourceCodePath
		success = cls_oaicitest.Deploy_Physim(ctx, HTML, node, workdir, script, options)

	elif action == 'DeployCoreNetwork' or action == 'UndeployCoreNetwork':
		cn_id = test.findtext('cn_id')
		core_op = getattr(cls_oaicitest.OaiCiTest, action)
		success = core_op(cn_id, ctx, HTML)

	elif action == 'Deploy_Object' or action == 'Undeploy_Object' or action == "Create_Workspace" or action == "Stop_Object":
		CONTAINERS.yamlPath = test.findtext('yaml_path')
		string_field=test.findtext('d_retx_th')
		if (string_field is not None):
			CONTAINERS.ran_checkers['d_retx_th'] = [float(x) for x in string_field.split(',')]
		string_field=test.findtext('u_retx_th')
		if (string_field is not None):
			CONTAINERS.ran_checkers['u_retx_th'] = [float(x) for x in string_field.split(',')]
		CONTAINERS.services = test.findtext('services')
		CONTAINERS.num_attempts = int(test.findtext('num_attempts') or 1)
		CONTAINERS.deploymentTag = cls_containerize.CreateTag(CONTAINERS.ranCommitID, CONTAINERS.ranBranch, CONTAINERS.ranAllowMerge)
		if action == 'Deploy_Object':
			success = CONTAINERS.DeployObject(ctx, node, HTML)
		elif action == 'Stop_Object':
			success = CONTAINERS.StopObject(ctx, node, HTML)
		elif action == 'Undeploy_Object':
			success = CONTAINERS.UndeployObject(ctx, node, HTML, RAN)
		elif action == 'Create_Workspace':
			if force_local:
				# Do not create a working directory when running locally. Current repo directory will be used
				return True
			success = CONTAINERS.Create_Workspace(node, HTML)

	elif action == 'LicenceAndFormattingCheck':
		success = SCA.LicenceAndFormattingCheck(ctx, node, HTML)

	elif action == 'Cppcheck_Analysis':
		success = SCA.CppCheckAnalysis(ctx, node, HTML)

	elif action == 'Push_Local_Registry':
		tag_prefix = test.findtext('tag_prefix') or ""
		success = CONTAINERS.Push_Image_to_Local_Registry(node, HTML, tag_prefix)

	elif action == 'Pull_Local_Registry' or action == 'Clean_Test_Server_Images':
		if force_local:
			# Do not pull or remove images when running locally. User is supposed to handle image creation & cleanup
			return True
		tag_prefix = test.findtext('tag_prefix') or ""
		images = test.findtext('images').split()
		# hack: for FlexRIC, we need to overwrite the tag to use
		tag = None
		if len(images) == 1 and images[0] == "oai-flexric":
			tag = CONTAINERS.flexricTag
		if action == "Pull_Local_Registry":
			success = CONTAINERS.Pull_Image_from_Registry(HTML, node, images, tag=tag, tag_prefix=tag_prefix)
		if action == "Clean_Test_Server_Images":
			success = CONTAINERS.Clean_Test_Server_Images(HTML, node, images, tag=tag)

	elif action == 'Custom_Command':
		command = test.findtext('command')
		# Allow referencing repository workspace path in XML via %%workspace%%
		command = command.replace("%%workspace%%", CONTAINERS.eNBSourceCodePath)
		success = cls_oaicitest.Custom_Command(HTML, node, command)

	elif action == 'Custom_Script':
		script = test.findtext('script')
		args = test.findtext('args')
		# Allow referencing repository workspace path in XML via %%workspace%%
		script = script.replace("%%workspace%%", CONTAINERS.eNBSourceCodePath)
		success = cls_oaicitest.Custom_Script(HTML, node, script, args)

	elif action == 'Pull_Cluster_Image':
		tag_prefix = test.findtext('tag_prefix') or ""
		images = test.findtext('images').split()
		success = CLUSTER.PullClusterImage(HTML, node, images, tag_prefix=tag_prefix)

	else:
		logging.warning(f"unknown action {action}, skip step")
		success = True # by default, we skip the step and print a warning

	return success

#check if given test is in list
#it is in list if one of the strings in 'list' is at the beginning of 'test'
def test_in_list(test, list):
	for check in list:
		check=check.replace('+','')
		if (test.startswith(check)):
			return True
	return False

test_runner_abort = False
def receive_signal(signum, frame):
    global test_runner_abort
    if not test_runner_abort:
        logging.warning("received signal, canceling steps")
        logging.info("send signal again to exit immediately")
        test_runner_abort = True
    else:
        logging.warning("received signal again, exiting")
        sys.exit(1)

def ShowTestID(ctx, desc, file, line):
    logging.info(f'\u001B[1m----------------------------------------\u001B[0m')
    logging.info(f'\u001B[1m Test #{ctx.test_idx} ({file}:{line})   \u001B[0m')
    logging.info(f'\u001B[1m {desc}                                 \u001B[0m')
    logging.info(f'\u001B[1m----------------------------------------\u001B[0m')

#-----------------------------------------------------------
# MAIN PART
#-----------------------------------------------------------

#loading xml action list from yaml
import yaml
xml_class_list_file='xml_class_list.yml'
if (os.path.isfile(xml_class_list_file)):
	yaml_file=xml_class_list_file
elif (os.path.isfile('ci-scripts/'+xml_class_list_file)):
	yaml_file='ci-scripts/'+xml_class_list_file
else:
	logging.error("XML action list yaml file cannot be found")
	sys.exit("XML action list yaml file cannot be found")

with open(yaml_file,'r') as f:
    # The FullLoader parameter handles the conversion-$
    #from YAML scalar values to Python dictionary format$
    xml_class_list = yaml.load(f,Loader=yaml.FullLoader)

mode = ''

CiTestObj = cls_oaicitest.OaiCiTest()
 
RAN = ran.RANManagement()
HTML = cls_oai_html.HTMLManagement()
CONTAINERS = cls_containerize.Containerize()
SCA = cls_static_code_analysis.StaticCodeAnalysis()
CLUSTER = cls_cluster.Cluster()

#-----------------------------------------------------------
# Parsing Command Line Arguments
#-----------------------------------------------------------

import args_parse
# Force local execution, move all execution targets to localhost
force_local = False
mode, force_local = args_parse.ArgsParse(sys.argv,CiTestObj,RAN,HTML,CONTAINERS,HELP,SCA,CLUSTER)

#-----------------------------------------------------------
# mode amd XML class (action) analysis
#-----------------------------------------------------------
cwd = os.getcwd()

if re.match('^TerminateeNB$', mode, re.IGNORECASE):
	logging.warning("Option TerminateeNB ignored")
elif re.match('^TerminateHSS$', mode, re.IGNORECASE):
	logging.warning("Option TerminateHSS ignored")
elif re.match('^TerminateMME$', mode, re.IGNORECASE):
	logging.warning("Option TerminateMME ignored")
elif re.match('^TerminateSPGW$', mode, re.IGNORECASE):
	logging.warning("Option TerminateSPGW ignored")
elif re.match('^LogCollectBuild$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectBuild ignored")
elif re.match('^LogCollecteNB$', mode, re.IGNORECASE):
	if RAN.eNBSourceCodePath == '':
		HELP.GenericHelp(CONST.Version)
		sys.exit('Insufficient Parameter')
	if os.path.isdir('cmake_targets/log'):
		cmd = 'zip -r enb.log.' + RAN.BuildId + '.zip cmake_targets/log'
		logging.info(cmd)
		try:
			zipStatus = subprocess.check_output(cmd, shell=True, stderr=subprocess.STDOUT, universal_newlines=True, timeout=60)
		except subprocess.CalledProcessError as e:
			logging.error("Command '{}' returned non-zero exit status {}.".format(e.cmd, e.returncode))
			logging.error("Error output:\n{}".format(e.output))
		sys.exit(0)
elif re.match('^LogCollectHSS$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectHSS ignored")
elif re.match('^LogCollectMME$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectMME ignored")
elif re.match('^LogCollectSPGW$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectSPGW ignored")
elif re.match('^LogCollectPing$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectPing ignored")
elif re.match('^LogCollectIperf$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectIperf ignored")
elif re.match('^LogCollectOAIUE$', mode, re.IGNORECASE):
	logging.warning("Option LogCollectOAIUE ignored")
elif re.match('^InitiateHtml$', mode, re.IGNORECASE):
	count = 0
	foundCount = 0
	while (count < HTML.nbTestXMLfiles):
		#xml_test_file = cwd + "/" + CiTestObj.testXMLfiles[count]
		xml_test_file = sys.path[0] + "/" + CiTestObj.testXMLfiles[count]
		if (os.path.isfile(xml_test_file)):
			try:
				xmlTree = ET.parse(xml_test_file)
			except Exception as e:
				print(f"Error: {e} while parsing file: {xml_test_file}.")
			xmlRoot = xmlTree.getroot()
			HTML.htmlTabRefs.append(xmlRoot.findtext('htmlTabRef',default='test-tab-' + str(count)))
			HTML.htmlTabNames.append(xmlRoot.findtext('htmlTabName',default='test-tab-' + str(count)))
			HTML.htmlTabIcons.append(xmlRoot.findtext('htmlTabIcon',default='info-sign'))
			foundCount += 1
		count += 1
	if foundCount != HTML.nbTestXMLfiles:
		HTML.nbTestXMLfiles=foundCount
	
	HTML.CreateHtmlHeader()
elif re.match('^FinalizeHtml$', mode, re.IGNORECASE):
	logging.info('\u001B[1m----------------------------------------\u001B[0m')
	logging.info('\u001B[1m  Creating HTML footer \u001B[0m')
	logging.info('\u001B[1m----------------------------------------\u001B[0m')

	HTML.CreateHtmlFooter(CiTestObj.finalStatus)
elif re.match('^TesteNB$', mode, re.IGNORECASE) or re.match('^TestUE$', mode, re.IGNORECASE):
	logging.info('\u001B[1m----------------------------------------\u001B[0m')
	logging.info('\u001B[1m  Starting Scenario: ' + CiTestObj.testXMLfiles[0] + '\u001B[0m')
	logging.info('\u001B[1m----------------------------------------\u001B[0m')
	if re.match('^TesteNB$', mode, re.IGNORECASE):
		if RAN.ranRepository == '' or RAN.ranBranch == '' or RAN.eNBSourceCodePath == '':
			HELP.GenericHelp(CONST.Version)
			if RAN.ranRepository == '':
				HELP.GitSrvHelp(RAN.ranRepository, RAN.ranBranch, RAN.ranCommitID, RAN.ranAllowMerge, RAN.ranTargetBranch)
			if RAN.eNBSourceCodePath == '':
				HELP.eNBSrvHelp(RAN.eNBSourceCodePath)
			sys.exit('Insufficient Parameter')
	else:
		if CiTestObj.ranRepository == '' or CiTestObj.ranBranch == '':
			HELP.GenericHelp(CONST.Version)
			sys.exit('UE: Insufficient Parameter')

	#read test_case_list.xml file
	# if no parameters for XML file, use default value
	if (HTML.nbTestXMLfiles != 1):
		xml_test_file = cwd + "/test_case_list.xml"
	else:
		xml_test_file = cwd + "/" + CiTestObj.testXMLfiles[0]

	# directory where all log artifacts will be placed
	logPath = f"{cwd}/../cmake_targets/log/{CiTestObj.testXMLfiles[0].split('/')[-1]}.d"
	# we run from within ci-scripts, but the logPath is absolute, so replace
	# the ci-scripts/..; if it does not exist, nothing will happen
	logPath = logPath.replace(r'/ci-scripts/..', '')
	logging.info(f"placing all artifacts for this run in {logPath}/")
	with cls_cmd.LocalCmd() as c:
		c.run(f"rm -rf {logPath}")
		c.run(f"mkdir -p {logPath}")

	xmlTree = ET.parse(xml_test_file)
	xmlRoot = xmlTree.getroot()

	if (HTML.nbTestXMLfiles == 1):
		HTML.htmlTabRefs.append(xmlRoot.findtext('htmlTabRef',default='test-tab-0'))
		HTML.htmlTabNames.append(xmlRoot.findtext('htmlTabName',default='Test-0'))
	all_tests=xmlRoot.findall('testCase')

	signal.signal(signal.SIGINT, receive_signal)

	HTML.CreateHtmlTabHeader()

	task_set_succeeded = True
	HTML.startTime=int(round(time.time() * 1000))

	for index, test in enumerate(all_tests, start=1):
		if test_runner_abort:
			task_set_succeeded = False
		test_case_idx = f"{index:06d}"
		ctx = TestCaseCtx(int(test_case_idx), logPath)
		HTML.testCaseIdx = test_case_idx
		desc = test.findtext('desc')
		node = test.findtext('node') if not force_local else 'localhost'
		always_exec = test.findtext('always_exec') in ['True', 'true', 'Yes', 'yes']
		may_fail = test.findtext('may_fail') in ['True', 'true', 'Yes', 'yes']
		HTML.desc = desc
		action = test.findtext('class')
		if not CheckClassValidity(xml_class_list, action, test_case_idx):
			task_set_succeeded = False
			continue
		file = os.path.basename(xml_test_file)
		line = test.find('class').sourceline
		ShowTestID(ctx, desc, file, line)
		if not task_set_succeeded and not always_exec:
			msg = f"skipping test due to prior error"
			logging.warning(msg)
			HTML.CreateHtmlTestRowQueue(msg, "SKIP", [])
			continue
		try:
			test_succeeded = ExecuteActionWithParam(action, ctx, node)
			if not test_succeeded and may_fail:
				logging.warning(f"test ID {test_case_idx} action {action} may or may not fail, proceeding despite error")
			elif not test_succeeded:
				logging.error(f"test ID {test_case_idx} action {action} failed ({test_succeeded}), skipping next tests")
				task_set_succeeded = False
		except Exception as e:
			s = traceback.format_exc()
			logging.error(f'while running CI, an exception occurred:\n{s}')
			HTML.CreateHtmlTestRowQueue("N/A", 'KO', [f"CI test code encountered an exception:\n{s}"])
			task_set_succeeded = False
			continue

	if not task_set_succeeded:
		logging.error('\u001B[1;37;41mScenario failed\u001B[0m')
		HTML.CreateHtmlTabFooter(False)
		sys.exit('Failed Scenario')
	else:
		logging.info('\u001B[1;37;42mScenario passed\u001B[0m')
		HTML.CreateHtmlTabFooter(True)
elif re.match('^LoadParams$', mode, re.IGNORECASE):
	pass
else:
	HELP.GenericHelp(CONST.Version)
	sys.exit('Invalid mode')
sys.exit(0)

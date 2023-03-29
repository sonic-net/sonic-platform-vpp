# Copyright (c) 2023 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys
import subprocess
import yaml
import ipaddress
import pprint
import re
import argparse
from jinja2 import Environment, FileSystemLoader

def getIppoolAndCidrMask(ipPoolCidr):
    separator = "/"
    ippool = ipPoolCidr.split(separator)
    return ippool[0], ippool[1]

def checkPosInteger(num):
    if not num.isdigit():
        print(f'Invalid IPPool has been provided')
        quit()

def checkIp(ip):
    try:
        ipaddress.ip_address(ip)
    except ValueError:
        print(f'Invalid IP address specified - {ip}')
        quit()

def checkAction(action):

    if  action != "build" and action != "tear" and action != "build,tear":
        print("Invalid action specified")
        quit()
  

def checkTopology(topology):
    if not (topology == "singleHop" or topology == "multiHop"):
        print("Invalid topology specified")
        quit()

def checkRouting(routing):
    if not (routing == "static" or routing == "bgp"):
        print("Invalid routing protocol specified")
        quit()

def getSubnet(ippool):

    separator = "."

    subnetArr = ippool.split(separator)

    subnetReqdArr = subnetArr[:2]

    subnet =  separator.join(subnetReqdArr)

    return subnet

def checkVerboselog(verboselog):
    for i in verboselog:
        if i != 'v' and verboselog.count(i) != len(verboselog):
            print("Invalid verbose argument passed. Please pass like - 'v' or 'vv' or 'vvv' or 'vvvv' or 'vvvvvv")
            quit()


def createInventoryFromTemplate(subnet, username, password, host):

    environment = Environment(loader=FileSystemLoader("."))

    #inventory.j2
    template = environment.get_template("inventory.j2")

    #new inventory file with values filled at templatized variables
    filename = "inventory.yaml"
    content = template.render(
        subnet = subnet,
        username = username,
        password = password,
        host = host
    )
    with open(filename, mode="w", encoding="utf-8") as message:
        message.write(content)
        print(f"... wrote {filename}")

def build(ansiblePlaybook, topology, verboselogArg):

    cmd = ['ansible-playbook', '-i', 'inventory.yaml',  ansiblePlaybook, '--extra-vars', '@inventory.yaml', '--tags', topology,  verboselogArg]
    print(cmd)

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    for l in p.stdout.readlines():
        loglineDecoded = l.decode()
        print(loglineDecoded)

        if  re.search('failed=', loglineDecoded) != None and re.search('unreachable=', loglineDecoded) != None:
            if None == re.search('failed=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
                quit()
            if None == re.search('unreachable=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task, VM is unreachable')
                quit()

        if None != re.search('ok=0', loglineDecoded):
            print(f'unable to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
            quit()

    print("SUCCESSFUL in validating SONiC routing...\n")
   

def tear(ansiblePlaybook, topology, verboselogArg):

    cmd = ['ansible-playbook', '-i', 'inventory.yaml', ansiblePlaybook, '--tags', topology, verboselogArg]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    for l in p.stdout.readlines():
        loglineDecoded = l.decode()
        print(loglineDecoded)

        if  re.search('failed=', loglineDecoded) != None and re.search('unreachable=', loglineDecoded) != None:
            if None == re.search('failed=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
                quit()
            if None == re.search('unreachable=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task, VM is unreachable')
                quit()

        if None != re.search('ok=0', loglineDecoded):
            print(f'unable to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
            quit()  

    print("SUCCESSFUL in tearing down...\n")    


def runSonic(action, topology, routing, verboselogArg):

    #playbook file is first argument taken
    if action == "build":
        ansiblePlaybook = "sonic-sw-dp-bring-up.yaml"
        print("ansiblePlaybook : ", ansiblePlaybook, "\n")
        if topology == "multiHop":
           topology = topology + ',' + routing
        build(ansiblePlaybook, topology, verboselogArg)
    elif action == "tear":
        ansiblePlaybook = "sonic-sw-dp-bring-down.yaml"
        print("ansiblePlaybook : ", ansiblePlaybook, "\n")
        tear(ansiblePlaybook, topology, verboselogArg)



#Main

parser = argparse.ArgumentParser()
parser.add_argument("--action", type=str, help="enter action -  {'build' | 'tear'}", nargs='?', default="build,tear")
parser.add_argument("--topology", type=str, help="enter topology - {'singleHop' | 'multiHop'}", nargs='?', default="multiHop")
parser.add_argument("--routing", type=str, help="enter routing protocol - {'static' | 'bgp'}", nargs='?', default="bgp")
parser.add_argument("--IPPool", type=str, help="enter IPPool - example : '172.16.0.0/16'", nargs='?', default="172.16.0.0/16") 
parser.add_argument("--host", type=str, help="enter host - example : '10.64.98.181'", nargs='?',required=True )   
parser.add_argument("--username", type=str, help="enter SSH username for host", nargs='?',required=True ) 
parser.add_argument("--password", type=str, help="enter SSH user password for host", nargs='?',required=True )                       
parser.add_argument("--verboselog", type=str, help="enter verbose logging - example : 'v or vv or vvv etc'", nargs='?', default="v")


args = parser.parse_args()


action = args.action
topology = args.topology
routing = args.routing
ippoolCidr = args.IPPool
host = args.host
username = args.username
password = args.password
verboselog = args.verboselog


#check action
checkAction(action)

#check topology
checkTopology(topology)

#check routing protocol
checkRouting(routing)

#get ippool
ipPool, cidrMask = getIppoolAndCidrMask(ippoolCidr)

#validate ippool
checkIp(ipPool)

#validate cidrMask
checkPosInteger(cidrMask)

#extract subnet
subnet =  getSubnet(ipPool)

#validate host
checkIp(host)

#validate verbose argument
checkVerboselog(verboselog)

print('Arguments passed:\n')
print('_________________________\n')
print(f'action - {action}\ntopology - {topology}\nrouting - {routing}\nippool - {ippoolCidr}\nusername - {username}\npassword - {password}\nhost - {host}\nverbose - {verboselog}')
print('_________________________\n')

#create inventory yaml file
createInventoryFromTemplate(subnet, username, password, host)

#run python script
if action == "build,tear":
    runSonic("build", topology, routing, "-"+verboselog)
    print("Now tearing down...\n")
    runSonic("tear", topology, routing, "-"+verboselog)
else:
    runSonic(action, topology, routing, "-"+verboselog)

print("OPERATION COMPLETE...\n")    



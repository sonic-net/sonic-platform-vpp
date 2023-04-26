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

    if len(ippool) != 2:
        print(f'Invalid IP Pool - "{ipPoolCidr}" is specified')
        sys.exit(1)

    return ippool[0], ippool[1]

def checkPosInteger(num):
    if not num.isdigit():
        print(f'Invalid IPPool has been provided')
        sys.exit(1)

def checkIp(ip):
    try:
        ipaddress.ip_address(ip)
    except ValueError:
        print(f'Invalid IP address specified - {ip}')
        sys.exit(1)

def checkAction(action):

    if  action != "build" and action != "tear" and action != "build,tear":
        print("Invalid action specified")
        sys.exit(1)
  

def checkTopology(topology):
    if not (topology == "singleHop" or topology == "multiHop"):
        print("Invalid topology specified")
        sys.exit(1)

def checkRouting(routing):
    if not (routing == "static" or routing == "bgp"):
        print("Invalid routing protocol specified")
        sys.exit(1)

def checkIpType(ip, iptype):

    if iptype == "ipv4":

        if not type(ipaddress.ip_address(ip)) is ipaddress.IPv4Address:
            print(f'Invalid IPV4 address - "{ip}" is specified')
            sys.exit(1)

    elif iptype == "ipv6":
         if not type(ipaddress.ip_address(ip)) is ipaddress.IPv6Address:
            print(f'Invalid IPV6 address - "{ip}" is specified')
            sys.exit(1)        


def getSubnet(ippool, separator):

    subnetArr = ippool.split(separator)

    subnetReqdArr = subnetArr[:2]

    subnet =  separator.join(subnetReqdArr)

    return subnet

def checkVerboselog(verboselog):
    for i in verboselog:
        if i != 'v' and verboselog.count(i) != len(verboselog):
            print("Invalid verbose argument passed. Please pass like - 'v' or 'vv' or 'vvv' or 'vvvv' or 'vvvvvv")
            sys.exit(1)


def checkNoLinuxNl(noLinuxNl):

    if noLinuxNl != "yes" and noLinuxNl != "no":
        print("Invalid 'noLinuxNl' argument specified. Enter 'yes' or 'no'")
        sys.exit(1)

def createInventoryFromTemplate(subnet, username, password, host, subnetV6, noLinuxNl):
    environment = Environment(loader=FileSystemLoader("."))

    #inventory.j2
    template = environment.get_template("inventory.j2")

    #new inventory file with values filled at templatized variables
    filename = "inventory.yaml"

    noLinuxNlArg = ""

    if noLinuxNl == "yes":
        noLinuxNlArg = "y"
    elif noLinuxNl == "no":
        noLinuxNlArg = "n"

    content = template.render(
        subnet = subnet,
        username = username,
        password = password,
        host = host,
        ipv6_subnet = subnetV6,
        no_linux_nl = noLinuxNlArg
    )
    with open(filename, mode="w", encoding="utf-8") as message:
        message.write(content)
        print(f"... wrote {filename}")

def execute(ansiblePlaybook, topology, verboselogArg):

    cmd = ['ansible-playbook', '-i', 'inventory.yaml',  ansiblePlaybook, '--extra-vars', '@inventory.yaml', '--tags', topology,  verboselogArg]
    print(cmd)

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    for l in p.stdout.readlines():
        loglineDecoded = l.decode()
        print(loglineDecoded)

        if  re.search('failed=', loglineDecoded) != None and re.search('unreachable=', loglineDecoded) != None:
            if None == re.search('failed=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
                sys.exit(1)
            if None == re.search('unreachable=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {topology} task, VM is unreachable')
                sys.exit(1)

        if None != re.search('ok=0', loglineDecoded):
            print(f'unable to execute {loglineDecoded.split().pop(0)} ansible {topology} task')
            sys.exit(1)

   

def runSonic(action, topology, routing, verboselogArg):

    #playbook file is first argument taken
    if action == "build":
        ansiblePlaybook = "sonic-sw-dp-bring-up.yaml"
        print("ansiblePlaybook : ", ansiblePlaybook, "\n")
        if topology == "multiHop":
           topology = topology + ',' + routing
        execute(ansiblePlaybook, topology, verboselogArg)
        print("SUCCESSFUL in validating SONiC routing...\n")

    elif action == "tear":
        ansiblePlaybook = "sonic-sw-dp-bring-down.yaml"
        print("ansiblePlaybook : ", ansiblePlaybook, "\n")
        execute(ansiblePlaybook, topology, verboselogArg)
        print("SUCCESSFUL in tearing down...\n")    



#Main

parser = argparse.ArgumentParser()
parser.add_argument("--action", type=str, help="enter action -  {'build' | 'tear'}", nargs='?', default="build,tear")
parser.add_argument("--topology", type=str, help="enter topology - {'singleHop' | 'multiHop'}", nargs='?', default="multiHop")
parser.add_argument("--routing", type=str, help="enter routing protocol - {'static' | 'bgp'}", nargs='?', default="bgp")
parser.add_argument("--IPPool", type=str, help="enter IPPool - example : '172.16.0.0/16'", nargs='?', default="172.16.0.0/16") 
parser.add_argument("--IPV6Pool", type=str, help="enter IPV6Pool - example : '2001:0db8::/64'", nargs='?', default="2001:0db8::/64") 
parser.add_argument("--host", type=str, help="enter host - example : '10.64.98.181'", nargs='?',required=True )   
parser.add_argument("--username", type=str, help="enter SSH username for host", nargs='?',required=True ) 
parser.add_argument("--password", type=str, help="enter SSH user password for host", nargs='?',required=True )                       
parser.add_argument("--verboselog", type=str, help="enter verbose logging - example : 'v or vv or vvv etc'", nargs='?', default="v")
parser.add_argument("--noLinuxNl", type=str, help="enter noLinuxNl - example : 'yes' or 'no'", nargs='?', default="no")


args = parser.parse_args()


action = args.action
topology = args.topology
routing = args.routing
ippoolCidr = args.IPPool
ipv6poolCidr = args.IPV6Pool
host = args.host
username = args.username
password = args.password
verboselog = args.verboselog
noLinuxNl = args.noLinuxNl


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

#check if ipv4
checkIpType(ipPool, "ipv4")

#validate cidrMask
checkPosInteger(cidrMask)

#extract subnet
subnet =  getSubnet(ipPool, ".")

#get ipv6pool
ipV6Pool, cidrV6Mask = getIppoolAndCidrMask(ipv6poolCidr)

#validate ipv6pool
checkIp(ipV6Pool)

#validate ipv6
checkIpType(ipV6Pool, "ipv6")

#validate cidrMask
checkPosInteger(cidrV6Mask)

#extract subnet
subnetV6 =  getSubnet(ipV6Pool, ":")

#validate host
checkIp(host)

#validate verbose argument
checkVerboselog(verboselog)

#validate noLinuxNl
checkNoLinuxNl(noLinuxNl)

print('Arguments passed:\n')
print('_________________________\n')
print(f'action - {action}\ntopology - {topology}\nrouting - {routing}\nippool - {ippoolCidr}\nipv6pool - {ipv6poolCidr}\nusername - {username}\npassword - {password}\nhost - {host}\nverbose - {verboselog}\nnoLinuxNl - {noLinuxNl}')
print('_________________________\n')

#create inventory yaml file
createInventoryFromTemplate(subnet, username, password, host, subnetV6, noLinuxNl)

#run python script
if action == "build,tear":
    runSonic("build", topology, routing, "-"+verboselog)
    print("Now tearing down...\n")
    runSonic("tear", topology, routing, "-"+verboselog)
else:
    runSonic(action, topology, routing, "-"+verboselog)

print("OPERATION COMPLETE...\n")    

sys.exit(0)

# Copyright (c) 2024 Cisco and/or its affiliates.
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
import ipaddress
import re
import argparse
from jinja2 import Environment, FileSystemLoader

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

def checkVerboselog(verboselog):
    for i in verboselog:
        if i != 'v' and verboselog.count(i) != len(verboselog):
            print("Invalid verbose argument passed. Please pass like - 'v' or 'vv' or 'vvv' or 'vvvv' or 'vvvvvv")
            sys.exit(1)


def createInventoryFromTemplate(username, password, host):

    environment = Environment(loader=FileSystemLoader("."))

    template = environment.get_template("inventory-vm.j2")

    filename = "inventory-vm.yaml"
    content = template.render(
        username = username,
        password = password,
        host = host
    )
    with open(filename, mode="w", encoding="utf-8") as message:
        message.write(content)
        print(f"... wrote {filename}")
      

def runSonic(action, verboselogArg):

    ansiblePlaybook = "sonic-vpp-vm-vxlan-bring-up.yaml"

    print("ansible-playbook : ", ansiblePlaybook, "\n")

    cmd = ['ansible-playbook', '-i', 'inventory-vm.yaml',  ansiblePlaybook, '--extra-vars', '@inventory-vm.yaml', '--tags', action, verboselogArg]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    for l in p.stdout.readlines():
        loglineDecoded = l.decode()
        print(loglineDecoded)

        if  re.search('failed=', loglineDecoded) != None and re.search('unreachable=', loglineDecoded) != None:
            if None == re.search('failed=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {action} task')
                sys.exit(1)
            if None == re.search('unreachable=0', loglineDecoded):
                print(f'failed to execute {loglineDecoded.split().pop(0)} ansible {action} task, VM is unreachable')
                sys.exit(1)

        if None != re.search('ok=0', loglineDecoded):
            print(f'unable to execute {loglineDecoded.split().pop(0)} ansible {action} task')
            sys.exit(1)

    if action == "build" or action == "build,tear":
        print("SUCCESSFUL in validating SONiC VM routing...\n")
    else:
        print("SUCCESSFUL in tearing down SONiC VM routing...\n")    

    print("OPERATION COMPLETE...\n") 


parser = argparse.ArgumentParser()
parser.add_argument("--action", type=str, help="enter action -  {'build' | 'tear'}", nargs='?', default="build,tear")   
parser.add_argument("--host", type=str, help="enter host - example : '10.64.98.181'", nargs='?',required=True )   
parser.add_argument("--username", type=str, help="enter SSH username for host", nargs='?',required=True )                       
parser.add_argument("--password", type=str, help="enter SSH user password for host", nargs='?',required=True )                       
parser.add_argument("--verboselog", type=str, help="enter verbose logging - example : 'v or vv or vvv etc'", nargs='?', default="v")


args = parser.parse_args()

action = args.action
host = args.host
username = args.username
password = args.password
verboselog = args.verboselog

checkAction(action)

checkIp(host)

checkVerboselog(verboselog)

print('Arguments passed:\n')
print('_________________________\n')
print(f'action - {action}\nusername - {username}\npassword - {password}\nhost - {host}\nverbose - {verboselog}')
print('_________________________\n')

createInventoryFromTemplate(username, password, host)

runSonic(action, "-"+verboselog)

sys.exit(0)

import sys
import subprocess
import yaml
import ipaddress
import pprint
import re
import argparse
from jinja2 import Environment, FileSystemLoader

def checkIp(ip):
    if ip == "localhost":
        return
    try:
        ipaddress.ip_address(ip)
    except ValueError:
        print(f'Invalid IP address specified - {ip}')
        sys.exit(1)

def checkAction(action):
    
    if  action != "build" and action != "tear" and action != "initialize" and action != "test" and action != "delete" and action != "build,tear":
        print("Invalid action specified")
        sys.exit(1)
  
def checkVerboselog(verboselog):
    for i in verboselog:
        if i != 'v' and verboselog.count(i) != len(verboselog):
            print("Invalid verbose argument passed. Please enter - 'v' or 'vv' or 'vvv' or 'vvvv' or 'vvvvvv")
            sys.exit(1)

def checkTopology(topology):
    
    if  topology != "t0" and topology != "t1":
        print("Invalid topology specified. Enter either t0 or t1")
        sys.exit(1)


def createInventoryFromTemplate(username, password, host, topology):

    environment = Environment(loader=FileSystemLoader("."))

    #inventory.j2
    template = environment.get_template("inventory-t0t1.j2")

    #new inventory file with values filled at templatized variables
    filename = "inventory-t0t1.yaml"
    content = template.render(
        username = username,
        password = password,
        host = host,
        topology = topology
    )
    with open(filename, mode="w", encoding="utf-8") as message:
        message.write(content)
        print(f"... wrote {filename}")
      

def runSonic(action, verboselogArg, topology):

    ansiblePlaybook = "sonic-vpp-t0t1-topology.yaml"

    print("ansible-playbook : ", ansiblePlaybook, "\n")

    tags = action

    if action == "build" or action == "tear" or action == "test":
        tags = action + topology #buildt0, buildt1, teart0, teart1

    cmd = ['ansible-playbook', '-i', 'inventory-t0t1.yaml',  ansiblePlaybook, '--extra-vars', '@inventory-t0t1.yaml', '--tags', tags, verboselogArg]
    print("cmd : ", cmd)

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


    print("SUCCESSFUL in Ansible execution for SONiC...\n")
 

    print("OPERATION COMPLETE...\n") 

#Main

parser = argparse.ArgumentParser()
parser.add_argument("--action", type=str, help="enter action -  {'initialize' | 'build' | 'tear' | 'test' | 'delete' }", nargs='?', default="build,tear")
parser.add_argument("--host", type=str, help="enter host - example : '10.64.98.181'", nargs='?',default="localhost" )   
parser.add_argument("--username", type=str, help="enter SSH username for host", nargs='?',required=True ) 
parser.add_argument("--password", type=str, help="enter SSH user password for host", nargs='?',required=True )                       
parser.add_argument("--verboselog", type=str, help="enter verbose logging - example : 'v or vv or vvv etc'", nargs='?', default="v")
parser.add_argument("--topology", type=str, help="enter topology - example : 't0 or t1'", nargs='?', default="t0")


args = parser.parse_args()

action = args.action
host = args.host
username = args.username
password = args.password
verboselog = args.verboselog
topology = args.topology

#check action
checkAction(action)

#validate host
checkIp(host)

#validate verbose argument
checkVerboselog(verboselog)

#validate topology argument
checkTopology(topology)

print('Arguments passed:\n')
print('_________________________\n')
print(f'action - {action}\nusername - {username}\npassword - {password}\nhost - {host}\nverbose - {verboselog}\ntopology - {topology}')
print('_________________________\n')

#create inventory yaml file
createInventoryFromTemplate(username, password, host, topology)

#run python script
runSonic(action, "-"+verboselog, topology)

sys.exit(0)



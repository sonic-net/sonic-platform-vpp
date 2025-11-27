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

import os
import signal
import sys
import syslog
import threading
import traceback

from swsscommon import swsscommon

from .config import ConfigMgr
from .directory import Directory
from .log import log_notice, log_crit
from .vpp import VppMgr
from .managers_vppcfg import VppCfgMgr
from .runner import Runner, signal_handler
from .template import TemplateFabric
from .utils import read_constants
from .vars import g_debug

VPP_TEMPLATE="/etc/sonic/vpp/vpp_template.json"

def do_work():
    """ Main function """
    vpp = VppMgr(VPP_TEMPLATE)

    common_objs = {
        'directory': Directory(),
        'cfg_mgr':   ConfigMgr(vpp),
        'tf':        TemplateFabric(),
        'constants': read_constants(),
    }
    managers = [
        VppCfgMgr(common_objs, "CONFIG_DB", "PLATFORM"),
        VppCfgMgr(common_objs, "STATE_DB", "PLATFORM"),
    ]
    runner = Runner(common_objs['cfg_mgr'])
    for mgr in managers:
        runner.add_manager(mgr)
    runner.run()


def main():
    rc = 0
    try:
        syslog.openlog('vppcfgd')
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)
        do_work()
    except KeyboardInterrupt:
        log_notice("Keyboard interrupt")
    except RuntimeError as exc:
        log_crit(str(exc))
        rc = -2
        if g_debug:
            raise
    except Exception as exc:
        log_crit("Got an exception %s: Traceback: %s" % (str(exc), traceback.format_exc()))
        rc = -1
        if g_debug:
            raise
    finally:
        syslog.closelog()
    try:
        sys.exit(rc)
    except SystemExit:
        os._exit(rc)


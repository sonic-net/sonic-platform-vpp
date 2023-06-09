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

import traceback
from .log import log_crit, log_err, log_debug, log_notice, dev_debug, g_dev_debug
from .manager import Manager
from .template import TemplateFabric
import socket
import json
from .utils import print_dict

class VppCfgMgr(Manager):
    """ This class updates vpp startup.conf when PLATFORM|vpp table entry is updated """
    def __init__(self, common_objs, db, table):
        """
        Initialize the object
        :param common_objs: common object dictionary
        :param db: name of the db
        :param table: name of the table in the db
        """
        super(VppCfgMgr, self).__init__(
            common_objs,
            [],
            db,
            table,
        )

    def set_handler(self, key, data):
        if key != "vpp":
            return True
        dev_debug("Key %s data %s" % (key, data))
        self.handle_config_data(data)
        return True


    def del_handler(self, key):
        pass

    @staticmethod
    def split_key(key):
        """
        :param key: key to split
        :return:  table, key
        """

        if '|' in key:
            return tuple(key.split('|', 1))
        else:
            return None, key

    def handle_config_data(self, data):
        print_dict(data)
        self.vpp_cfg = {}
        for key, val in data.items():
            val = val.replace("'", "\"")
            self.vpp_cfg[key] = json.loads(val)

        self.cfg_mgr.update(self.vpp_cfg)


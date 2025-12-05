# Copyright 2016 Microsoft, Inc.
# Modifications copyright (c) 2023 Cisco and/or its affiliates.
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

import syslog

from .vars import g_debug, g_dev_debug

def log_debug(msg):
    """ Send a message msg to the syslog as DEBUG """
    dev_debug(msg)
    if g_debug:
        syslog.syslog(syslog.LOG_DEBUG, msg)


def log_notice(msg):
    """ Send a message msg to the syslog as NOTICE """
    dev_debug(msg)
    syslog.syslog(syslog.LOG_NOTICE, msg)


def log_info(msg):
    """ Send a message msg to the syslog as INFO """
    dev_debug(msg)
    syslog.syslog(syslog.LOG_INFO, msg)


def log_warn(msg):
    """ Send a message msg to the syslog as WARNING """
    dev_debug(msg)
    syslog.syslog(syslog.LOG_WARNING, msg)


def log_err(msg):
    """ Send a message msg to the syslog as ERR """
    dev_debug(msg)
    syslog.syslog(syslog.LOG_ERR, msg)


def log_crit(msg):
    """ Send a message msg to the syslog as CRIT """
    dev_debug(msg)
    syslog.syslog(syslog.LOG_CRIT, msg)

def dev_debug(msg):
    if g_dev_debug:
        print(msg)

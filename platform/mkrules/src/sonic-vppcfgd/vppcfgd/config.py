import json
from .vars import g_dev_debug, VPP_CONFIG_JSON_PATH, VPP_CFG_STARTUP_PATH, VPP_BOOTUP_CFG_PATH, INDENT
from .log import dev_debug, log_notice
from .utils import print_dict
import os

class ConfigMgr(object):
    """ The class represents vpp configuration """
    def __init__(self, vpp):
        self.vpp = vpp
        self.vpp_json_cfg = None
        self.vpp_startup_conf = ''
        self.vpp_vports_conf = ''

    def reset(self):
        """ Reset stored config """
        self.vpp_json_cfg = None
        self.vpp_startup_conf = ''
        self.vpp_vports_conf = ''

    def deep_update(self, input, output):
        for key, val in input.items():
            dev_debug("key {} : val {}".format(key, val))
            if key in output:
                out_val = output[key]
                if type(val) == dict:
                    self.deep_update(val, out_val)
                else:
                    output[key] = val
            else:
                output[key] = val

    def update(self, sonic_vpp_cfg):
        """ Read current config from VPP """
        self.vpp_json_cfg = self.vpp.get_config()
        self.deep_update(sonic_vpp_cfg, self.vpp_json_cfg)
        print_dict(self.vpp_json_cfg)

    def render_vpp_config(self):
        indoff=''
        self.vpp_startup_conf=''
        self.gen_vpp_conf(self.vpp_json_cfg, indoff)

    def gen_vpp_conf(self, cfg, indoff):
        for key, val in cfg.items():
            if type(val) == dict:
                if key == 'vports':
                    continue
                if key == 'dev':
                    self.gen_dpdk_dev_cfg(val, indoff)
                elif key == 'plugin':
                    self.gen_plugins_cfg(val, indoff)
                else:
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+key+' '+'{'+'\n'
                    self.gen_vpp_conf(val, indoff+INDENT)
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+'}'+'\n'
            else:
                if val:
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+key+' '+val+'\n'
                else:
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+key+'\n'

    def gen_dpdk_dev_cfg(self, cfg, indoff):
        for key, val in cfg.items():
            if type(val) == dict:
                self.vpp_startup_conf = self.vpp_startup_conf+indoff+'dev '+key+' '+'{'+'\n'
                self.gen_dpdk_dev_cfg(val, indoff+INDENT)
                self.vpp_startup_conf = self.vpp_startup_conf+indoff+'}'+'\n'
            else:
                if val:
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+key+' '+val+'\n'
                else:
                    self.vpp_startup_conf = self.vpp_startup_conf+indoff+key+'\n'

    def gen_plugins_cfg(self, cfg, indoff):
        for key, val in cfg.items():
            self.vpp_startup_conf = self.vpp_startup_conf+indoff+'plugin '+key+' '+'{ '+val+' }'+'\n'

    def commit(self):
        """
        Write configuration change to VPP.
        :return: True if change was applied successfully, False otherwise
        """

        self.vpp_vports_config()
        self.vpp_dpdk_config()
        self.vpp_env_config()
        self.render_vpp_config()

        with open(VPP_CONFIG_JSON_PATH, "w") as fp:
            json.dump(self.vpp_json_cfg, fp, indent=4)

        dev_debug("bootup ports config {}".format(self.vpp_vports_conf))
        with open(VPP_BOOTUP_CFG_PATH, "w") as fp:
            fp.write(self.vpp_vports_conf)

        dev_debug("startup {}".format(self.vpp_startup_conf))
        with open(VPP_CFG_STARTUP_PATH, "w") as fp:
            fp.write(self.vpp_startup_conf)

        self.reset()

        log_notice("Config changes committed.");
        return True

    def vpp_vports_config(self):
        if 'vports' in self.vpp_json_cfg:
            for key, val in self.vpp_json_cfg['vports'].items():
                self.vpp_vports_conf = self.vpp_vports_conf + 'create host-interface name {}\n'.format(key)

    def vpp_dpdk_config(self):
        disable_dpdk = True

        if 'dpdk' in self.vpp_json_cfg and 'dev' in self.vpp_json_cfg['dpdk']:
            for key, val in self.vpp_json_cfg['dpdk']['dev'].items():
                if key != 'default':
                    disable_dpdk = False

        if disable_dpdk:
            self.disable_dpdk_cfg()

    def vpp_env_config(self):
        disable_dpdk = os.getenv('DPDK_DISABLE', 'n')
        if disable_dpdk == 'y':
            self.disable_dpdk_cfg()

        no_linux_nl = os.getenv('NO_LINUX_NL', 'n')
        if no_linux_nl == 'y':
            self.vpp_json_cfg['plugins']['plugin']['linux_nl_plugin.so'] = 'disable'

    def disable_dpdk_cfg(self):
        self.vpp_json_cfg['plugins']['plugin']['dpdk_plugin.so'] = 'disable'
        self.vpp_json_cfg.pop('dpdk', None)

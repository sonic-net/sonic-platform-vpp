import json

class VppMgr(object):
    """ The class represents vpp  configuration template in json"""
    def __init__(self, template):
        self.vpp_config = {}
        with open(template, "r") as flp:
            self.vpp_config = json.load(flp)
            print("Template {}".format(self.vpp_config))

    def get_config(self):
        return self.vpp_config

# sonic-vppcfgd package

SONIC_VPPCFGD = sonic_vppcfgd-1.0-py3-none-any.whl
$(SONIC_VPPCFGD)_SRC_PATH = $(PLATFORM_PATH)/sonic-vppcfgd
# These dependencies are only needed because they are dependencies
# of sonic-config-engine and vppcfgd explicitly calls sonic-cfggen
# as part of its unit tests.
# TODO: Refactor unit tests so that these dependencies are not needed

$(SONIC_VPPCFGD)_DEPENDS += $(SONIC_CONFIG_ENGINE_PY3) \
                            $(SONIC_YANG_MGMT_PY3) \
                            $(SONIC_YANG_MODELS_PY3)
$(SONIC_VPPCFGD)_DEBS_DEPENDS += $(LIBYANG) \
                                 $(LIBYANG_CPP) \
                                 $(LIBYANG_PY3) \
                                 $(PYTHON3_SWSSCOMMON)
$(SONIC_VPPCFGD)_PYTHON_VERSION = 3
$(SONIC_VPPCFGD)_TEST = n
SONIC_PYTHON_WHEELS += $(SONIC_VPPCFGD)

export vpp_platform_vppcfgd_py3_wheel_path="$(addprefix $(PYTHON_WHEELS_PATH)/,$(SONIC_VPPCFGD))"

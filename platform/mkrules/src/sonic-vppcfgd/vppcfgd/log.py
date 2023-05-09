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

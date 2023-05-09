import setuptools

setuptools.setup(
    name = 'sonic-vppcfgd',
    version = '1.0',
    description = 'Utility to configure vpp startup config based on vpp cfg in config_db',
    author = 'Shashidhar Patil',
    author_email = 'shaship@cisco.com',
    url = 'https://github.com/sonic-net/sonic-platform-vpp',
    packages = setuptools.find_packages(),
    entry_points = {
        'console_scripts': [
            'vppcfgd = vppcfgd.main:main',
        ]
    },
    install_requires = [
        'jinja2>=2.10',
        'pyyaml==5.4.1',
    ],
    setup_requires = [
        'pytest-runner',
        'wheel'
    ],
    tests_require = [
        'pytest',
        'pytest-cov',
        'sonic-config-engine'
    ]
)

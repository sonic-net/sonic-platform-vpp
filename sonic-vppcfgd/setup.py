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
        'pyyaml>=5.4.1',
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

#!/usr/bin/env python
# -*- coding: utf-8 -*-
__author__ = "Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>"
__copyright__ = "Copyright 2016, Cisco Systems, Inc."
__license__ = "Apache 2.0"

# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from ConcurrentHelpers import *
from time import sleep
import subprocess
import sysrepo as sr


class SysrepodDaemonTester(SysrepoTester):

    def startDaemonStep(self, log_level = sr.SR_LL_INF):
        self.process = subprocess.Popen(["sysrepod", "-d", "-l {0}".format(log_level)])
        self.report_pid(self.process.pid)
        sleep(0.1)


    def stopDaemonStep(self):
        os.kill(self.process.pid, signal.SIGTERM)
        self.process.wait()

#!/usr/bin/env python
from __future__ import print_function

__author__ = "Mislav Novakovic <mislav.novakovic@sartura.hr>"
__copyright__ = "Copyright 2018, Deutsche Telekom AG"
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

import sysrepo as sr
import sys

def test_rpc_cb(xpath, in_vals, holder, private_ctx):
    try:
        print ("\n\n ========== RPC CALLED ==========")
        out_vals = holder.allocate(3)

        if (in_vals == None or out_vals == None):
            return

        for n in range(in_vals.val_cnt()):
            print (in_vals.val(n).to_string(),end='')

        out_vals.val(0).set("/test-module:activate-software-image/status", "The image acmefw-2.3 is being installed.", sr.SR_STRING_T)
        out_vals.val(1).set("/test-module:activate-software-image/version", "2.3", sr.SR_STRING_T)
        out_vals.val(2).set("/test-module:activate-software-image/location", "/", sr.SR_STRING_T)

    except Exception as e:
        print (e)

def test_rpc_tree_cb(xpath, in_trees, holder, private_ctx):
    try:
        print ("\n ========== RPC TREE CALLED ==========\n")

        out_trees = holder.allocate(3)

        if (in_trees == None or out_trees == None):
            return

        for n in range(in_trees.tree_cnt()):
            print (in_trees.tree(n).name() + " = " + in_trees.tree(n).data().get_string())

        out_trees.tree(0).set_name("status")
        out_trees.tree(0).set("The image acmefw-2.3 is being installed.", sr.SR_STRING_T)
        out_trees.tree(1).set_name("version")
        out_trees.tree(1).set("2.3", sr.SR_STRING_T)
        out_trees.tree(2).set_name("location")
        out_trees.tree(2).set("/root/", sr.SR_STRING_T)

    except Exception as e:
        print (e)

try:
    module_name = "test-module"

    # connect to sysrepo
    conn = sr.Connection("example_application")

    # start session
    sess = sr.Session(conn)

    # subscribe for changes in running config */
    subscribe = sr.Subscribe(sess)


    print ("\n ========== SUBSCRIBE TO RPC CALL ==========")
    subscribe.rpc_subscribe("/test-module:activate-software-image", test_rpc_cb)

    in_vals = sr.Vals(2)

    in_vals.val(0).set("/test-module:activate-software-image/image-name", "acmefw-2.3", sr.SR_STRING_T)
    in_vals.val(1).set("/test-module:activate-software-image/location", "/", sr.SR_STRING_T)


    print ("\n ========== START RPC CALL ==========")
    out_vals = sess.rpc_send("/test-module:activate-software-image", in_vals)

    print ("\n ========== PRINT RETURN VALUE ==========")

    for n in range (out_vals.val_cnt()):
        print (out_vals.val(n).to_string(),end='')

    print ("\n ========== SUBSCRIBE TO RPC TREE CALL ==========\n")
    subscribe.rpc_subscribe_tree("/test-module:activate-software-image", test_rpc_tree_cb)

    in_trees = sr.Trees(1)

    in_trees.tree(0).set_name("image-name")
    in_trees.tree(0).set("acmefw-2.3", sr.SR_STRING_T)

    print ("\n ========== START RPC TREE CALL ==========\n")
    out_trees = sess.rpc_send("/test-module:activate-software-image", in_trees)

    print ("\n ========== PRINT RETURN VALUE ==========\n")
    for n in range (out_trees.tree_cnt()):
        if (out_trees.tree(n).type() == sr.SR_STRING_T):
            print (out_trees.tree(n).name() + " = " + out_trees.tree(n).data().get_string())
        if (out_trees.tree(n).type() == sr.SR_CONTAINER_T):
            print (out_trees.tree(n).name() + " = (container)")

    print ("\n ========== END PROGRAM ==========\n")

except Exception as e:
    print (e)



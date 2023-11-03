/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import jdk.test.lib.apps.LingeredApp;
import jtreg.SkippedException;

/**
 * @test
 * @bug 8318682
 * @summary Test clhsdb 'testdebuginfodecode' command
 * @requires vm.hasSA
 * @library /test/lib
 * @run main/othervm/timeout=2400 -Xmx1g -Xcomp ClhsdbTestDebugInfodDecode
 */

public class ClhsdbTestDebugInfodDecode {

    public static void main(String[] args) throws Exception {
        System.out.println("Starting ClhsdbTestDebugInfodDecode test");

        LingeredAppWithEnum theApp = null;
        try {
            ClhsdbLauncher test = new ClhsdbLauncher();

            theApp = new LingeredAppWithEnum();
            LingeredApp.startApp(theApp);
            System.out.println("Started LingeredAppWithEnum with pid " + theApp.getPid());

            List<String> cmds = List.of("testdebuginfodecode");

            // sun.jvm.hotspot.utilities.AssertionFailure is caught by the harness so it's not
            // necessary to include extra filters here.
            Map<String, List<String>> expStrMap = new HashMap<>();
            Map<String, List<String>> unExpStrMap = new HashMap<>();
            test.run(theApp.getPid(), cmds, expStrMap, unExpStrMap);
        } catch (SkippedException se) {
            throw se;
        } catch (Exception ex) {
            throw new RuntimeException("Test ERROR " + ex, ex);
        } finally {
            LingeredApp.stopApp(theApp);
        }
        System.out.println("Test PASSED");
    }
}

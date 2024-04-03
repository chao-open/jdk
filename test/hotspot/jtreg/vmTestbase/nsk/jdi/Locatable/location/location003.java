/*
 * Copyright (c) 2001, 2024, Oracle and/or its affiliates. All rights reserved.
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

package nsk.jdi.Locatable.location;

import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

import com.sun.jdi.*;
import java.util.*;
import java.io.*;

/**
 * The test for the implementation of an object of the type     <BR>
 * Locatable.                                                   <BR>
 *                                                      <BR>
 * The test checks up that results of the method        <BR>
 * <code>com.sun.jdi.Locatable.location()</code>        <BR>
 * complies with its spec when a tested method          <BR>
 * is one of ReferenceTypes.                            <BR>
 * <BR>
 * The cases for testing are as follows.                <BR>
 *                                                      <BR>
 * When a gebuggee creates an object of the following   <BR>
 * class type with methods returning ReferenceType objects:<BR>
 *                                                      <BR>
 *    class TestClass {                                 <BR>
 *              .                                       <BR>
 *              .                                       <BR>
 *        public ClassForCheck[] arraymethod () {       <BR>
 *            return cfc;                               <BR>
 *        }                                             <BR>
 *        public ClassForCheck classmethod () {         <BR>
 *            return classFC;                           <BR>
 *        }                                             <BR>
 *        public InterfaceForCheck ifacemethod () {     <BR>
 *            return iface;                             <BR>
 *        }                                             <BR>
 *   }                                                  <BR>
 * a debugger checks up that for all of the above
 * methods returning ReferenceType objects,             <BR>
 * the invocation of the method Locatable.location()    <BR>
 * returns non-null value.                              <BR>
 * <BR>
 */

public class location003 {

    //----------------------------------------------------- templete section
    static final int PASSED = 0;
    static final int FAILED = 2;
    static final int PASS_BASE = 95;

    //----------------------------------------------------- templete parameters
    static final String
    sHeader1 = "\n==> nsk/jdi/Locatable/location/location003  ",
    sHeader2 = "--> location003: ",
    sHeader3 = "##> location003: ";

    //----------------------------------------------------- main method

    public static void main (String argv[]) {
        int result = run(argv, System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }
    }

    public static int run (String argv[], PrintStream out) {
        return new location003().runThis(argv, out);
    }

     //--------------------------------------------------   log procedures

    //private static boolean verbMode = false;

    private static Log  logHandler;

    private static void log1(String message) {
        logHandler.display(sHeader1 + message);
    }
    private static void log2(String message) {
        logHandler.display(sHeader2 + message);
    }
    private static void log3(String message) {
        logHandler.complain(sHeader3 + message);
    }

    //  ************************************************    test parameters

    private String debuggeeName =
        "nsk.jdi.Locatable.location.location003a";

    String mName = "nsk.jdi.Locatable.location";

    //====================================================== test program

    static ArgumentHandler      argsHandler;
    static int                  testExitCode = PASSED;

    //------------------------------------------------------ common section

    private int runThis (String argv[], PrintStream out) {

        Debugee debuggee;

        argsHandler     = new ArgumentHandler(argv);
        logHandler      = new Log(out, argsHandler);
        Binder binder   = new Binder(argsHandler, logHandler);


        if (argsHandler.verbose()) {
            debuggee = binder.bindToDebugee(debuggeeName + " -vbs");  // *** tp
        } else {
            debuggee = binder.bindToDebugee(debuggeeName);            // *** tp
        }

        IOPipe pipe     = new IOPipe(debuggee);

        debuggee.redirectStderr(out);
        log2("location003a debuggee launched");
        debuggee.resume();

        String line = pipe.readln();
        if ((line == null) || !line.equals("ready")) {
            log3("signal received is not 'ready' but: " + line);
            return FAILED;
        } else {
            log2("'ready' recieved");
        }

        VirtualMachine vm = debuggee.VM();

    //------------------------------------------------------  testing section
        log1("      TESTING BEGINS");

        for (int i = 0; ; i++) {
        pipe.println("newcheck");
            line = pipe.readln();

            if (line.equals("checkend")) {
                log2("     : returned string is 'checkend'");
                break ;
            } else if (!line.equals("checkready")) {
                log3("ERROR: returned string is not 'checkready'");
                testExitCode = FAILED;
                break ;
            }

            log1("new check: #" + i);

            //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ variable part

           List listOfDebuggeeClasses = vm.classesByName(mName + ".location003aTestClass");
                if (listOfDebuggeeClasses.size() != 1) {
                    testExitCode = FAILED;
                    log3("ERROR: listOfDebuggeeClasses.size() != 1");
                    break ;
                }

            List     methods   = null;
            Method   m         = null;
            Location mLocation = null;

            int i2;

            for (i2 = 0; ; i2++) {

                int expresult = 0;

                log2("new check: #" + i2);

                switch (i2) {

                case 0:                 // ArrayType

                        methods = ((ReferenceType) listOfDebuggeeClasses.get(0)).
                           methodsByName("arraymethod");
                        m = (Method) methods.get(0);
                        mLocation = m.location();

                        if (mLocation == null) {
                            log3("ERROR: mLocation == null for 'arraymethod'");
                            testExitCode = FAILED;
                        }
                        break;

                case 1:                 // ClassType

                        methods = ((ReferenceType) listOfDebuggeeClasses.get(0)).
                           methodsByName("classmethod");
                        m = (Method) methods.get(0);
                        mLocation = m.location();

                        if (mLocation == null) {
                            log3("ERROR: mLocation == null for 'classmethod'");
                            testExitCode = FAILED;
                        }
                        break;

                case 2:                 // InterfaceType

                        methods = ((ReferenceType) listOfDebuggeeClasses.get(0)).
                           methodsByName("ifacemethod");
                        m = (Method) methods.get(0);
                        mLocation = m.location();

                        if (mLocation == null) {
                            log3("ERROR: mLocation == null for 'ifacemethod'");
                            testExitCode = FAILED;
                        }
                        break;


                default: expresult = 2;
                         break ;
                }

                if (expresult == 2) {
                    log2("      test cases finished");
                    break ;
                } else if (expresult == 1) {
                    log3("ERROR: expresult != true;  check # = " + i);
                    testExitCode = FAILED;
                }
            }
            //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        }
        log1("      TESTING ENDS");

    //--------------------------------------------------   test summary section
    //-------------------------------------------------    standard end section

        pipe.println("quit");
        log2("waiting for the debuggee to finish ...");
        debuggee.waitFor();

        int status = debuggee.getStatus();
        if (status != PASSED + PASS_BASE) {
            log3("debuggee returned UNEXPECTED exit status: " +
                    status + " != PASS_BASE");
            testExitCode = FAILED;
        } else {
            log2("debuggee returned expected exit status: " +
                    status + " == PASS_BASE");
        }

        if (testExitCode != PASSED) {
            logHandler.complain("TEST FAILED");
        }
        return testExitCode;
    }
}
